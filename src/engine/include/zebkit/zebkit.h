/*
 * zebkit: the web engine, for applications that embed it.
 *
 * The engine is Chromium's content layer, in a library: libzebkit.so.1 on
 * Linux, Zebkit.framework on macOS, zebkit.dll on Windows. On Linux and
 * macOS a helper program beside it runs the renderer, GPU and utility
 * processes; on Windows the application's own executable runs them, linking
 * zebkit_sandbox.lib and calling zebkit_win_startup() first in main()
 * (<zebkit/win_sandbox.h>). An application links the library,
 * calls zebkit_main() from its main thread, and creates views when the
 * engine is ready:
 *
 *     static void ready(void *data) {
 *         zebkit_view_new("https://example.com/", 0, 0);
 *     }
 *
 *     int main(int argc, char **argv) {
 *         return zebkit_main(argc, argv, ready, NULL);
 *     }
 *
 * Threads: everything here is for the engine's thread, the one that called
 * zebkit_main() or zebkit_init(), and every callback is called on it, except
 * where a function says otherwise: zebkit_post_task(), which any thread may
 * call to run a function there, is one.
 *
 * Strings are UTF-8. A string the engine returns is the caller's, to free
 * with zebkit_free(); one it passes to a callback is the engine's, valid
 * for the call.
 *
 * A function that returns bool and takes a function to answer it (its
 * completion: zebkit_post_task(), zebkit_view_set_user_agent_override(),
 * zebkit_view_export_native_window(), zebkit_view_context_menu_show()) never
 * calls that function when it returns false: what was given for it can be
 * released at once. Functions that take a handler of events (zebkit_view_on_*,
 * menus) are not completions.
 */

#ifndef ZEBKIT_ZEBKIT_H
#define ZEBKIT_ZEBKIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ZEBKIT_IMPLEMENTATION) && defined(_WIN32)
#define ZEBKIT_API __declspec(dllexport)
#elif defined(ZEBKIT_IMPLEMENTATION)
#define ZEBKIT_API __attribute__((visibility("default")))
#else
#define ZEBKIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* zebkit's version, as in "0.1.0". */
ZEBKIT_API const char *zebkit_version(void);

/* The engine's, as in "Chromium 157.0.8103.38". */
ZEBKIT_API const char *zebkit_engine_version(void);

/* Frees a string or buffer the engine returned. NULL is ignored. */
ZEBKIT_API void zebkit_free(void *pointer);

/* ---- The engine ------------------------------------------------------ */

typedef void (*zebkit_task_func)(void *data);

/* Scheduling notification for the application's loop: pump no later than
 * delay_ms from now (0: promptly). May run on any engine thread, including
 * during init. It must only queue a host event/timer, never call zebkit or
 * block. Keep the earliest outstanding deadline; a later notification must
 * not postpone earlier work. When the host calls pump, consume that deadline
 * before entering it. Chromium's native window events still need the host's
 * ordinary platform event loop.
 *
 * Configure before init; NULL removes it. Data must live through shutdown,
 * which waits for callbacks in progress and prevents subsequent callbacks: so
 * do not hold a mutex the callback takes while calling zebkit_shutdown(). The
 * callback may run on the thread that posted the task, inside that call, so a
 * thread must not post while holding a mutex the callback takes unless that
 * mutex is recursive.
 * False after startup has been attempted. Not used by zebkit_main().
 */
typedef void (*zebkit_wakeup_func)(int64_t delay_ms, void *data);
ZEBKIT_API bool zebkit_set_wakeup(zebkit_wakeup_func func, void *data);

/*
 * Initializes the engine without taking over the application's loop. Returns
 * 0 when views may be created, or a nonzero startup error. On macOS, the main
 * thread only. Elsewhere any thread, which is then the engine's: it pumps, and
 * makes every call that needs the UI thread. SIGPIPE is set to be ignored if
 * it has the default action, and Chromium does not change it otherwise.
 * Chromium switches in argv have the same meaning as in zebkit_main().
 * Call only once per process (including after shutdown). Do not mix with
 * zebkit_main(). Use zebkit_set_wakeup() for event-driven integration, or
 * call zebkit_pump() periodically from the host's loop.
 */
ZEBKIT_API int zebkit_init(int argc, char **argv);

/* init with validated JSON startup settings, applied over corresponding argv
 * switches. Other Chromium switches remain an escape hatch. NULL is {}.
 * profileDirectory (absolute path), proxy (Chrome proxy-server syntax),
 * userAgent (string), headless/sandbox (boolean), gpu (auto/disabled/software),
 * logging: {destination: stderr/file/none, path: absolute file path, level: 0..3},
 * cookies: {storage: os/file, keyDirectory: absolute path, thirdParty: boolean},
 * applicationIntegration (boolean; opt into zebkit's macOS delegate/menu/Dock),
 * appearance: {follow: boolean}: pages take the system's colour scheme and
 * contrast, and their changes (default false: light, as before).
 * httpCache: {storage: disk/memory/off, maxSize: bytes}: the engine profile's
 * HTTP cache, default disk (<profile>/Cache) as Chrome's; maxSize 0 (default)
 * is Chromium's choice; contexts keep theirs in memory (<zebkit/services.h>).
 * preconnect: [http(s) URLs]: a connection to each origin as the engine
 * starts, for the engine's profile (zebkit_preconnect()).
 * Unknown keys and malformed values fail before startup or file IO. error,
 * when provided, is NULL on success or a message to free with zebkit_free().
 */
ZEBKIT_API int zebkit_init_with_settings(int argc, char **argv,
    const char *settings_json, char **error);

/* Processes currently runnable engine and native events without waiting.
 * Call on the initializing thread, outside an engine callback. Returns false
 * before init, after shutdown, on another thread or during a nested pump.
 * The host remains responsible for its own loop and for calling this again
 * when delayed work becomes due (for example from a toolkit timer). Native
 * events are the platform loop's: on macOS the application's own timers and
 * events can run inside this call, where zebkit_shutdown() is refused; stop
 * after it returns.
 */
ZEBKIT_API bool zebkit_pump(void);

