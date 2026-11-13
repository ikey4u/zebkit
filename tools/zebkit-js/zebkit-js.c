/* zebkit-js: run an application written in JavaScript.
 *
 *     zebkit-js [chromium switches] app.mjs [arguments]
 *
 * app.mjs is an ES module, with zebkit.* (docs/API.md): views to
 * make, the engine's services, timers and console. The program ends when
 * the script calls zebkit.quit(), or once nothing of it is pending and no
 * view is open, as Node ends. Its exit status is 1 when the module threw,
 * and when an error went uncaught later: thrown from a callback (a timer, an
 * event or hook handler) or a promise rejected with no handler. Such an error
 * is printed ("Uncaught ...") and the program runs on to its end: the engine
 * is not stopped inside a callback. A URL scheme handler's error is answered
 * (500) and printed, and does not fail the program, as a server's.
 *
 * Its own switches, before the module: --zebkit-cookie-key-file=PATH (below);
 * the engine's --zebkit-scheme=..., --zebkit-cookie-storage=os|file and the
 * other --zebkit-... switches (docs/API.md) and Chromium's go to the engine.
 *
 * Built with ZEBKIT_WIN_SANDBOX on Windows (and zebkit_sandbox.lib, as the
 * release archive's is), it is the engine's sandboxed children too
 * (<zebkit/win_sandbox.h>); without, it runs there only with --no-sandbox. */

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>
#if defined(_WIN32) && defined(ZEBKIT_WIN_SANDBOX)
#include <zebkit/win_sandbox.h>
#endif

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

/* Opens a file named in UTF-8 for reading: on Windows through the wide
 * path, as fopen() reads the name in the system's code page. */
static FILE *open_utf8(const char *path) {
#ifdef _WIN32
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (size <= 0)
        return NULL;
    wchar_t *wide = malloc(sizeof(wchar_t) * (size_t)size);
    if (!wide)
        return NULL;
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, size);
    FILE *file = NULL;
    if (_wfopen_s(&file, wide, L"rb") != 0)
        file = NULL;
    free(wide);
    return file;
#else
    return fopen(path, "rb");
#endif
}

/* Clears a secret, in a way the compiler keeps. */
static void wipe(void *secret, size_t size) {
    volatile unsigned char *bytes = secret;
    while (size--)
        *bytes++ = 0;
}

int main(int argc, char **argv) {
#if defined(_WIN32) && defined(ZEBKIT_WIN_SANDBOX)
    int child_code;
    if (zebkit_win_startup(&child_code))
        return child_code;
#endif
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
        fprintf(stderr,
                "usage: zebkit-js [switches] app.mjs [arguments]\n"
                "  switches: --zebkit-cookie-key-file=PATH, the engine's --zebkit-... and Chromium's\n"
                "  exit status: 1 when the module threw or an error went uncaught (printed, run on to the end)\n");
        return 2;
    }
    /* --zebkit-cookie-key-file=PATH: cookies encrypted with the application's
     * own key, its 32 bytes read from PATH (zebkit_set_cookie_encryption_key):
     * the key is never an argument. --zebkit-cookie-storage=os|file and
     * --zebkit-scheme=... are the engine's own switches. */
    for (int i = 1; argv[i] != module_path; ++i) {
        static const char prefix[] = "--zebkit-cookie-key-file=";
        if (strncmp(argv[i], prefix, sizeof prefix - 1) != 0)
            continue;
        unsigned char key[33];
        FILE *file = open_utf8(argv[i] + sizeof prefix - 1);
        /* Unbuffered, before the first read: no copy of the key is left in
         * stdio's buffer, which fclose() frees without wiping. */
        if (file)
            setvbuf(file, NULL, _IONBF, 0);
        const size_t size = file ? fread(key, 1, sizeof key, file) : 0;
        if (file)
            fclose(file);
        const bool set = size == 32 && zebkit_set_cookie_encryption_key(key, 32);
        wipe(key, sizeof key);
        if (!set) {
            fprintf(stderr, "zebkit-js: %s holds no 32-byte key\n", argv[i] + sizeof prefix - 1);
            return 2;
        }
    }
    int code = zebkit_main(argc, argv, ready, NULL);
    return failed ? 1 : code;
}
