//! Starting the engine, its loop, and running code on its thread.

use crate::{call_once, call_once_for, string_from, sys, Error};
use std::cell::Cell;
use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::time::Duration;

type WakeupFunction = dyn Fn(Duration) + Send + Sync;
static WAKEUP: std::sync::Mutex<Option<std::sync::Arc<WakeupFunction>>> = std::sync::Mutex::new(None);

fn wakeup_slot() -> std::sync::MutexGuard<'static, Option<std::sync::Arc<WakeupFunction>>> {
    // Nothing panics while holding it, but a poisoned lock still holds the function.
    WAKEUP.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

unsafe extern "C" fn on_wakeup(delay_ms: i64, _data: *mut std::ffi::c_void) {
    // Out of the lock while it runs; it may run on any thread, so a panic
    // in it is not resumed (it would be lost there): the process aborts,
    // as the documentation of set_wakeup() says.
    let function = wakeup_slot().clone();
    if let Some(function) = function {
        function(Duration::from_millis(delay_ms.max(0) as u64));
    }
}

/// Configures event-driven pumping before initialization, on the main thread.
/// The callback may run on any engine thread: only queue a host event/timer;
/// never call zebkit or block. Keep the earliest outstanding deadline, then
/// consume it before calling [`pump`]. The callback is retained until shutdown.
/// It must not panic: a panic there aborts the process.
pub fn set_wakeup<F: Fn(Duration) + Send + Sync + 'static>(function: F) -> bool {
    // In place before the engine may call it.
    let previous = wakeup_slot().replace(std::sync::Arc::new(function));
    if !unsafe { sys::zebkit_set_wakeup(Some(on_wakeup), std::ptr::null_mut()) } {
        *wakeup_slot() = previous;
        return false;
    }
    true
}

/// Removes the scheduling callback before startup. Returns false after startup.
pub fn clear_wakeup() -> bool {
    if !unsafe { sys::zebkit_set_wakeup(None, std::ptr::null_mut()) } {
        return false;
    }
    wakeup_slot().take();
    true
}

thread_local! {
    /// Whether this thread is the engine's: the one in [`run`].
    static ENGINE_THREAD: Cell<bool> = const { Cell::new(false) };
    /// Whether [`run`]'s loop is running, which a panic ends.
    static IN_RUN: Cell<bool> = const { Cell::new(false) };
}

/// After a callback panicked ([`crate::guard`]): [`run`]'s loop ends, and
/// resumes the panic; an application's own loop gets it from its next
/// [`pump`] (or [`shutdown`]), which the [`set_wakeup`] function, when there
/// is one, is asked for at once, as for work that is due now.
pub(crate) fn stop_after_panic() {
    if IN_RUN.with(Cell::get) {
        unsafe { sys::zebkit_quit() }
        return;
    }
    // Out of the lock while it runs, as on_wakeup() calls it.
    let function = wakeup_slot().clone();
    if let Some(function) = function {
        function(Duration::ZERO);
    }
}

/// Panics unless this is the engine's thread, for what the engine does on
/// it alone.
pub(crate) fn require_engine_thread(what: &str) {
    assert!(
        ENGINE_THREAD.with(Cell::get),
        "zebkit: {what} requires the engine's initializing thread"
    );
}

/// Built-in cookie key storage, configured before starting the engine.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CookieStorage {
    /// The operating system key store (the default).
    Os,
    /// Zebkit generates/persists the key in a private file, without OS key-store UI.
    File,
}

/// Selects Zebkit's built-in storage backend. `None` places a file key in
/// `<user-data-dir>/Secrets`; a custom directory must be absolute. No host
/// key-file implementation is needed. I/O happens at startup, which returns
/// a nonzero exit code if the key cannot be loaded safely.
pub fn set_cookie_storage(storage: CookieStorage, directory: Option<&std::path::Path>) -> bool {
    let directory = match directory {
        Some(path) => match path.to_str().and_then(|value| CString::new(value).ok()) {
            Some(value) => Some(value),
            None => return false,
        },
        None => None,
    };
    unsafe {
        sys::zebkit_set_cookie_storage(
            match storage {
                CookieStorage::Os => 0,
                CookieStorage::File => 1,
            },
            directory.as_ref().map_or(std::ptr::null(), |value| value.as_ptr()),
        )
    }
}

/// Installs a host-owned AES-256 cookie key before starting the engine.
/// The host must persist/reuse the key. No Keychain/keyring is accessed.
/// Host cookies use a separate database; existing OS-encrypted cookies stay intact.
/// `None` restores the default OS provider. Returns false after startup.
pub fn set_cookie_encryption_key(key: Option<&[u8; 32]>) -> bool {
    unsafe { sys::zebkit_set_cookie_encryption_key(key.map_or(std::ptr::null(), |key| key.as_ptr()), key.map_or(0, |_| 32)) }
}

