#define _POSIX_C_SOURCE 200809L
#include "harness.h"
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static GMainLoop *loop;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t finished = PTHREAD_COND_INITIALIZER;
static gint64 deadline = G_MAXINT64;
static int queued, complete, evaluated, delayed, posted;
static atomic_int pumps;
static guint timer;
static gboolean dispatch(void *data);

static void wake(int64_t delay, void *data) {
    (void)data;
    pthread_mutex_lock(&lock);
    gint64 due = g_get_monotonic_time() + delay * 1000;
    if (due < deadline) deadline = due;
    if (!queued) {
        queued = 1;
        g_idle_add_full(G_PRIORITY_DEFAULT, dispatch, NULL, NULL);
    }
    pthread_mutex_unlock(&lock);
}
static gboolean dispatch(void *data) {
    (void)data;
    pthread_mutex_lock(&lock);
    queued = 0;
    gint64 due = deadline;
    if (due <= g_get_monotonic_time()) deadline = G_MAXINT64;
    pthread_mutex_unlock(&lock);
    if (timer) { g_source_remove(timer); timer = 0; }
    if (due == G_MAXINT64) return G_SOURCE_REMOVE;
    gint64 remaining = due - g_get_monotonic_time();
    if (remaining > 0) timer = g_timeout_add((remaining + 999) / 1000, dispatch, NULL);
    else { ++pumps; zebkit_pump(); }
    return G_SOURCE_REMOVE;
}
static void maybe_done(void) {
    if (evaluated && delayed && posted) g_main_loop_quit(loop);
}
static void result(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view; (void)data;
    evaluated = !error && json && !strcmp(json, "42");
    maybe_done();
}
static void loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)url; (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) zebkit_view_evaluate(view, "6 * 7", result, NULL);
}
static void delayed_task(void *data) { (void)data; delayed = 1; maybe_done(); }
static void posted_task(void *data) { (void)data; posted = 1; maybe_done(); }
static void *post(void *data) { (void)data; zebkit_post_task(posted_task, NULL); return NULL; }
static void *watchdog(void *data) {
    (void)data;
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += 12;
    pthread_mutex_lock(&lock);
    while (!complete) {
        if (pthread_cond_timedwait(&finished, &lock, &limit) && !complete) {
            fprintf(stderr, "FAIL GLib host did not return from pumping (pumps=%d)\n", pumps);
            _Exit(1);
        }
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}
int main(int argc, char **argv) {
    pthread_t guard, worker;
    loop = g_main_loop_new(NULL, FALSE);
    pthread_create(&guard, NULL, watchdog, NULL);
    if (!zebkit_set_wakeup(wake, NULL) || zebkit_init(argc, argv)) return 1;
    zebkit_view *view = zebkit_view_new(NULL, 320, 240);
    zebkit_view_on_load(view, loaded, NULL);
    zebkit_view_load_url(view, "data:text/html,<title>GLib host</title>");
    zebkit_post_delayed_task(delayed_task, NULL, 120);
    pthread_create(&worker, NULL, post, NULL);
    pthread_join(worker, NULL);
    wake(0, NULL);
    g_main_loop_run(loop);
    if (timer) g_source_remove(timer);
    int stopped = zebkit_shutdown();
    pthread_mutex_lock(&lock);
    complete = 1;
    pthread_cond_signal(&finished);
    pthread_mutex_unlock(&lock);
    pthread_join(guard, NULL);
    g_main_loop_unref(loop);
    check(evaluated && delayed && posted && pumps < 500 && stopped, "GLib idle host",
          "page=%d delayed=%d posted=%d pumps=%d shutdown=%d", evaluated, delayed, posted, pumps, stopped);
    return summary();
}
