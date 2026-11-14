/*
 * What a JavaScript library costs (<zebkit/library.h>, docs/JS_LIBRARIES.md,
 * "Measuring"), in one fresh process: scripts/library-bench.sh runs it many
 * times and takes medians.
 *
 *   bench <bench dir> [<markdown bundle> <charts bundle>] [--options JSON] [engine switches...]
 *
 * <bench dir> is tests/bench/library: its echo/ and host/ libraries.
 * --options is every load's options (as {"backend": "page"}); the loads of the
 * pool add "instances".
 *
 * Prints one JSON object: load to ready (ms) of each library, and of a second
 * load of the same library in the process; the round trip of 2000 calls of
 * echo with 100 bytes of JSON each way (µs: median, p99); a 1 MiB part there
 * and back (ms, median of 20); a library's call of the application's
 * function (host.call(), answered at once: µs, median; built with
 * -DBENCH_HOST, for an engine that has it); resident memory (MiB)
 * per empty instance, over 10; ECharts: its first render, the next ones,
 * memory after 100 renders and after 3 s idle (from 2.5 s after the earlier
 * libraries' last call, past their own idle shrink), the first render after
 * that idle and the next, and 40 renders on 1 and on 4 instances (ms, wall).
 * The engine is driven by the program's own loop, pumped without sleeping, as
 * an application waiting on a result would be woken. A measure the engine
 * cannot take (an option it does not know) is left out.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>

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

/* The proportional set size of this process's descendants (renderers, the
 * network service): what a page's instance costs beside this process's own
 * memory (the backend "page"; nothing for "isolate"). */