/* Closes views and stops the engine's threads and child processes. Call on
 * the initializing thread, outside callbacks. False in the same invalid
 * states as zebkit_pump(). The host's loop continues after this returns.
 */
ZEBKIT_API bool zebkit_shutdown(void);

/*
 * Starts the engine, calls @ready on this thread once views can be created,
 * runs until zebkit_quit(), and returns the process's exit code. Call it
 * once, from the main thread, with main()'s arguments: Chromium's switches
 * among them are honoured, such as --remote-debugging-port=9222 (CDP, as
 * Chrome serves it), --user-data-dir, --no-sandbox, --headless and
 * --enable-logging=stderr.
 *
 * Views have windows when the machine has a display (Wayland or X11 on
 * Linux; on macOS always) and none otherwise or with --headless; a page
 * loads, runs and can be driven over CDP either way. Profile data is kept
 * in --user-data-dir, by default $XDG_CONFIG_HOME/zebkit/<program name>,
 * on macOS ~/Library/Application Support/zebkit/<program name>, on Windows
 * %LOCALAPPDATA%\zebkit\<program name>; with no
 * home to put it in, a new private directory in the temporary one, deleted
 * as the engine stops. A --user-data-dir that cannot be made fails the
 * start (exit code 1). The log
 * goes to stderr unless --enable-logging or --log-file says otherwise.
 *
 * The loop runs until zebkit_quit(), also with no view open: closing the
 * last window is the application's to act on (zebkit_view_on_close()).
 * Returns the engine's exit code; 1 also when a zebkit_script (below) left an
 * error uncaught while it ran.
 */
ZEBKIT_API int zebkit_main(int argc, char **argv, zebkit_task_func ready, void *data);

/* Built-in cookie key storage. Select on the main thread before zebkit_main().
 * OS is the default. FILE generates/reuses a private key file without OS key
 * store access. NULL/empty key_directory uses <user-data-dir>/Secrets; an
 * override must be absolute. The engine resolves its profile and loads the key
 * at startup, before opening cookie databases, returning exit code 1 on error.
 * Configuration never overwrites a bad/missing key for existing Cookies.host.
 * FILE preserves the same format/database as host-owned keys below.
 */
typedef enum zebkit_cookie_storage {
    ZEBKIT_COOKIE_STORAGE_OS = 0,
    ZEBKIT_COOKIE_STORAGE_FILE = 1
} zebkit_cookie_storage;
ZEBKIT_API bool zebkit_set_cookie_storage(zebkit_cookie_storage storage,
                                         const char *key_directory);

/* Optional host-owned cookie encryption, configured on the main thread before
 * zebkit_main(). Copies exactly 32 random bytes (AES-256-GCM). The host must
 * persist and reuse this key; no OS key-store provider is accessed in this mode.
 * NULL with size 0 restores the default OS provider before startup. False for
 * invalid size or after startup. Host cookies use Network/Cookies.host, leaving
 * the OS-encrypted Network/Cookies untouched. Losing/changing the key loses
 * access to host cookies. Never expose it to websites, arguments or logs.
 */
ZEBKIT_API bool zebkit_set_cookie_encryption_key(const uint8_t *key, size_t size);

/* Closes every view and ends zebkit_main()'s loop. After zebkit_init() there is
 * no such loop: it closes the views, and the application calls zebkit_shutdown().
 */
ZEBKIT_API void zebkit_quit(void);

/* Runs @func(@data) on the engine's thread. Any thread may call it. False,
 * and @func is never called, for a NULL @func, before the engine has started
 * and once it has stopped. */
ZEBKIT_API bool zebkit_post_task(zebkit_task_func func, void *data);

/* The same, @delay_ms milliseconds from now. */
ZEBKIT_API bool zebkit_post_delayed_task(zebkit_task_func func, void *data, uint32_t delay_ms);

/* Whether views are without windows: no display, or --headless. */
ZEBKIT_API bool zebkit_is_headless(void);

/*
 * What the system says of how things should look, as JSON: {"colorScheme":
 * "light" | "dark", "contrast": "no-preference" | "more" | "less" | "custom",
 * "forcedColors", "reducedTransparency", "invertedColors", "locale",
 * "following", "accessibility": "off" | "native" | "basic" | "complete"}
 * (pages follow the system with the startup setting appearance.follow; the
 * rest is the system's whether they do or not). Caller frees. NULL off the engine's thread.
 * zebkit_on_environment_changed() tells changes; NULL stops. The JSON is the
 * engine's, for the call. The engine never changes the system's settings.
 */
ZEBKIT_API char *zebkit_get_environment(void);

/*
 * Web notifications (new Notification(), a service worker's
 * showNotification()), once a page has the "notifications" permission, which is
 * undecided until the application decides (zebkit_view_on_permission,
 * zebkit_view_set_permission): Notification.permission is "default", and a
 * request nobody answers is denied. With @func the application shows
 * them: @func gets {"event": "show", id, persistent, origin, title, body, icon,
 * image, badge, tag, lang, silent, requireInteraction, renotify, actions: [{action,
 * title}]} and {"event": "close", id} when the page closes one; it tells what the
 * person did with zebkit_notification_clicked(id, action index or -1) and
 * zebkit_notification_closed(id, by_user), which reach the page's onclick/onclose
 * and its service worker's notificationclick/notificationclose. Without @func the
 * system shows them where it can: org.freedesktop.Notifications on Linux, the
 * UserNotifications framework on macOS (for an application with a bundle
 * identifier). The JSON is the engine's, for the call. Engine thread only.
 */
typedef void (*zebkit_notification_func)(const char *json, void *data);

/*
 * The system's handler for @url (a browser for https:, the mail program for
 * mailto:, the default program of a file: URL), or the file manager showing
 * @path selected in its folder. For the application: pages reach neither
 * (zebkit_view_on_external_protocol tells their links to other programs). The
 * URL or path is one argument to the system, never a shell's. False with
 * @error for javascript:, data:, blob:, about: and invalid URLs, or a
 * relative path; true once the system is asked, which then goes on off the
 * engine's thread: the _async forms tell how it went.
 */
