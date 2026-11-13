/* An application keeps its loop before, during and after the engine's life. */
#define _POSIX_C_SOURCE 200809L
#include "harness.h"
#include <zebkit/zebkit.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int evaluated, delayed, posted, closed;
static zebkit_view *view;
static void host_signal(int number) { (void)number; }

static void task(void *data) {
    (void)data;
    ++posted;
}

static void *worker(void *data) {
    (void)data;
    /* These calls must be rejected without touching Chromium's thread state. */
    int ok = !zebkit_pump() && !zebkit_shutdown();
    zebkit_post_task(task, NULL);
    return (void *)(uintptr_t)ok;
}

static void after_delay(void *data) {
    (void)data;
    delayed = 1;
}

static void result(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v; (void)data;
    check(!error && json && strcmp(json, "42") == 0, "page executes in the host's loop", NULL);
    check(!zebkit_pump() && !zebkit_shutdown(), "nested lifecycle calls are rejected", NULL);
    evaluated = 1;
}

static void loaded(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) {
        zebkit_view_on_load(v, NULL, NULL);
        zebkit_view_evaluate(v, "6 * 7", result, NULL);
    }
}

static void on_close(zebkit_view *v, void *data) {
    (void)v; (void)data;
    ++closed;
    check(!zebkit_shutdown(), "shutdown cannot reenter through a close callback", NULL);
}

int main(int argc, char **argv) {
    struct sigaction action = {0}, actual = {0};
    action.sa_handler = host_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigset_t blocked, previous, current;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    check(!zebkit_pump() && !zebkit_shutdown(), "calls before initialization are rejected", NULL);
    int code = zebkit_init(argc, argv);
    check(code == 0, "initialization returns to the application", NULL);
    if (code)
        return 1;
    sigaction(SIGINT, NULL, &actual);
    pthread_sigmask(SIG_SETMASK, NULL, &current);
    check(actual.sa_handler == host_signal && sigismember(&current, SIGUSR1),
          "initialization preserves the host's signal handler and mask", NULL);
    check(zebkit_init(argc, argv) != 0, "duplicate initialization is rejected", NULL);
    check(zebkit_main(argc, argv, NULL, NULL) != 0, "the blocking entry cannot be mixed with init", NULL);
    view = zebkit_view_new("about:blank", 320, 200);
    zebkit_view_on_load(view, loaded, NULL);
    zebkit_view_on_close(view, on_close, NULL);
    zebkit_post_delayed_task(after_delay, NULL, 30);
    pthread_t thread;
    void *thread_ok = NULL;
    pthread_create(&thread, NULL, worker, NULL);
    pthread_join(thread, &thread_ok);
    check((uintptr_t)thread_ok == 1, "lifecycle calls from other threads are rejected", NULL);
    struct timespec pause = {0, 1000000};
    for (int i = 0; i < 15000 && !(evaluated && delayed && posted); ++i) {
        if (!zebkit_pump()) {
            check(0, "pump accepts the initializing thread", NULL);
            break;
        }
        nanosleep(&pause, NULL);
    }
    check(evaluated && delayed && posted == 1, "page, timer and cross-thread tasks complete", NULL);
    check(zebkit_shutdown(), "shutdown returns to the application's loop", NULL);
    sigaction(SIGINT, NULL, &actual);
    pthread_sigmask(SIG_SETMASK, NULL, &current);
    check(actual.sa_handler == host_signal && sigismember(&current, SIGUSR1),
          "shutdown leaves the host's signal handler and mask intact", NULL);
    check(closed == 1, "shutdown closes live views exactly once", NULL);
    check(!zebkit_pump() && !zebkit_shutdown(), "calls after shutdown are rejected", NULL);
    check(zebkit_init(argc, argv) != 0, "Chromium cannot be restarted in the same process", NULL);
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return summary();
}
