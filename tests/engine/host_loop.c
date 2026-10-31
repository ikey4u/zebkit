#define _DARWIN_C_SOURCE
/* A blocking host loop: wakes only for engine work or the next deadline. */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <zebkit/zebkit.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    int pipe[2];
    int64_t next;
    int notifications;
} host_state;

static int failures, evaluated, delayed, posted, closed;
static int64_t began, timer_fired;
static atomic_int monitor_active, monitor_ready, handler_changed;
static void host_signal(int number) { (void)number; }

static void *monitor_signals(void *data) {
    (void)data;
    atomic_store(&monitor_ready, 1);
    while (atomic_load(&monitor_active)) {
        struct sigaction action;
        sigaction(SIGINT, NULL, &action);
        if (action.sa_handler != host_signal)
            atomic_store(&handler_changed, 1);
        struct timespec wait = {0, 100000};
        nanosleep(&wait, NULL);
    }
    return NULL;
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void check(int ok, const char *name) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    fflush(stdout);
    failures += !ok;
}

static void wakeup(int64_t delay, void *data) {
    host_state *host = data;
    pthread_mutex_lock(&host->lock);
    int64_t due = now_ms() + delay;
    if (due < host->next)
        host->next = due;
    ++host->notifications;
    pthread_mutex_unlock(&host->lock);
    /* A full pipe already represents a wakeup; never wait for its reader. */
    (void)write(host->pipe[1], "w", 1);
}

static void from_worker(void *data) { (void)data; posted = 1; }
static void after_timer(void *data) {
    (void)data;
    timer_fired = now_ms();
    delayed = 1;
}
static void *worker(void *data) {
    (void)data;
    struct timespec wait = {0, 30000000};
    nanosleep(&wait, NULL);
    zebkit_post_task(from_worker, NULL);
    return NULL;
}
static void result(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view; (void)data;
    check(!error && json && json[0] == '4' && json[1] == '2', "page evaluation completes in an event-driven loop");
    evaluated = 1;
}
static void on_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) {
        zebkit_view_on_load(view, NULL, NULL);
        zebkit_view_evaluate(view, "6 * 7", result, NULL);
    }
}
static void on_close(zebkit_view *view, void *data) { (void)view; (void)data; ++closed; }

int main(int argc, char **argv) {
    setlocale(LC_ALL, "C");
#if defined(__APPLE__)
    pthread_setname_np("zebkit-host");
#else
    pthread_setname_np(pthread_self(), "zebkit-host");
#endif
    const int had_bus_environment = getenv("DBUS_SESSION_BUS_ADDRESS") != NULL;
    host_state host = {.lock = PTHREAD_MUTEX_INITIALIZER, .next = INT64_MAX};
    if (pipe(host.pipe))
        return 1;
    fcntl(host.pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(host.pipe[1], F_SETFL, O_NONBLOCK);
    check(zebkit_set_wakeup(wakeup, &host), "host wakeup configured before startup");
    struct sigaction action = {0};
    action.sa_handler = host_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    atomic_store(&monitor_active, 1);
    pthread_t monitor;
    pthread_create(&monitor, NULL, monitor_signals, NULL);
    while (!atomic_load(&monitor_ready)) {
        struct timespec wait = {0, 100000};
        nanosleep(&wait, NULL);
    }
    int startup = zebkit_init(argc, argv);
    atomic_store(&monitor_active, 0);
    pthread_join(monitor, NULL);
    if (startup)
        return 1;
    check(!atomic_load(&handler_changed), "signal handlers remain the host's throughout initialization");
    char thread_name[64] = {0};
    pthread_getname_np(pthread_self(), thread_name, sizeof(thread_name));
    check(strcmp(thread_name, "zebkit-host") == 0, "initialization preserves the host thread's name");
    check(strcmp(setlocale(LC_ALL, NULL), "C") == 0, "initialization preserves the host C locale");
    check(had_bus_environment || !getenv("DBUS_SESSION_BUS_ADDRESS"), "initialization preserves an unset host D-Bus environment");
    check(!zebkit_set_wakeup(NULL, NULL), "wakeup lifetime cannot be replaced during startup's lifetime");
    zebkit_view *view = zebkit_view_new("data:text/html,<title>host loop</title>", 320, 200);
    zebkit_view_on_load(view, on_load, NULL);
    zebkit_view_on_close(view, on_close, NULL);
    began = now_ms();
    zebkit_post_delayed_task(after_timer, NULL, 200);
    pthread_t thread;
    pthread_create(&thread, NULL, worker, NULL);
    int pumps = 0;
    const int64_t watchdog = began + 10000;
    while (!(evaluated && delayed && posted) && now_ms() < watchdog) {
        pthread_mutex_lock(&host.lock);
        int64_t due = host.next < watchdog ? host.next : watchdog;
        pthread_mutex_unlock(&host.lock);
        int64_t remaining = due - now_ms();
        struct pollfd fd = {.fd = host.pipe[0], .events = POLLIN};
        poll(&fd, 1, remaining > 0 ? (int)remaining : 0);
        char bytes[256];
        while (read(host.pipe[0], bytes, sizeof(bytes)) > 0) {}
        pthread_mutex_lock(&host.lock);
        int ready = host.next <= now_ms();
        if (ready)
            host.next = INT64_MAX;
        pthread_mutex_unlock(&host.lock);
        if (ready) {
            check(zebkit_pump(), "host can pump after a notification");
            ++pumps;
        }
    }
    pthread_join(thread, NULL);
    check(evaluated && posted && delayed, "page, worker and delayed work finish without periodic polling");
    check(timer_fired - began >= 190 && timer_fired - began < 3000, "delayed work wakes near its deadline");
    check(pumps < 500, "future deadlines do not make the host busy-loop");
    check(zebkit_shutdown(), "host loop engine shuts down");
    check(closed == 1, "shutdown closes the host's view");
    close(host.pipe[0]);
    close(host.pipe[1]);
    pthread_mutex_destroy(&host.lock);
    return failures ? 1 : 0;
}