ZEBKIT_API bool zebkit_open_external(const char *url, char **error);
ZEBKIT_API bool zebkit_show_in_folder(const char *path, char **error);
/*
 * The same, telling @func how it went, once, on the engine's thread, after
 * the call returns: done, or false with the error (an invalid URL or path, a
 * file that is not there, no program for the URL, a file manager that
 * failed). macOS's Finder says nothing of itself: done once it is asked.
 * Off the engine's thread, @func is told the error at once. Not called if the
 * engine stops first: what @data holds is the application's to free then.
 */
typedef void (*zebkit_desktop_done_func)(bool done, const char *error, void *data);
ZEBKIT_API void zebkit_open_external_async(const char *url, zebkit_desktop_done_func func, void *data);
ZEBKIT_API void zebkit_show_in_folder_async(const char *path, zebkit_desktop_done_func func, void *data);

/*
 * What the system tells of itself, as JSON to @func on the engine's thread.
 * zebkit_on_power_event(): {"event": "suspend"}, {"event": "resume"},
 * {"event": "power-source", "onBattery"}, {"event": "lock"} and
 * {"event": "unlock"} for the session's lock, from the system's notifications (on Linux the
 * desktop screen saver's, over D-Bus; nothing is told on a desktop without one). zebkit_get_displays(): the screens,
 * [{id, bounds, workArea, scale, rotation, primary}] in DIPs (free it);
 * zebkit_on_displays_changed() tells {"event": "displays", "displays": [...]}.
 * NULL @func stops.
 * zebkit_prevent_sleep(): keeps the system from sleeping (and its display on,
 * with @keep_display_on) for the application until zebkit_allow_sleep(token),
 * or the engine's end; pages have the Wake Lock API for their own. 0 on failure.
 */
typedef void (*zebkit_system_event_func)(const char *json, void *data);
ZEBKIT_API void zebkit_on_power_event(zebkit_system_event_func func, void *data);
ZEBKIT_API char *zebkit_get_displays(void);
ZEBKIT_API void zebkit_on_displays_changed(zebkit_system_event_func func, void *data);
ZEBKIT_API uint64_t zebkit_prevent_sleep(bool keep_display_on, const char *reason);
ZEBKIT_API bool zebkit_allow_sleep(uint64_t token);
ZEBKIT_API void zebkit_on_notification(zebkit_notification_func func, void *data);
ZEBKIT_API bool zebkit_notification_clicked(const char *id, int action);
ZEBKIT_API bool zebkit_notification_closed(const char *id, bool by_user);
typedef void (*zebkit_environment_func)(const char *json, void *data);
ZEBKIT_API void zebkit_on_environment_changed(zebkit_environment_func func, void *data);

/* ---- The application ------------------------------------------------- */

typedef void (*zebkit_menu_func)(const char *id, void *data);

/*
 * The application's menu, from @json: [{"title": "File", "items": [...]},
 * ...], where an item is
 *   {"title": "Open…", "id": "open", "key": "o", "modifiers": ["shift"]}:
 *       @func gets "open" when it is chosen;
 *   {"role": "undo"}: a standard command, which also takes "title" (roles:
 *       undo, redo, cut, copy, paste, pasteAndMatchStyle, selectAll,
 *       minimize, zoom, close,
 *       fullscreen, front, quit, hide, hideOthers, showAll, about,
 *       services);
 *   {"title": "Zoom", "items": [...]}: a submenu; or {"separator": true}.
 * A first menu {"role": "appMenu", "items": [...]} is the application menu
 * on macOS, in place of zebkit's (About, Services, Hide, Quit).
 * "key" is with the command key, ⌘ on macOS and Ctrl elsewhere: a
 * character, or "f1" to "f24"; "modifiers" adds "shift", "alt" and "ctrl"
 * (Control, on macOS). On macOS it is the menu bar, after the application
 * menu zebkit makes (About, Services, Hide, Quit), with the window menu
 * the menu titled "Window". Elsewhere, where windows have no menu bar, the
 * items' keys work the same in the application's windows when the page
 * does not take them. On the engine's thread; again to change it.
 */
ZEBKIT_API bool zebkit_app_set_menu(const char *json, zebkit_menu_func func, void *data, char **error);

/*
 * The menu's items also take "enabled" (default true), "checked" (default
 * false) and "radioGroup" (one checked item in a group). Their state changes
 * without a new menu: zebkit_app_update_menu_item(@id, {"enabled",
 * "checked", "title"}) on every item with @id; checking one of a group
 * unchecks the others. A disabled item is not chosen, nor its key taken.
 * zebkit_app_get_menu() is the menu with its state, as JSON (free it).
 */
ZEBKIT_API bool zebkit_app_update_menu_item(const char *id, const char *json, char **error);
ZEBKIT_API char *zebkit_app_get_menu(void);

typedef void (*zebkit_paths_func)(const char *paths_json, void *data);

/*
 * Files the system asks the running application to open, as a JSON array
 * of paths: on macOS from the Finder or the Dock, also the ones it was
 * started with, which are kept until @func is set. Elsewhere they are
 * program arguments.
 */
ZEBKIT_API void zebkit_app_on_open_files(zebkit_paths_func func, void *data);

typedef void (*zebkit_app_func)(void *data);

/*
 * The application's icon in the Dock (macOS) or on the taskbar (Windows), or on a
 * Linux dock that knows the launcher API (com.canonical.Unity.LauncherEntry; its
 * desktop file named after the program): a badge (a number on Linux; "" for none),
 * a progress from 0 to 1 (below 0 for none), and a request for the person's
 * attention (the Dock's bounce, the taskbar button's flash). The Dock's menu, on
 * macOS only, takes zebkit_app_set_menu()'s items; @func gets a chosen item's id.
 */
ZEBKIT_API bool zebkit_app_set_badge(const char *text);
ZEBKIT_API bool zebkit_app_set_progress(double progress);
ZEBKIT_API void zebkit_app_request_attention(bool critical);
ZEBKIT_API bool zebkit_app_set_dock_menu(const char *json, zebkit_menu_func func, void *data, char **error);

/* The application's icon in the Dock was clicked while no window was shown (macOS). */
ZEBKIT_API void zebkit_app_on_reopen(zebkit_app_func func, void *data);

