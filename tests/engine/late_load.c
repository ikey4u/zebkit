/* No link to zebkit: all host threads exist before dlopen. */
#define _POSIX_C_SOURCE 200809L
#include <zebkit/zebkit.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define API(name) static __typeof__(&zebkit_##name) api_##name
API(init_with_settings); API(pump); API(shutdown); API(post_task);
API(view_new); API(view_load_url); API(view_on_load); API(view_evaluate); API(free);
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready, loaded, started, finished, worker_owner;
static int engine_argc;
static char **engine_argv;
static atomic_int failures;
static int evaluated, evaluating, posted;
static _Thread_local int marker;

static void fail(const char *message) {
    fprintf(stderr, "FAIL %s\n", message);
    atomic_fetch_add(&failures, 1);
}
static void task(void *data) { (void)data; ++posted; }
static void result(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view; (void)data;
    if (error || !json || strcmp(json, "42")) fail("late-loaded page evaluation");
    evaluated = 1;
}
static void page_loaded(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    (void)data;
    if (!evaluating && event == ZEBKIT_LOAD_FINISHED && !strncmp(url, "data:", 5)) {
        evaluating = 1;
        api_view_evaluate(view, "21*2", result, NULL);
    }
}
static double now(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec / 1e9;
}
static void owner(void) {
    char *error = NULL;
    int code = api_init_with_settings(engine_argc, engine_argv, "{\"logging\":{\"destination\":\"none\"}}", &error);
    if (code) fail(error ? error : "late-loaded initialization");
    api_free(error);
    pthread_mutex_lock(&mutex);
    started = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    if (!code) {
        zebkit_view *view = api_view_new(NULL, 320, 200);
        if (!view) fail("late-loaded view creation");
        else {
            api_view_on_load(view, page_loaded, NULL);
            api_view_load_url(view, "data:text/html,late-loaded");
        }
        double until = now() + 15;
        struct timespec pause = {0, 1000000};
        while ((!evaluated || posted < (worker_owner ? 3 : 4)) && now() < until) {
            if (!api_pump()) { fail("late-loaded pump"); break; }
            nanosleep(&pause, NULL);
        }
        if (!evaluated || posted != (worker_owner ? 3 : 4)) fail("pre-existing threads complete engine work");
        if (!api_shutdown()) fail("late-loaded shutdown");
    }
    pthread_mutex_lock(&mutex);
    finished = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
}
static void *worker(void *data) {
    int index = (int)(intptr_t)data;
    marker = index + 100;
    pthread_mutex_lock(&mutex);
    ++ready;
    pthread_cond_broadcast(&changed);
    while (!loaded) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
    if (worker_owner && index == 0) owner();
    else {
        pthread_mutex_lock(&mutex);
        while (!started) pthread_cond_wait(&changed, &mutex);
        pthread_mutex_unlock(&mutex);
        api_post_task(task, data);
        pthread_mutex_lock(&mutex);
        while (!finished) pthread_cond_wait(&changed, &mutex);
        pthread_mutex_unlock(&mutex);
    }
    if (marker != index + 100) fail("host thread-local data preserved");
    return NULL;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    worker_owner = !strcmp(argv[2], "worker");
    engine_argc = argc - 2;
    engine_argv = calloc((size_t)engine_argc + 1, sizeof(char *));
    if (!engine_argv) return 2;
    engine_argv[0] = argv[0];
    for (int i = 3; i < argc; ++i) engine_argv[i - 2] = argv[i];
    pthread_t threads[4];
    for (intptr_t i = 0; i < 4; ++i) if (pthread_create(&threads[i], NULL, worker, (void *)i)) return 2;
    pthread_mutex_lock(&mutex);
    while (ready != 4) pthread_cond_wait(&changed, &mutex);
    pthread_mutex_unlock(&mutex);
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define LOAD(name) do { *(void **)(&api_##name) = dlsym(library, "zebkit_" #name); if (!api_##name) return 2; } while (0)
    LOAD(init_with_settings); LOAD(pump); LOAD(shutdown); LOAD(post_task);
    LOAD(view_new); LOAD(view_load_url); LOAD(view_on_load); LOAD(view_evaluate); LOAD(free);
    pthread_mutex_lock(&mutex);
    loaded = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&mutex);
    if (!worker_owner) owner();
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    free(engine_argv);
    if (atomic_load(&failures)) return 1;
    printf("ok C late load owner=%s: pre-existing threads, page, posted tasks and shutdown\n", argv[2]);
    /* Chromium is kept loaded until process exit; unloading is not tested. */
    return 0;
}
