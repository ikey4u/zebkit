/* zebkit-js: run an application written in JavaScript.
 *
 *     zebkit-js [chromium switches] app.mjs [arguments]
 *
 * app.mjs is an ES module, with zebkit.* (docs/blink/API.md): views to
 * make, the engine's services, timers and console. The program ends when
 * the script calls zebkit.quit(), or once nothing of it is pending and no
 * view is open, as Node ends. Its exit status is 1 when the module threw. */

#include <stdio.h>
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

int main(int argc, char **argv) {
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
