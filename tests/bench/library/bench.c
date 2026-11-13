/*
 * What a JavaScript library costs (<zebkit/library.h>, docs/JS_LIBRARIES.md,
 * "Measuring"), in one fresh process: scripts/library-bench.sh runs it many
 * times and takes medians.
 *
 *   bench <echo library> [<markdown bundle> <charts bundle>] [engine switches...]
 *
 * Prints one JSON object: load to ready (ms) of each library; the round
 * trip of 2000 calls of echo with 100 bytes of JSON each way (µs: median,
 * p99); a 1 MiB part there and back (ms, median of 20); resident memory
 * (MiB) per empty instance, over 10, and of ECharts after 100 renders.
 * The engine is driven by the program's own loop, pumped without sleeping,
 * as an application waiting on a result would be woken.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zebkit/library.h>

static double now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e6 + t.tv_nsec / 1e3;
}

static double rss_mib(void) {
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    double kib = 0;
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, "VmRSS:", 6)) kib = atof(line + 6);
    if (f) fclose(f);
    return kib / 1024;
}

static int told_count;
static zebkit_library_code last_code;
static size_t last_size;

static void told(zebkit_library *library, zebkit_library_code code, const char *json, const zebkit_part *parts,
                 size_t count, void *data) {
    (void)library; (void)data;
    last_code = code;
    last_size = count ? parts[0].size : strlen(json);
    if (code != ZEBKIT_LIBRARY_OK) fprintf(stderr, "bench: code %d: %s\n", code, json);
    ++told_count;
}

/* Pumps until @n outcomes more have been told. */
static void until(int n) {
    int goal = told_count + n;
    double end = now_us() + 60e6;
    while (told_count < goal && now_us() < end) zebkit_pump();
    if (told_count < goal) { fprintf(stderr, "bench: no answer in 60 s\n"); exit(1); }
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double load_ms(const char *path, zebkit_library **out) {
    char *error = NULL;
    double start = now_us();
    zebkit_library *library = zebkit_library_load(path, NULL, told, NULL, &error);
    if (!library) { fprintf(stderr, "bench: %s\n", error); exit(1); }
    until(1);
    if (last_code != ZEBKIT_LIBRARY_OK) exit(1);
    *out = library;
    return (now_us() - start) / 1e3;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bench <echo library> [<markdown> <charts>] [switches]\n"); return 2; }
    const char *echo = argv[1];
    const char *markdown = argc > 3 && argv[2][0] != '-' ? argv[2] : NULL;
    const char *charts = markdown ? argv[3] : NULL;
    int first_switch = markdown ? 4 : 2;
    char *engine_argv[64];
    int engine_argc = 0;
    engine_argv[engine_argc++] = argv[0];
    for (int i = first_switch; i < argc && engine_argc < 63; ++i) engine_argv[engine_argc++] = argv[i];
    engine_argv[engine_argc] = NULL;
    if (zebkit_init(engine_argc, engine_argv) != 0) return 1;
    for (int i = 0; i < 50; ++i) zebkit_pump();

    zebkit_library *lib;
    double echo_load = load_ms(echo, &lib);

    /* 100 bytes of JSON each way. */
    const char *args = "[\"0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567\"]";
    enum { WARM = 200, CALLS = 2000 };
    static double times[CALLS];
    for (int i = 0; i < WARM; ++i) { zebkit_library_call(lib, "echo", args, NULL, 0, 0, told, NULL); until(1); }
    for (int i = 0; i < CALLS; ++i) {
        double start = now_us();
        zebkit_library_call(lib, "echo", args, NULL, 0, 0, told, NULL);
        until(1);
        times[i] = now_us() - start;
    }
    qsort(times, CALLS, sizeof times[0], cmp);

    /* A 1 MiB part there and back. */
    static uint8_t mib[1 << 20];
    for (size_t i = 0; i < sizeof mib; ++i) mib[i] = (uint8_t)i;
    zebkit_part part = {mib, sizeof mib};
    double parts[20];
    for (int i = 0; i < 20; ++i) {
        double start = now_us();
        zebkit_library_call(lib, "bytes", "[{\"$part\":0}]", &part, 1, 0, told, NULL);
        until(1);
        parts[i] = (now_us() - start) / 1e3;
        if (last_size != sizeof mib) { fprintf(stderr, "bench: the part came back as %zu bytes\n", last_size); return 1; }
    }
    qsort(parts, 20, sizeof parts[0], cmp);

    /* Memory per empty instance, over 10. */
    for (int i = 0; i < 20; ++i) zebkit_pump();
    double before = rss_mib();
    zebkit_library *ten[10];
    for (int i = 0; i < 10; ++i) load_ms(echo, &ten[i]);
    for (int i = 0; i < 10; ++i) { zebkit_library_call(ten[i], "echo", "[1]", NULL, 0, 0, told, NULL); until(1); }
    double per_instance = (rss_mib() - before) / 10;
    for (int i = 0; i < 10; ++i) zebkit_library_close(ten[i]);

    printf("{\"echoLoadMs\": %.2f, \"callMedianUs\": %.1f, \"callP99Us\": %.1f, \"partMiBMs\": %.2f, "
           "\"emptyInstanceMiB\": %.2f", echo_load, times[CALLS / 2], times[CALLS * 99 / 100], parts[10], per_instance);
    if (markdown) {
        zebkit_library *md, *ec;
        printf(", \"markdownLoadMs\": %.2f", load_ms(markdown, &md));
        zebkit_library_call(md, "render", "[\"# Title\\n\\nSome *text*.\"]", NULL, 0, 0, told, NULL);
        until(1);
        double start_rss = rss_mib();
        printf(", \"chartsLoadMs\": %.2f", load_ms(charts, &ec));
        const char *spec = "[{\"width\":600,\"height\":400,\"option\":{\"xAxis\":{\"type\":\"category\",\"data\":"
                           "[\"a\",\"b\",\"c\",\"d\"]},\"yAxis\":{},\"series\":[{\"type\":\"bar\",\"data\":[5,20,36,10]},"
                           "{\"type\":\"line\",\"data\":[8,12,30,15]}]}}]";
        double first = now_us();
        zebkit_library_call(ec, "renderSvg", spec, NULL, 0, 0, told, NULL);
        until(1);
        double first_ms = (now_us() - first) / 1e3;
        double renders = now_us();
        for (int i = 0; i < 100; ++i) { zebkit_library_call(ec, "renderSvg", spec, NULL, 0, 0, told, NULL); until(1); }
        printf(", \"chartsFirstRenderMs\": %.2f, \"chartsRenderMs\": %.2f, \"chartsAfter100MiB\": %.2f", first_ms,
               (now_us() - renders) / 1e3 / 100, rss_mib() - start_rss);
        zebkit_library_close(md);
        zebkit_library_close(ec);
    }
    printf("}\n");
    zebkit_library_close(lib);
    for (int i = 0; i < 50; ++i) zebkit_pump();
    zebkit_shutdown();
    return 0;
}