/*
 * The user asked the application to quit (⌘Q, the Dock, logging out, on
 * macOS): @func decides, calling zebkit_quit() or not. Without it zebkit
 * quits.
 */
ZEBKIT_API void zebkit_app_on_quit_request(zebkit_app_func func, void *data);

/*
 * The application's icon in the system's tray: the menu bar on macOS, the
 * notification area on Windows, a StatusNotifierItem on Linux (a desktop
 * without a host for those shows none). @png: the icon, a PNG of at most 1024
 * by 1024 (a template image on macOS: its shape, in the menu bar's colour).
 * Its menu takes zebkit_app_set_menu()'s items, enabled, checked and
 * radioGroup included; @func gets a chosen item's id. A click on the icon is
 * zebkit_tray_on_click()'s; on macOS, with a menu, a click opens it. Engine
 * thread only; zebkit_tray_close() removes it.
 */
typedef struct zebkit_tray zebkit_tray;
ZEBKIT_API zebkit_tray *zebkit_tray_new(const uint8_t *png, size_t size, const char *tooltip, char **error);
ZEBKIT_API bool zebkit_tray_set_icon(zebkit_tray *tray, const uint8_t *png, size_t size, char **error);
ZEBKIT_API void zebkit_tray_set_tooltip(zebkit_tray *tray, const char *tooltip);
ZEBKIT_API bool zebkit_tray_set_menu(zebkit_tray *tray, const char *json, zebkit_menu_func func, void *data,
                                     char **error);
ZEBKIT_API void zebkit_tray_on_click(zebkit_tray *tray, zebkit_app_func func, void *data);
ZEBKIT_API void zebkit_tray_close(zebkit_tray *tray);

/* ---- Views ----------------------------------------------------------- */

typedef struct zebkit_view zebkit_view;

/*
 * The rule for every zebkit_view_*() function: on the engine's thread, with a
 * view that has not closed. Called otherwise (NULL, a closed view, another
 * thread) it does nothing and returns its failure value: false, NULL, or
 * nothing. A callback the engine owes a view (an evaluation, a capture, a
 * PDF, a media list, a drag's answer) is called exactly once: with the result,
 * or, when the view closes first, with an error, the view still valid and
 * before its on_close.
 */

/*
 * A view loading @url (NULL for none: about:blank, whose loading is not
 * reported, until zebkit_view_load_url()), @width by @height pixels, or
 * a default size when either is 0. It lives until it is closed, by
 * zebkit_view_close(), by the user closing its window, by the page's
 * window.close(), or by zebkit_quit(); the on_close callback is the last
 * use of it. zebkit_view_close() and zebkit_quit() take effect once the
 * current callback has returned, so either may be called from any of them.
 */
ZEBKIT_API zebkit_view *zebkit_view_new(const char *url, int width, int height);

/*
 * A context: a profile of its own, in memory, as an incognito window's:
 * its cookies, storage, cache and permissions are its own and go with it.
 * Playwright's BrowserContext. zebkit_context_close() closes its views once
 * the current callback has returned, as zebkit_view_close() does, so it may
 * be called from any of them; the profile itself is freed when its pages'
 * processes have gone, or else when the engine stops.
 */
typedef struct zebkit_context zebkit_context;
ZEBKIT_API zebkit_context *zebkit_context_new(void);
ZEBKIT_API void zebkit_context_close(zebkit_context *context);

/* zebkit_view_new() in @context; NULL for the engine's own profile. */
ZEBKIT_API zebkit_view *zebkit_view_new_in(zebkit_context *context, const char *url, int width, int height);

ZEBKIT_API void zebkit_view_load_url(zebkit_view *view, const char *url);
/* CDP target identifier for this existing view. Caller frees with zebkit_free().
 * Use it to adopt a popup; recreating only its URL loses POST data and opener. */
ZEBKIT_API char *zebkit_view_get_target_id(zebkit_view *view);

ZEBKIT_API void zebkit_view_reload(zebkit_view *view);
ZEBKIT_API void zebkit_view_go_back(zebkit_view *view);
ZEBKIT_API void zebkit_view_go_forward(zebkit_view *view);
ZEBKIT_API void zebkit_view_resize(zebkit_view *view, int width, int height);

/*
 * The smallest the view's window may be made, its content @min_width by
 * @min_height (0 for no limit), and whether it may be resized at all:
 * Electron's minWidth, minHeight and resizable.
 */
ZEBKIT_API void zebkit_view_set_size_limits(zebkit_view *view, int min_width, int min_height, bool resizable);

/*
 * Places @view inside @parent's window, over @parent's page, at (@x, @y),
 * @width by @height, in @parent's coordinates (CSS pixels at zoom 1); its
 * own window is hidden. A browser inside an application's window. The view
 * closes with @parent (zebkit_view_on_close()). Attached again to another
 * view, it moves there; zebkit_view_attach_native() puts its page back in
 * its own window first. False for a NULL or closed view, @view itself as
 * @parent, or a parent without a window to be in.
 */
ZEBKIT_API bool zebkit_view_attach(zebkit_view *view, zebkit_view *parent, int x, int y, int width, int height);

/* Where an attached view is in its parent's window. */
ZEBKIT_API void zebkit_view_set_bounds(zebkit_view *view, int x, int y, int width, int height);

/* Shows or hides an attached view. */
ZEBKIT_API void zebkit_view_set_visible(zebkit_view *view, bool visible);

/*
 * Puts @view's window inside a window of the application's, at (@x, @y),
 * @width by @height: @native_parent is an X11 window id, on macOS an NSView*.
 * The application keeps its window, toolkit and loop; the engine draws in its
 * own child window, and takes its input from the system there. The units are
 * the parent's: X11 pixels, AppKit points.
 *
 * zebkit_view_set_bounds(), _set_visible(), _resize() and _focus() then act in
 * the parent. A click in the page gives it the keyboard; so does
 * zebkit_view_focus(), which an application calls when focus comes to it. The
 * view's window is shown as its own until this is called, so call it right
 * after zebkit_view_new(). A view closed takes only its child window. On X11, a
 * parent destroyed first takes the view's window with it and the view closes
 * (zebkit_view_on_close()); on macOS, where a view outlives its superview, the
 * view stays until the application closes or attaches it again.
 *
 * False, with the reason in @error (free with zebkit_free(); NULL if unwanted),
 * for a parent that is not there, and where a window cannot be put in another:
 * on Wayland, under --headless, and for an off-screen view. Those draw the page
 * in the application's window with off-screen views (<zebkit/offscreen.h>),
 * which work everywhere. On Windows @native_parent is an HWND.
 */