/// Starts the engine with the program's arguments, calls `ready` once views
/// can be made, and runs until [`quit`]. Returns the exit code for the
/// process. A closure the engine called that panicked ends the loop, and
/// its panic is resumed here.
///
/// Chromium's switches among the arguments are honoured:
/// `--remote-debugging-port=9222` (CDP, as Chrome serves it),
/// `--user-data-dir`, `--headless`, `--no-sandbox`, `--enable-logging`.
/// Call it once, from the program's main thread.
pub fn run<F: FnOnce() + 'static>(ready: F) -> i32 {
    run_with_args(std::env::args_os(), ready)
}

/// Initializes the engine for an application's own loop. Main thread only,
/// once per process. After success, create views and call [`pump`] from a
/// toolkit timer; call [`shutdown`] outside callbacks when finished.
/// Returns Chromium's nonzero startup code on failure.
pub fn init<I>(args: I) -> Result<(), i32>
where
    I: IntoIterator,
    I::Item: Into<std::ffi::OsString>,
{
    let args: Vec<CString> = args.into_iter().map(|arg| os_c_string(arg.into())).collect();
    let mut argv: Vec<*mut c_char> = args.iter().map(|arg| arg.as_ptr().cast_mut()).collect();
    let argc = argv.len() as c_int;
    argv.push(std::ptr::null_mut());
    let code = unsafe { sys::zebkit_init(argc, argv.as_mut_ptr()) };
    if code != 0 {
        return Err(code);
    }
    ENGINE_THREAD.with(|engine| engine.set(true));
    Ok(())
}

/// Processes currently ready engine and native events without waiting.
/// False on the wrong thread, outside the engine's life, or inside a callback.
///
/// # Panics
///
/// With the panic of a closure the engine called, during this pump or
/// since the last one (a closure can be called outside a pump: a page's call
/// answered, a view closed). When it panicked, the [`set_wakeup`] function is
/// asked for a pump at once, so that a host that pumps on its wakeups gets
/// the panic without waiting for other work; a host that pumps on a timer
/// gets it at its next tick. Only the first panic is kept until then: closures
/// that panic after it, before it is resumed, have their panics dropped (each
/// panic's message is still printed by the panic hook as it happens).
pub fn pump() -> bool {
    let pumped = unsafe { sys::zebkit_pump() };
    crate::resume_panic();
    pumped
}

/// Initializes with strict JSON startup settings, overriding matching argv
/// switches. See `<zebkit/zebkit.h>` for the supported fields. Invalid settings
/// are rejected before starting Chromium or opening profile/key/log files.
pub fn init_with_settings<I>(args: I, settings: &str) -> Result<(), Error>
where
    I: IntoIterator,
    I::Item: Into<std::ffi::OsString>,
{
    let settings = CString::new(settings).map_err(|_| Error::new("startup settings contain NUL"))?;
    let args: Vec<CString> = args.into_iter().map(|arg| os_c_string(arg.into())).collect();
    let mut argv: Vec<*mut c_char> = args.iter().map(|arg| arg.as_ptr().cast_mut()).collect();
    let argc = argv.len() as c_int;
    argv.push(std::ptr::null_mut());
    let mut error = std::ptr::null_mut();
    let code = unsafe { sys::zebkit_init_with_settings(argc, argv.as_mut_ptr(), settings.as_ptr(), &mut error) };
    let message = unsafe { crate::take_string(error) };
    if code != 0 {
        return Err(Error::new(message.unwrap_or_else(|| format!("engine startup failed: {code}"))));
    }
    ENGINE_THREAD.with(|engine| engine.set(true));
    Ok(())
}

/// Stops the embedded engine and closes its views, returning to the host.
/// False on the wrong thread, outside the engine's life, or inside a callback.
/// Chromium cannot subsequently be initialized again in this process.
pub fn shutdown() -> bool {
    if !unsafe { sys::zebkit_shutdown() } {
        return false;
    }
    ENGINE_THREAD.with(|engine| engine.set(false));
    wakeup_slot().take();
    crate::view::release_automation_closures();
    crate::resume_panic();
    true
}

/// [`run`] with arguments of the application's choosing; the first is the
/// program's name.
pub fn run_with_args<I, F>(args: I, ready: F) -> i32
where
    I: IntoIterator,
    I::Item: Into<std::ffi::OsString>,
    F: FnOnce() + 'static,
{
    let args: Vec<CString> = args.into_iter().map(|arg| os_c_string(arg.into())).collect();
    let mut argv: Vec<*mut c_char> = args.iter().map(|arg| arg.as_ptr().cast_mut()).collect();
    let argc = argv.len() as c_int;
    argv.push(std::ptr::null_mut());
    let called = std::rc::Rc::new(Cell::new(false));
    let ready_called = called.clone();
    let ready = move || {
        ready_called.set(true);
        ENGINE_THREAD.with(|engine| engine.set(true));
        ready();
    };
    let trampoline = call_once_for(&ready);
    let data = Box::into_raw(Box::new(ready));
    let was_running = IN_RUN.with(|running| running.replace(true));
    let result = unsafe { sys::zebkit_main(argc, argv.as_mut_ptr(), Some(trampoline), data.cast()) };
    IN_RUN.with(|running| running.set(was_running));
    if called.get() {
        ENGINE_THREAD.with(|engine| engine.set(false));
        crate::view::release_automation_closures();
    } else {
        // A failed/rejected startup never transferred the closure to the
        // trampoline. Reclaim it without changing an existing engine's flag.
        drop(unsafe { Box::from_raw(data) });
    }
    crate::resume_panic();
    result
}

