/* What the C engine tests share, as tests/engine/harness.py is the Python
 * tests': their checks, the summary line with the exit code, waiting with the
 * engine pumped, evaluating in a view, the page server's address, and the
 * activation a page's gated call needs. Header only, C or C++: include it
 * first (it asks for POSIX's names on Linux), before any system header.
 *
 *     #include "harness.h"
 *     check(ok, "what", "detail %d", n);   detail is printf's; NULL for none
 *     check_text(ok, "what", text);        detail as it is, or NULL
 *     WAIT(done, 15);                      the engine pumped until done, or 15 s
 *     return summary();
 *
 * The lines a test prints:
 *     ok   <what>             a check passed
 *     FAIL <what>: <detail>   a check failed
 *     skip <what>: <why>      a part not run here (counted, never a pass)
 *     narrowed: <what>        a ZEBKIT_TEST_* knob made the test do less
 *     N skipped: ...          before the summary, when something was skipped
 *     N of M checks passed    the summary, last
 *
 * summary() is the exit code: 0 only when there were checks and none failed.
 * Under ENGINE_TEST_STRICT=1 a skip or a narrowed run fails too, unless a line
 * of ENGINE_TEST_EXPECTED_SKIPS (extended regular expressions) matches
 * "<test>: <what>: <why>" (or "<test>: narrowed: ..."), <test> being
 * ENGINE_TEST_NAME, which scripts/engine-test.sh sets; on Windows, which has no
 * <regex.h>, every such skip fails a strict run.
 */
#ifndef ZEBKIT_TESTS_HARNESS_H
#define ZEBKIT_TESTS_HARNESS_H

#if defined(__linux__) && !defined(_POSIX_C_SOURCE) && !defined(_GNU_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <regex.h>
#endif
#include <zebkit/zebkit.h>

#define HARNESS_UNUSED __attribute__((unused))

/* A page's user activation reaches the browser on a pipe of its own, after
 * the evaluation that gave it; a gated request that comes first is refused
 * there. ACTIVATED("script") is a script that runs script once it has: for
 * every activation-gated call (fullscreen, pickers, device and display
 * requests, locks). */
#define ACTIVATION_DELAY_MS 200
#define ACTIVATED(script) "new Promise(r => setTimeout(r, 200)).then(() => " script ")"
/* How long a negative check waits for something that must not happen. */
#define SETTLE_SECONDS 0.5
/* How often WAIT pumps and looks. */
#define POLL_MS 5

static int harness_checks, harness_failures, harness_skips, harness_refused;
static char harness_skipped[2048];

/* Whether ENGINE_TEST_EXPECTED_SKIPS accepts entry, "<test>: <what>: <why>". */
static HARNESS_UNUSED int harness_expected(const char *entry) {
#ifdef _WIN32
    (void)entry;
    return 0;
#else
    const char *lines = getenv("ENGINE_TEST_EXPECTED_SKIPS");
    if (!lines)
        return 0;
    while (*lines) {
        const char *end = strchr(lines, '\n');
        size_t size = end ? (size_t)(end - lines) : strlen(lines);
        char pattern[1024];
        if (size && size < sizeof pattern) {
            memcpy(pattern, lines, size);
            pattern[size] = 0;
            regex_t compiled;
            if (!regcomp(&compiled, pattern, REG_EXTENDED | REG_NOSUB)) {
                int matched = !regexec(&compiled, entry, 0, NULL, 0);
                regfree(&compiled);
                if (matched)
                    return 1;
            }
        }
        lines += size + (end ? 1 : 0);
    }
    return 0;
#endif
}

static HARNESS_UNUSED const char *harness_test_name(void) {
    const char *name = getenv("ENGINE_TEST_NAME");
    return name && *name ? name : "c";
}

static HARNESS_UNUSED int harness_strict(void) {
    const char *strict = getenv("ENGINE_TEST_STRICT");
    return strict && !strcmp(strict, "1");
}

static HARNESS_UNUSED void harness_vcheck(int ok, const char *what, const char *format, va_list args) {
    ++harness_checks;
    if (!ok)
        ++harness_failures;
    char detail[1024] = "";
    if (format)
        vsnprintf(detail, sizeof detail, format, args);
    printf("%s%s%s%s\n", ok ? "ok   " : "FAIL ", what, *detail ? ": " : "", detail);
    fflush(stdout);
}

/* One check: an "ok   " or "FAIL " line, with printf's detail unless format
 * is NULL; returns ok. */
static HARNESS_UNUSED int check(int ok, const char *what, const char *format, ...) {
    va_list args;
    va_start(args, format);
    harness_vcheck(ok, what, format, args);
    va_end(args);
    return ok;
}

/* check(), its detail a text as it is (NULL for none). */
static HARNESS_UNUSED int check_text(int ok, const char *what, const char *detail) {
    return detail ? check(ok, what, "%s", detail) : check(ok, what, NULL);
}