ZEBKIT_API bool zebkit_view_attach_native(zebkit_view *view, uintptr_t native_parent, int x, int y, int width, int height, char **error);

/* Makes the view its own window again, shown. False when it is in no window of the application's. */
ZEBKIT_API bool zebkit_view_detach_native(zebkit_view *view);

/* Borrowed system window containing the page, for parenting native dialogs.
 * X11: window id; Windows: HWND; AppKit: NSWindow* (not the NSView* used by
 * attach_native). Always the top-level window: for an attached page, the one
 * containing the application's parent (on X11, the client window the window
 * manager manages, not its frame).
 * No ownership transfers: do not destroy/release the handle. Query again after
 * attaching/detaching; it is invalid once that window closes. UI thread only.
 * False with NONE/0 for NULL/closed views, off-screen/headless views, Wayland
 * (which requires asynchronous exported parent tokens), or the wrong thread.
 * A NULL output is rejected. This never exposes Chromium's internal pointers.
 */
typedef enum {
    ZEBKIT_NATIVE_WINDOW_NONE = 0,
    ZEBKIT_NATIVE_WINDOW_X11 = 1,
    ZEBKIT_NATIVE_WINDOW_WIN32 = 2,
    ZEBKIT_NATIVE_WINDOW_APPKIT = 3
} zebkit_native_window_kind;
typedef struct {
    zebkit_native_window_kind kind;
    uintptr_t handle;
} zebkit_native_window;
ZEBKIT_API bool zebkit_view_get_native_window(zebkit_view *view, zebkit_native_window *window);

/* The page's window as the system's portals name a parent window (XDG Desktop
 * Portal's parent_window): "x11:<XID>" or, on Wayland, "wayland:<handle>"
 * exported through xdg-foreign, so that a portal's dialog (a file chooser, a
 * screen share) is the window's own. Wayland exports it asynchronously: the
 * callback runs once, on the engine thread, after this returns, with the
 * handle, or NULL when there is none (headless, off-screen, a compositor
 * without xdg-foreign, the view closed meanwhile; on Windows and macOS always,
 * where zebkit_view_get_native_window() is the handle). The string is the
 * callback's to copy. A handle stays valid while its window is open. A view
 * that closes first is told NULL before its on_close, @view still valid, as
 * every callback the engine owes a view. False, and no callback, for a NULL
 * view or callback or on another thread. */
typedef void (*zebkit_native_window_export_func)(zebkit_view *view, const char *handle, void *data);
ZEBKIT_API bool zebkit_view_export_native_window(zebkit_view *view, zebkit_native_window_export_func callback,
                                                 void *data);

/* The title of the view's window; NULL for the page's, which it is by default. */
ZEBKIT_API void zebkit_view_set_title(zebkit_view *view, const char *title);

/* Brings the view's window to the front and gives it the keyboard. */
ZEBKIT_API void zebkit_view_focus(zebkit_view *view);
ZEBKIT_API void zebkit_view_close(zebkit_view *view);

/* The page's URL and title. Free with zebkit_free(). */
ZEBKIT_API char *zebkit_view_get_url(zebkit_view *view);
ZEBKIT_API char *zebkit_view_get_title(zebkit_view *view);

typedef enum {
    ZEBKIT_LOAD_STARTED = 0,   /* a load of the page was asked for */
    ZEBKIT_LOAD_COMMITTED = 1, /* the new document replaced the old */
    ZEBKIT_LOAD_FINISHED = 2,  /* the page and its resources have loaded */
    ZEBKIT_LOAD_FAILED = 3,    /* the load failed or was stopped */
} zebkit_load_event;

typedef void (*zebkit_load_func)(zebkit_view *view, zebkit_load_event event, const char *url, void *data);
typedef void (*zebkit_view_func)(zebkit_view *view, void *data);
typedef void (*zebkit_title_func)(zebkit_view *view, const char *title, void *data);

/* One callback of each kind per view; a new one replaces the old, NULL removes it. */
ZEBKIT_API void zebkit_view_on_load(zebkit_view *view, zebkit_load_func func, void *data);
ZEBKIT_API void zebkit_view_on_title(zebkit_view *view, zebkit_title_func func, void *data);
ZEBKIT_API void zebkit_view_on_close(zebkit_view *view, zebkit_view_func func, void *data);

/*
 * The page's process went (Electron's render-process-gone), told
 * asynchronously as JSON: {"reason", "exitCode"}, reason one of "crashed" (a
 * crash, chrome://crash), "killed" (a signal, the system, chrome://kill),
 * "oom" (out of memory), "abnormal-exit" (a non-zero exit), "clean-exit",
 * "launch-failed", "integrity-failure" (Windows' code integrity),
 * "memory-eviction"; exitCode is the process's exit code or signal as the
 * system gives it. The view stays open, its page blank: zebkit_view_reload()
 * or zebkit_view_load_url() start a process again. One function per view;
 * NULL removes it.
 */
typedef void (*zebkit_process_func)(zebkit_view *view, const char *json, void *data);
ZEBKIT_API void zebkit_view_on_process_gone(zebkit_view *view, zebkit_process_func func, void *data);