static double children_pss_mib(void) {
    enum { MAX = 4096 };
    static int pids[MAX], ppids[MAX];
    int n = 0;
    DIR *proc = opendir("/proc");
    struct dirent *entry;
    while (proc && (entry = readdir(proc)) && n < MAX) {
        int pid = atoi(entry->d_name);
        char path[64], text[512];
        if (pid <= 0) continue;
        snprintf(path, sizeof path, "/proc/%d/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t got = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[got] = 0;
        const char *close = strrchr(text, ')');
        int ppid = 0;
        if (close && sscanf(close + 2, "%*c %d", &ppid) == 1) { pids[n] = pid; ppids[n++] = ppid; }
    }
    if (proc) closedir(proc);
    double kib = 0;
    for (int i = 0; i < n; ++i) {
        int up = ppids[i], hops = 0;
        while (up > 1 && up != getpid() && hops++ < 16) {
            int j = 0;
            while (j < n && pids[j] != up) ++j;
            up = j < n ? ppids[j] : 0;
        }
        if (up != getpid()) continue;
        char path[64], line[256];
        snprintf(path, sizeof path, "/proc/%d/smaps_rollup", pids[i]);
        FILE *f = fopen(path, "r");
        while (f && fgets(line, sizeof line, f))
            if (!strncmp(line, "Pss:", 4)) kib += atof(line + 4);
        if (f) fclose(f);
    }
    return kib / 1024;
}

static int told_count;
static zebkit_library_code last_code;
static size_t last_size;
static const char *options = NULL;

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

static void pump_for(double ms) {
    double end = now_us() + ms * 1e3;
    while (now_us() < end) zebkit_pump();
}

static int cmp(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The load's options: --options with @extra's members added. */
static const char *with(const char *extra) {
    static char text[4096];
    if (!extra) return options;
    if (!options || !strcmp(options, "{}")) {
        snprintf(text, sizeof text, "{%s}", extra);
    } else {
        size_t n = strlen(options);
        snprintf(text, sizeof text, "%.*s, %s}", (int)(n - 1), options, extra);
    }
    return text;
}

/* Load to ready, in ms; < 0 when the options are refused. */
static double load_with(const char *path, const char *extra, zebkit_library **out) {
    char *error = NULL;
    double start = now_us();
    zebkit_library *library = zebkit_library_load(path, with(extra), told, NULL, &error);
    if (!library) {
        if (extra) { fprintf(stderr, "bench: skipped (%s): %s\n", extra, error); return -1; }
        fprintf(stderr, "bench: %s\n", error);
        exit(1);
    }
    until(1);
    if (last_code != ZEBKIT_LIBRARY_OK) exit(1);
    *out = library;
    return (now_us() - start) / 1e3;
}

static double load_ms(const char *path, zebkit_library **out) { return load_with(path, NULL, out); }

#ifdef BENCH_HOST
/* The host library's application function: answered at once, with what it got. */
static void host_echo(zebkit_library *library, uint64_t request, const char *name, const char *args_json,
                      const zebkit_part *parts, size_t part_count, void *data) {
    (void)name; (void)data;
    zebkit_library_answer(library, request, ZEBKIT_LIBRARY_OK, args_json, parts, part_count);
}
#endif

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: bench <bench dir> [<markdown> <charts>] [--options JSON] [switches]\n");
        return 2;
    }
    char echo[4096], host_path[4096];
    snprintf(echo, sizeof echo, "%s/echo", argv[1]);
    snprintf(host_path, sizeof host_path, "%s/host", argv[1]);
    const char *markdown = argc > 3 && argv[2][0] != '-' ? argv[2] : NULL;
    const char *charts = markdown ? argv[3] : NULL;
    int first_switch = markdown ? 4 : 2;
    if (first_switch + 1 < argc && !strcmp(argv[first_switch], "--options")) {
        options = argv[first_switch + 1];
        first_switch += 2;
    }
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
    double before = rss_mib(), before_children = children_pss_mib();
    zebkit_library *ten[10];
    for (int i = 0; i < 10; ++i) load_ms(echo, &ten[i]);
    for (int i = 0; i < 10; ++i) { zebkit_library_call(ten[i], "echo", "[1]", NULL, 0, 0, told, NULL); until(1); }
    double per_instance = (rss_mib() - before) / 10;
    /* With the processes it made (a page's renderer). */
    double per_instance_all = per_instance + (children_pss_mib() - before_children) / 10;
    for (int i = 0; i < 10; ++i) zebkit_library_close(ten[i]);

    printf("{\"echoLoadMs\": %.2f, \"callMedianUs\": %.1f, \"callP99Us\": %.1f, \"partMiBMs\": %.2f, "
           "\"emptyInstanceMiB\": %.2f, \"emptyInstanceAllMiB\": %.2f", echo_load, times[CALLS / 2], times[CALLS * 99 / 100],
           parts[10], per_instance, per_instance_all);
    fflush(stdout);

    /* host.call() of a function the application answers at once: 200 to warm
     * up, then 1000. Only with an engine that has it. */
#ifdef BENCH_HOST
    {
        zebkit_library *host;
        if (load_with(host_path, "\"host\": [\"echo\"]", &host) >= 0) {
            zebkit_library_set_host(host, host_echo, NULL);
            enum { HOST_CALLS = 1000 };
            static double host_times[HOST_CALLS];
            for (int i = -200; i < HOST_CALLS; ++i) {
                double start = now_us();
                zebkit_library_call(host, "viaHost", "[\"0123456789\"]", NULL, 0, 0, told, NULL);
                until(1);
                if (i >= 0) host_times[i] = now_us() - start;
            }
            qsort(host_times, HOST_CALLS, sizeof host_times[0], cmp);
            printf(", \"hostCallMedianUs\": %.1f", host_times[HOST_CALLS / 2]);
            zebkit_library_close(host);
        }
    }
#else
    (void)host_path;
#endif

    if (markdown) {
        zebkit_library *md, *md2, *ec, *ec2;
        printf(", \"markdownLoadMs\": %.2f", load_ms(markdown, &md));
        zebkit_library_call(md, "render", "[\"# Title\\n\\nSome *text*.\"]", NULL, 0, 0, told, NULL);
        until(1);
        zebkit_library_close(md);
        for (int i = 0; i < 20; ++i) zebkit_pump();
        printf(", \"markdownLoad2Ms\": %.2f", load_ms(markdown, &md2));
        zebkit_library_close(md2);
        /* Past the others' idle shrink (2 s): what follows is ECharts' own. */
        pump_for(2500);
        double start_rss = rss_mib(), start_children = children_pss_mib();
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
        /* With the processes it made (a page's renderer). */
        printf(", \"chartsAfter100AllMiB\": %.2f", rss_mib() - start_rss + children_pss_mib() - start_children);
        pump_for(3000);
        printf(", \"chartsIdleMiB\": %.2f", rss_mib() - start_rss);
        fflush(stdout);
        /* The first render after it was idle, and the next. */
        for (int i = 0; i < 2; ++i) {
            double again = now_us();
            zebkit_library_call(ec, "renderSvg", spec, NULL, 0, 0, told, NULL);
            until(1);
            printf(i ? ", \"chartsRenderNextMs\": %.2f" : ", \"chartsRenderAfterIdleMs\": %.2f", (now_us() - again) / 1e3);
        }
        /* A second ECharts in the process: its load and first render. */
        printf(", \"chartsLoad2Ms\": %.2f", load_ms(charts, &ec2));
        first = now_us();
        zebkit_library_call(ec2, "renderSvg", spec, NULL, 0, 0, told, NULL);
        until(1);
        printf(", \"chartsFirstRender2Ms\": %.2f", (now_us() - first) / 1e3);
        zebkit_library_close(ec2);
        /* 40 renders made at once: on the one instance, then on a pool of 4. */
        double wall = now_us();
        for (int i = 0; i < 40; ++i) zebkit_library_call(ec, "renderSvg", spec, NULL, 0, 0, told, NULL);
        until(40);
        printf(", \"charts40On1Ms\": %.2f", (now_us() - wall) / 1e3);
        zebkit_library_close(ec);
        zebkit_library *pool;
        if (load_with(charts, "\"instances\": 4", &pool) >= 0) {
            for (int i = 0; i < 8; ++i) zebkit_library_call(pool, "renderSvg", spec, NULL, 0, 0, told, NULL);
            until(8);
            wall = now_us();
            for (int i = 0; i < 40; ++i) zebkit_library_call(pool, "renderSvg", spec, NULL, 0, 0, told, NULL);
            until(40);
            printf(", \"charts40On4Ms\": %.2f", (now_us() - wall) / 1e3);
            zebkit_library_close(pool);
        }
    }
    printf("}\n");
    zebkit_library_close(lib);
    for (int i = 0; i < 50; ++i) zebkit_pump();
    zebkit_shutdown();
    return 0;
}
