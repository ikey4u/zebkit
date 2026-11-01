/* zebkit-js: run an application written in JavaScript.
 *
 *     zebkit-js [chromium switches] app.mjs [arguments]
 *
 * app.mjs is an ES module, with zebkit.* (docs/API.md): views to
 * make, the engine's services, timers and console. The program ends when
 * the script calls zebkit.quit(), or once nothing of it is pending and no
 * view is open, as Node ends. Its exit status is 1 when the module threw. */

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

static const char *module_path;
static int script_argc;
static const char **script_argv;
static int failed;

static void finished(zebkit_view *view, const char *json, const char *error, void *data) {
    (void)view, (void)json, (void)data;
    if (error) {
        fprintf(stderr, "Uncaught %s\n", error);
        failed = 1;
    }
}

static void idle(void *data) {
    (void)data;
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    zebkit_script *script = zebkit_script_new(script_argc, script_argv);
    zebkit_script_on_idle(script, idle, NULL);
    zebkit_script_run_module(script, module_path, finished, NULL);
}

#ifdef _WIN32
/* The script's arguments in UTF-8, as the C API's strings are: argv is in the
 * system's code page, which cannot hold every name. */
static char **windows_utf8_argv(int *argc) {
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), argc);
    char **utf8 = calloc((size_t)*argc + 1, sizeof(char *));
    for (int i = 0; i < *argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        utf8[i] = malloc((size_t)size);
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, utf8[i], size, NULL, NULL);
    }
    LocalFree(wide);
    return utf8;
}
#endif

int main(int argc, char **argv) {
#ifdef _WIN32
    argv = windows_utf8_argv(&argc);
#endif
    /* The first argument that is not a switch is the module; the rest are
     * the script's (zebkit.args). */
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--", 2) != 0) {
            module_path = argv[i];
            script_argc = argc - i - 1;
            script_argv = (const char **)argv + i + 1;
            break;
        }
    }
    if (!module_path) {
        fprintf(stderr, "usage: zebkit-js [switches] app.mjs [arguments]\n");
        return 2;
    }
    int code = zebkit_main(argc, argv, ready, NULL);
    return failed ? 1 : code;
}