#[cfg(unix)]
fn os_c_string(arg: std::ffi::OsString) -> CString {
    use std::os::unix::ffi::OsStringExt;
    let mut bytes = arg.into_vec();
    if let Some(end) = bytes.iter().position(|byte| *byte == 0) {
        bytes.truncate(end);
    }
    CString::new(bytes).expect("no zero byte is left")
}

#[cfg(not(unix))]
fn os_c_string(arg: std::ffi::OsString) -> CString {
    crate::c_string(&arg.to_string_lossy())
}

/// Closes every view and ends [`run`]'s loop, once the current callback
/// has returned.
pub fn quit() {
    require_engine_thread("quit()");
    unsafe { sys::zebkit_quit() }
}

/// Runs `function` on the engine's thread. Any thread may call it. Before
/// the engine has started and once it has stopped, `function` is dropped
/// and never runs.
pub fn post<F: FnOnce() + Send + 'static>(function: F) {
    let data = Box::into_raw(Box::new(function));
    if !unsafe { sys::zebkit_post_task(call_once::<F>, data.cast()) } {
        drop(unsafe { Box::from_raw(data) });
    }
}

/// Runs `function` on the engine's thread `delay` from now. Any thread may
/// call it; as [`post`], it never runs outside the engine's life.
pub fn post_after<F: FnOnce() + Send + 'static>(delay: Duration, function: F) {
    let data = Box::into_raw(Box::new(function));
    if !unsafe { sys::zebkit_post_delayed_task(call_once::<F>, data.cast(), milliseconds(delay)) } {
        drop(unsafe { Box::from_raw(data) });
    }
}

/// Runs `function`, which need not be `Send`, `delay` from now. On the
/// engine's thread only, which it stays on.
pub fn after<F: FnOnce() + 'static>(delay: Duration, function: F) {
    require_engine_thread("after()");
    let data = Box::into_raw(Box::new(function));
    if !unsafe { sys::zebkit_post_delayed_task(call_once::<F>, data.cast(), milliseconds(delay)) } {
        drop(unsafe { Box::from_raw(data) });
    }
}

fn milliseconds(delay: Duration) -> u32 {
    u32::try_from(delay.as_millis()).unwrap_or(u32::MAX)
}

/// Whether views are without windows: there is no display, or the
/// arguments said `--headless`.
pub fn is_headless() -> bool {
    unsafe { sys::zebkit_is_headless() }
}

/// What zebkit decides for the whole engine, while it runs (`<zebkit/hooks.h>`):
/// thirdPartyCookies, spareRenderer, dialogs, downloads, newWindows. All of it
/// applies or none. On the engine's thread.
pub fn set_engine_settings(json: &str) -> Result<(), Error> {
    if json.contains('\0') {
        return Err(Error::new("runtime settings contain NUL"));
    }
    let mut error = std::ptr::null_mut();
    let ok = unsafe { sys::zebkit_set_engine_settings(crate::c_string(json).as_ptr(), &mut error) };
    if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "runtime settings rejected") }) }
}

/// `{"explicit": ..., "effective": ...}`, as JSON; None off the engine's thread.
pub fn engine_settings() -> Option<String> {
    unsafe { crate::take_string(sys::zebkit_get_engine_settings()) }
}

/// zebkit's version, as in `"0.1.0"`.
pub fn version() -> String {
    unsafe { string_from(sys::zebkit_version()) }.unwrap_or_default()
}

/// The engine's, as in `"Chromium 157.0.8103.38"`.
pub fn engine_version() -> String {
    unsafe { string_from(sys::zebkit_engine_version()) }.unwrap_or_default()
}

#[cfg(test)]
mod cookie_storage_tests {
    use super::*;
    #[test]
    fn built_in_selection_rejects_invalid_directories_without_io() {
        assert!(set_cookie_storage(CookieStorage::File, None));
        assert!(!set_cookie_storage(CookieStorage::File, Some(std::path::Path::new("relative"))));
        assert!(!set_cookie_storage(CookieStorage::Os, Some(std::path::Path::new("/unused"))));
        assert!(set_cookie_storage(CookieStorage::Os, None));
    }
}