/*
 * The page stopped answering input, as Chrome's "page unresponsive" dialog
 * (which zebkit does not show) and Electron's unresponsive/responsive events:
 * {"responsive": false}, then {"responsive": true} once it answers again. Only
 * the changes are told, asynchronously.
 *
 * It is Chromium's hang monitor, so it is told when:
 * - input that waits on the page's main thread (a key, a mouse button, a
 *   wheel or touch the page listens to) has not been answered after 15
 *   seconds and a ping of that thread a second more; a page that hangs
 *   without input is not told until input comes. The delay is the feature
 *   --enable-features=RendererHangWatcher:delay/2s (a switch at start);
 *   --disable-hang-monitor turns it off. Input is the user's in a window or
 *   attached view, or zebkit_view_send_key() / zebkit_view_send_mouse() to an
 *   off-screen view;
 * - or a navigation or its cancellation waits on a page that does not answer;
 * - and the view is shown (not minimised, or hidden with zebkit_view_set_visible());
 * - and no debugger is attached to the page: a CDP client (zebkit_cdp_listen(),
 *   --remote-debugging-port, a CDP session a Playwright client opens on the
 *   endpoint) holds it, as in Chrome, since its page may sit on a breakpoint.
 *   Automation is not a debugger: a page driven by zebkit_automation_*() or
 *   through a Playwright endpoint (<zebkit/automation.h>) is told, as is one
 *   driven by the view's own session (zebkit_view_evaluate(), init scripts,
 *   grants).
 * A page whose process goes is told by zebkit_view_on_process_gone() instead,
 * and starts responsive. One function per view; NULL removes it.
 */
ZEBKIT_API void zebkit_view_on_responsiveness(zebkit_view *view, zebkit_process_func func, void *data);

/*
 * The result of a script, or of a call from the page. @json is the value as
 * JSON, or NULL when @error is set; @error is the message of what was
 * thrown, or NULL.
 */
typedef void (*zebkit_result_func)(zebkit_view *view, const char *json, const char *error, void *data);

/*
 * Runs @script in the page's main frame, as the page's own script would
 * run, and calls @func with its value. A promise is waited for. @func may
 * be NULL. It runs as the person's act, as DevTools' console runs script:
 * it may open a window, which the page's own script may only do while the
 * person clicks or types (popups are blocked as Chrome blocks them).
 */
ZEBKIT_API void zebkit_view_evaluate(zebkit_view *view, const char *script, zebkit_result_func func, void *data);

/*
 * Runs @source in every document the view loads from now on, in each
 * frame's main world, before the page's own script: what Electron calls a
 * preload, Puppeteer evaluateOnNewDocument. A load asked for right after
 * waits until the engine has it.
 */
ZEBKIT_API void zebkit_view_add_script(zebkit_view *view, const char *source);

/* Stops the page's load. */
ZEBKIT_API void zebkit_view_stop(zebkit_view *view);

/* Whether the page is loading. */
ZEBKIT_API bool zebkit_view_is_loading(zebkit_view *view);

/* The User-Agent for the view's requests and documents.
 * NULL/empty clears this session's override. Invalid HTTP header values are
 * ignored without changing current or future-document overrides. External
 * CDP sessions keep Chromium's own independent override scope. */
ZEBKIT_API void zebkit_view_set_user_agent(zebkit_view *view, const char *user_agent);
/* Atomically set the string and optional client hints using
 * Emulation.setUserAgentOverride JSON: userAgent is required; acceptLanguage,
 * platform and userAgentMetadata are optional. Up to 65536 UTF-8 bytes.
 * True when queued: func is then told once, after this returns, {} on success
 * or the error (invalid JSON or options among them). False, func never
 * called, for a NULL or closed view or off the engine's thread. Only
 * successful settings persist into future pages. Loads wait for completion.
 * Closing reports an error for pending callbacks before the view's close
 * notification.
 */
ZEBKIT_API bool zebkit_view_set_user_agent_override(zebkit_view *view, const char *json,
                                                    zebkit_result_func func, void *data);

typedef void (*zebkit_capture_func)(zebkit_view *view, const uint8_t *png, size_t size, const char *error,
                                    void *data);

/*
 * A picture of what the view shows, as PNG bytes, valid during @func; or
 * @error. Chrome's Page.captureScreenshot: with or without a window.
 */
ZEBKIT_API void zebkit_view_capture(zebkit_view *view, zebkit_capture_func func, void *data);

typedef void (*zebkit_url_func)(zebkit_view *view, const char *url, void *data);

/*
 * What a page of @view opens in a new window (window.open, a link with
 * target=_blank) goes to @func as its URL, and no window is made: the
 * application opens it where it wants. With no function the engine opens
 * a window of its own, which the application has no view of.
 */
ZEBKIT_API void zebkit_view_on_new_window(zebkit_view *view, zebkit_url_func func, void *data);

typedef void (*zebkit_window_open_func)(zebkit_view *opener, zebkit_view *opened, const char *url, void *data);

/*
 * A window a page of @view opens (window.open, a link to a new window) is
 * made as a view of its own and given to @func, still linked to the page
 * that opened it: window.opener, postMessage() and window.closed work
 * between the two, as a site's sign-in through a popup needs ("Sign in
 * with Google"). The popup is as @view is: a window, or off the screen
 * (<zebkit/offscreen.h>), its frames for the application to show. It is
 * the application's to show and to close; the page closing it
 * (window.close()) calls its zebkit_view_on_close(). A page opens
 * one only while the person clicks or types, as in Chrome.
 * zebkit_view_on_new_window(), when set, takes the URL instead, and no
 * popup is made.
 */
ZEBKIT_API void zebkit_view_on_window_open(zebkit_view *view, zebkit_window_open_func func, void *data);

typedef void (*zebkit_drop_func)(zebkit_view *view, const char *paths_json, void *data);

/*
 * The paths of files dropped on the view, from the desktop or another
 * application, as a JSON array, to @func before the page gets the drop:
 * what a page cannot learn of a dropped file. With @func set, the page's
 * zebkit.pathForFile(file) gives a dropped File's path too (as Electron's
 * webUtils.getPathForFile does): the page has the paths before the drop.
 */
ZEBKIT_API void zebkit_view_on_drop(zebkit_view *view, zebkit_drop_func func, void *data);

/* ---- The page's script and the application -------------------------- */

