#define _DARWIN_C_SOURCE
/* Strict startup validation is repeatable and precedes profile/key/log IO. */
#define _POSIX_C_SOURCE 200809L
#include <zebkit/zebkit.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures, evaluated;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    failures += !ok;
}
static void result(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view; (void)data;
    check(!error && json && strcmp(json, "\"Zebkit Startup Settings\"") == 0,
          "a page sees the API's user agent");
    evaluated = 1;
}

int main(int argc, char **argv) {
    const char *invalid[] = {
        "[]", "{", "{\"unknown\":true}", "{\"headless\":\"yes\"}",
        "{\"profileDirectory\":\"relative\"}", "{\"gpu\":\"fast\"}",
        "{\"logging\":{\"destination\":\"none\",\"path\":\"/tmp/no.log\"}}",
        "{\"logging\":{\"destination\":\"file\"}}",
        "{\"logging\":{\"destination\":\"stderr\",\"level\":8}}",
        "{\"cookies\":{\"storage\":\"unknown\"}}",
        "{\"cookies\":{\"storage\":\"os\",\"keyDirectory\":\"/tmp/key\"}}",
        "{\"userAgent\":\"bad\\u0000value\"}",
        "{\"applicationIntegration\":1}", "{\"sandbox\":null}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char *error = NULL;
        int code = zebkit_init_with_settings(argc, argv, invalid[i], &error);
        check(code != 0 && error && *error, "invalid settings fail with an explanation without consuming startup");
        zebkit_free(error);
    }
#ifdef _WIN32
    char temporary[MAX_PATH + 32];
    snprintf(temporary, sizeof(temporary), "%s\\zebkit-settings-XXXXXX", getenv("TEMP"));
    /* These paths go in JSON. */
    for (char *c = temporary; *c; ++c)
        if (*c == '\\')
            *c = '/';
#else
    char temporary[] = "/tmp/zebkit-settings-XXXXXX";
#endif
    const char *root = getenv("ZEBKIT_TEST_SETTINGS_DIR");
    if (!root)
        root = mkdtemp(temporary);
    if (!root)
        return 1;
    char profile[PATH_MAX], keys[PATH_MAX], config[PATH_MAX * 3];
    snprintf(profile, sizeof(profile), "%s/profile", root);
    snprintf(keys, sizeof(keys), "%s/keys", root);
    snprintf(config, sizeof(config),
             "{\"profileDirectory\":\"%s\",\"cookies\":{\"storage\":\"file\",\"keyDirectory\":\"%s\"},\"bad\":true}", profile, keys);
    char *error = NULL;
    check(zebkit_init_with_settings(argc, argv, config, &error) != 0,
          "a late unknown field rejects the whole configuration");
    zebkit_free(error);
    check(access(profile, F_OK) != 0 && access(keys, F_OK) != 0,
          "invalid settings create neither the profile nor the key directory");
    snprintf(config, sizeof(config),
             "{\"profileDirectory\":\"%s\",\"userAgent\":\"Zebkit Startup Settings\","
             "\"cookies\":{\"storage\":\"file\",\"keyDirectory\":\"%s\",\"thirdParty\":false},"
             "\"logging\":{\"destination\":\"none\"},\"gpu\":\"disabled\","
             "\"headless\":true,\"sandbox\":false,\"applicationIntegration\":false}", profile, keys);
    error = NULL;
    int code = zebkit_init_with_settings(argc, argv, config, &error);
    check(code == 0 && !error, "valid settings start the engine after validation failures");
    zebkit_free(error);
    if (code)
        return 1;
    check(zebkit_is_headless(), "headless is selected through the API");
    check(access(profile, F_OK) == 0 && access(keys, F_OK) == 0,
          "profile and file-cookie key locations come from the API");
    zebkit_view *view = zebkit_view_new(NULL, 320, 200);
    zebkit_view_evaluate(view, "navigator.userAgent", result, NULL);
    struct timespec wait = {0, 1000000};
    for (int i = 0; i < 10000 && !evaluated; ++i) {
        zebkit_pump();
        nanosleep(&wait, NULL);
    }
    check(evaluated, "the configured engine drives a real page");
    check(zebkit_shutdown(), "configured startup shuts down normally");
    return failures ? 1 : 0;
}