/* A part not run on this machine: a "skip " line, never a pass. */
static HARNESS_UNUSED void skip(const char *what, const char *why) {
    printf("skip %s: %s\n", what, why);
    fflush(stdout);
    ++harness_skips;
    size_t used = strlen(harness_skipped);
    snprintf(harness_skipped + used, sizeof harness_skipped - used, "%s%s", used ? "; " : "", what);
    char entry[1024];
    snprintf(entry, sizeof entry, "%s: %s: %s", harness_test_name(), what, why);
    if (harness_strict() && !harness_expected(entry))
        ++harness_refused;
}

/* ZEBKIT_TEST_<name>, a debugging option that narrows the test: a
 * "narrowed:" line when it is set. */
static HARNESS_UNUSED const char *knob(const char *name) {
    char variable[128];
    snprintf(variable, sizeof variable, "ZEBKIT_TEST_%s", name);
    const char *value = getenv(variable);
    if (!value || !*value)
        return NULL;
    printf("narrowed: %s=%s\n", variable, value);
    fflush(stdout);
    char entry[1024];
    snprintf(entry, sizeof entry, "%s: narrowed: %s=%s", harness_test_name(), variable, value);
    if (harness_strict() && !harness_expected(entry))
        ++harness_refused;
    return value;
}

/* The summary line, last; the exit code. */
static HARNESS_UNUSED int summary(void) {
    if (harness_skips)
        printf("%d skipped: %s\n", harness_skips, harness_skipped);
    printf("%d of %d checks passed\n", harness_checks - harness_failures, harness_checks);
    fflush(stdout);
    return harness_failures || harness_refused || !harness_checks ? 1 : 0;
}

/* ZEBKIT_TEST_BASE, http://127.0.0.1:<port> serving tests/engine/pages, which
 * scripts/engine-test.sh starts on a port the system gives it: without it the
 * test cannot run, and says so (exit 2). */
static HARNESS_UNUSED const char *test_base(void) {
    const char *base = getenv("ZEBKIT_TEST_BASE");
    if (!base || !*base) {
        fprintf(stderr, "set ZEBKIT_TEST_BASE: http://127.0.0.1:<port>, serving tests/engine/pages\n");
        exit(2);
    }
    return base;
}

/* Seconds on a clock that only goes forward (C11's, real time, where there is
 * no other). */
static HARNESS_UNUSED double harness_now(void) {
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#elif defined(CLOCK_MONOTONIC)
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
#else
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return now.tv_sec + now.tv_nsec / 1e9;
#endif
}

static HARNESS_UNUSED void harness_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec pause = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
#endif
}

/* One turn of WAIT: the engine pumped, a pause; whether to go on (before end,
 * and the engine still running). */
static HARNESS_UNUSED int harness_pump_step(double end) {
    if (!zebkit_pump())
        return 0;
    harness_sleep_ms(POLL_MS);
    return harness_now() < end;
}

/* The engine pumped until condition is true or seconds have passed, on the
 * thread that started it (zebkit_init); look at condition again after it. */
#define WAIT(condition, seconds) \
    for (double harness_end_ = harness_now() + (seconds); !(condition) && harness_pump_step(harness_end_);) {}

/* The engine run for a while: for negative checks only. */
static HARNESS_UNUSED void pump_for(double seconds) {
    double end = harness_now() + seconds;
    while (harness_pump_step(end)) {}
}

/* What harness_evaluate() waits for. */
struct harness_evaluation { int done; char *json, *error; };

static HARNESS_UNUSED char *harness_copy(const char *text) {
    if (!text)
        return NULL;
    size_t size = strlen(text) + 1;
    char *copy = (char *)malloc(size);
    if (copy)
        memcpy(copy, text, size);
    return copy;
}

static HARNESS_UNUSED void harness_evaluated(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view;
    struct harness_evaluation *evaluation = (struct harness_evaluation *)data;
    evaluation->json = harness_copy(json);
    evaluation->error = harness_copy(error);
    evaluation->done = 1;
}

/* script's value in view as JSON, the engine pumped until it came (for tests
 * that pump, not zebkit_main's); NULL when it threw (its message in *error,
 * when error is not NULL) or no answer came within seconds ("timed out").
 * Free what it returns, and *error, with free(). */
static HARNESS_UNUSED char *harness_evaluate(zebkit_view *view, const char *script, double seconds, char **error) {
    struct harness_evaluation *evaluation = (struct harness_evaluation *)calloc(1, sizeof *evaluation);
    zebkit_view_evaluate(view, script, harness_evaluated, evaluation);
    WAIT(evaluation->done, seconds);
    char *json = NULL;
    if (!evaluation->done) {
        /* Its answer may still come: the record stays, a small leak. */
        if (error)
            *error = harness_copy("timed out");
        return NULL;
    }
    json = evaluation->json;
    if (error)
        *error = evaluation->error;
    else
        free(evaluation->error);
    free(evaluation);
    return json;
}

#endif