/*
 * What a page may use of the global object `zebkit`. A page has none of it
 * unless zebkit_view_grant() gives it to the page's origin.
 *
 *   ZEBKIT_ALLOW_APP       zebkit.app.<name>(...args): the functions the
 *                          application binds with zebkit_view_bind()
 *   ZEBKIT_ALLOW_FETCH     zebkit.fetch(url, {method, headers, body}): the
 *                          engine's network stack, without CORS, with the
 *                          browser's cookies (zebkit_fetch())
 *   ZEBKIT_ALLOW_FILES     zebkit.fs: readFile, writeFile, readDir, stat,
 *                          mkdir, remove, under the directory granted
 *   ZEBKIT_ALLOW_COMPRESS  zebkit.compress(format, bytes, level) and
 *                          zebkit.decompress(format, bytes)
 *   ZEBKIT_ALLOW_CRYPTO    zebkit.crypto: digest, hmac, randomBytes, aesGcmSeal, aesGcmOpen
 *   ZEBKIT_ALLOW_IMAGE     zebkit.image: decode(bytes), encode(format, image, quality)
 *   ZEBKIT_ALLOW_URL       zebkit.url: parse(url, base), registrableDomain(host)
 *   ZEBKIT_ALLOW_PDF       zebkit.pdf: open(bytes), and the document's
 *                          functions (<zebkit/pdf.h>); PDFium works on the
 *                          page's documents in the browser process
 *
 * Every function returns a promise. Bytes are Uint8Arrays.
 */
typedef enum {
    ZEBKIT_ALLOW_APP = 1 << 0,
    ZEBKIT_ALLOW_FETCH = 1 << 1,
    ZEBKIT_ALLOW_FILES = 1 << 2,
    ZEBKIT_ALLOW_COMPRESS = 1 << 3,
    ZEBKIT_ALLOW_CRYPTO = 1 << 4,
    ZEBKIT_ALLOW_IMAGE = 1 << 5,
    ZEBKIT_ALLOW_URL = 1 << 6,
    ZEBKIT_ALLOW_PDF = 1 << 7,
    ZEBKIT_ALLOW_ALL = 0xff,
} zebkit_allow;

/*
 * Gives pages of @origin in @view what @allow says (zebkit_allow bits; 0
 * takes it back). @origin is as a browser serializes it, such as
 * "https://app.example" or "http://127.0.0.1:8080", or "file://" for local
 * files, or "*" for every origin, which is for pages the application wrote
 * itself and nothing else. @files_root is the directory zebkit.fs works in
 * for ZEBKIT_ALLOW_FILES, or NULL. Frames of another origin than a grant's
 * get nothing, and frames of another site are never given the object.
 */
ZEBKIT_API void zebkit_view_grant(zebkit_view *view, const char *origin, unsigned allow, const char *files_root);

typedef struct zebkit_call zebkit_call;

/*
 * A call from the page: zebkit.app.<name>(...args). @args_json is the
 * arguments as a JSON array, @origin the calling page's. Answer it, now or
 * later, with exactly one of zebkit_call_resolve() and zebkit_call_reject(),
 * from any thread (an answer from another thread reaches the page through the
 * engine's thread), or drop it with zebkit_call_free(). A call never answered
 * nor freed stays in memory until the end, its promise pending.
 */
typedef void (*zebkit_bind_func)(zebkit_view *view, const char *origin, const char *args_json, zebkit_call *call,
                                 void *data);

/* Makes zebkit.app.<@name> call @func. NULL @func removes it. */
ZEBKIT_API void zebkit_view_bind(zebkit_view *view, const char *name, zebkit_bind_func func, void *data);

/* Resolves the page's promise with @json (NULL for undefined). */
ZEBKIT_API void zebkit_call_resolve(zebkit_call *call, const char *json);

/* The same with @length bytes of JSON at @json, which need not end in a NUL:
 * a part of a larger string, the arguments' own for one, goes as it is,
 * without a copy. */
ZEBKIT_API void zebkit_call_resolve_json(zebkit_call *call, const char *json, size_t length);

/* Rejects the page's promise with an Error of @message. */
ZEBKIT_API void zebkit_call_reject(zebkit_call *call, const char *message);

/* Drops a call the application will not answer, a binding's object for it
 * gone unanswered: the page's promise is rejected with an Error ("the
 * application dropped the call") and @call is freed. From any thread, as the
 * answers; after one of them @call is gone, and is not to be freed. */
ZEBKIT_API void zebkit_call_free(zebkit_call *call);

/* ---- Script outside any page ---------------------------------------- */

/*
 * A JavaScript context of the application's own, in this process, on the
 * engine's thread: V8 with the whole of zebkit.* (no grants: it is the
 * application's code, as an Electron application's main process is), and
 * zebkit.View to make views, load pages in them and answer their calls.
 * <zebkit/zebkit.h>'s script reference is docs/API.md. An error thrown from
 * one of its callbacks, or a promise rejected with no handler, is printed to
 * stderr ("Uncaught ...") and the script runs on; zebkit_main() then returns 1.
 * A page's call to a bound function whose result never settles is dropped
 * (zebkit_call_free()) once that result is collected or the script ends.
 */
typedef struct zebkit_script zebkit_script;

/* @argc strings at @argv: what the script sees as zebkit.args; @argv may be NULL. */
ZEBKIT_API zebkit_script *zebkit_script_new(int argc, const char *const *argv);

/*
 * Runs the ES module in the file @path, with the modules it imports
 * relative to it, and calls @func when its evaluation ends (for a
 * top-level await, when the promise settles): @json NULL, or @error the
 * message and stack of what was thrown.
 */
ZEBKIT_API void zebkit_script_run_module(zebkit_script *script, const char *path, zebkit_result_func func,
                                         void *data);

/* Evaluates @source as a classic script; @func gets the value as JSON. */
ZEBKIT_API void zebkit_script_evaluate(zebkit_script *script, const char *source, zebkit_result_func func,
                                       void *data);

/*
 * Called once nothing of the script is pending: no timer, no call in
 * progress, no view open, and its module evaluated. zebkit-js quits then,
 * as Node exits.
 */
ZEBKIT_API void zebkit_script_on_idle(zebkit_script *script, zebkit_task_func func, void *data);

/* Ends the script: its timers stop, its views stay. */
ZEBKIT_API void zebkit_script_destroy(zebkit_script *script);

/* ---- The Chrome DevTools Protocol ------------------------------------ */

