/*
 * zebkit on Windows: the application's executable as the engine's sandboxed
 * child process.
 *
 * Chromium's sandbox on Windows writes into a child at addresses from its own
 * image, so the process that starts children and the children must be one
 * executable (docs/WINDOWS.md). An application links zebkit_sandbox.lib, with
 * the static C runtime (/MT), and calls this first in main():
 *
 *     int main(int argc, char **argv) {
 *         int code;
 *         if (zebkit_win_startup(&code))
 *             return code;     // this process was one of the engine's children
 *         ...                  // the application, as on every system
 *     }
 *
 * The rest of the API is the same on Linux and macOS. Without this call the
 * engine runs only with --no-sandbox, and says so.
 */

#ifndef ZEBKIT_WIN_SANDBOX_H
#define ZEBKIT_WIN_SANDBOX_H

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In zebkit_sandbox.lib: true when this process was a child of the engine and
 * has finished, with its exit code in @exit_code. */
bool zebkit_win_startup(int *exit_code);

/* In zebkit.dll, for zebkit_win_startup() alone: this executable's sandbox. */
ZEBKIT_API void zebkit_win_set_sandbox(void *sandbox_interface_info);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_WIN_SANDBOX_H */