/*
 * Serves CDP on @address (a numeric IP address, NULL for 127.0.0.1) and
 * @port (0 for one the system chooses), as Chrome does with
 * --remote-debugging-port. Returns the port, or 0 with @error set (free it
 * with zebkit_free(); @error may be NULL). If the endpoint already runs,
 * returns its port; asking for another address or port then is an error.
 * Waits, on the engine's thread, for the server's thread to bind (up to ten
 * seconds): zebkit_cdp_listen_async() does not.
 */
ZEBKIT_API unsigned zebkit_cdp_listen(const char *address, unsigned port, char **error);
/*
 * zebkit_cdp_listen() without waiting: @func gets the port and the browser
 * target's WebSocket address (as zebkit_cdp_get_url() gives it), or 0, NULL
 * and the error, once, on the engine's thread, after this returns. Calls
 * while it starts are answered together; zebkit_cdp_stop() answers them with
 * an error. Off the engine's thread, @func is told the error at once. Not
 * called if the engine stops first (a call refused at once is answered by a
 * task of the engine's thread, which then does not run): what @data holds is
 * the application's to free then.
 */
typedef void (*zebkit_cdp_listen_func)(unsigned port, const char *url, const char *error, void *data);
ZEBKIT_API void zebkit_cdp_listen_async(const char *address, unsigned port, zebkit_cdp_listen_func func, void *data);

/* The port CDP is served on, or 0. */
ZEBKIT_API unsigned zebkit_cdp_get_port(void);

/*
 * The browser target's WebSocket address, what Puppeteer calls
 * browserWSEndpoint, or NULL when CDP is not served. Free with zebkit_free().
 * Right after zebkit_cdp_listen() it may wait a little for the server to
 * write it; zebkit_cdp_listen_async() gives it without waiting.
 */
ZEBKIT_API char *zebkit_cdp_get_url(void);

/* Disconnects every client and stops serving CDP. */
ZEBKIT_API void zebkit_cdp_stop(void);

/* ---- URL schemes of the application's own ---------------------------- */

typedef enum {
    /* Like https:, with a host and a path: an origin of its own, relative
     * URLs, cookies and storage. */
    ZEBKIT_SCHEME_STANDARD = 1 << 0,
    /* A secure context, as https: is. */
    ZEBKIT_SCHEME_SECURE = 1 << 1,
    /* fetch() and XMLHttpRequest from pages, with CORS. */
    ZEBKIT_SCHEME_FETCH = 1 << 2,
    /* Pages' Content-Security-Policy does not restrict loads from it. */
    ZEBKIT_SCHEME_BYPASS_CSP = 1 << 3,
} zebkit_scheme_flags;

/*
 * Makes @scheme ("app", lower case) a URL scheme of the application's, as
 * @flags say. Before zebkit_main(); every process of the engine learns of
 * it. The same from the command line: --zebkit-scheme=app,standard,secure,
 * fetch,bypass-csp. Returns false for a scheme the engine has already.
 */
ZEBKIT_API bool zebkit_scheme_register(const char *scheme, unsigned flags);

typedef struct zebkit_request zebkit_request;
typedef void (*zebkit_scheme_func)(zebkit_request *request, void *data);

/*
 * Answers the requests for @scheme's URLs with @func, on the engine's
 * thread: page loads, subresources, fetch(), media. NULL stops; requests
 * nobody answers fail as not found.
 */
ZEBKIT_API void zebkit_scheme_handle(const char *scheme, zebkit_scheme_func func, void *data);

/* What was asked for: valid until the request is answered. */
ZEBKIT_API const char *zebkit_request_url(zebkit_request *request);
ZEBKIT_API const char *zebkit_request_method(zebkit_request *request);
/* "Name: value" lines separated by "\r\n". */
ZEBKIT_API const char *zebkit_request_headers(zebkit_request *request);
/* The body sent with it, or NULL; *@size is set. */
ZEBKIT_API const uint8_t *zebkit_request_body(zebkit_request *request, size_t *size);

/*
 * Answers @request, from any thread, once: @status, @headers as "Name:
 * value" lines separated by "\r\n" (NULL for none; Content-Type is worth
 * giving), and @size bytes of @body, which are copied. The request is gone
 * after.
 */
ZEBKIT_API void zebkit_request_respond(zebkit_request *request, int status, const char *headers, const void *body,
                                       size_t size);

/*
 * Answers @request with the file at @path, read as it is sent, with the
 * range a Range header asks for (206), as a player seeking in a video
 * asks; 404 when it is not there. Content-Type is taken from the file's
 * extension unless @headers gives one. A path with a ".." component is
 * answered 404 and not opened: an application that makes @path from the
 * request's URL keeps the page inside its directory, but should still
 * check that the path it made is there.
 */
ZEBKIT_API void zebkit_request_respond_file(zebkit_request *request, const char *path, const char *headers);

/* ---- For zebkit-helper ----------------------------------------------- */

/* The renderer, GPU and utility processes. Only zebkit-helper calls it. */
ZEBKIT_API int zebkit_helper_main(int argc, char **argv);

/* Host-owned WebAuthn UI. Register before a request; NULL restores default UI
 * for future requests. This interface is shared by supported desktop platforms.
 * Events are versioned JSON, valid during the callback (see docs/notes/passkeys.md).
 * Calls run on the engine thread. Respond using the request and revision from
 * the latest event. Never log events (QR pairing data) or PIN actions.
 * Removing/replacing a handler cancels its active requests. Hosts must retain
 * their callback data until removal returns; closing a view ends delivery.
 */
typedef void (*zebkit_authentication_func)(zebkit_view *view,
                                          const char *event_json, void *data);
ZEBKIT_API bool zebkit_view_on_authentication(zebkit_view *view,
    zebkit_authentication_func func, void *data);
/* Accepts an action for the current request/revision; false for stale,
 * malformed, unsupported or unavailable actions. A true return queues the
 * action, not an authentication result. Safe from inside the event callback.
 */
ZEBKIT_API bool zebkit_view_authentication_action(zebkit_view *view,
                                                const char *action_json);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_ZEBKIT_H */
