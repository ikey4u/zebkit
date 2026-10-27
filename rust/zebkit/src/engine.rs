//! Starting the engine, its loop, and running code on its thread.

use crate::{call_once, call_once_for, string_from, sys};
use std::cell::Cell;
use std::ffi::CString;
use std::os::raw::{c_char, c_int};
use std::time::Duration;

thread_local! {
    /// Whether this thread is the engine's: the one in [`run`].
    static ENGINE_THREAD: Cell<bool> = const { Cell::new(false) };
}

/// Panics unless this is the engine's thread, for what the engine does on
/// it alone.
pub(crate) fn require_engine_thread(what: &str) {
    assert!(
        ENGINE_THREAD.with(Cell::get),
        "zebkit: {what} is for the engine's thread, inside zebkit::run()"
    );
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
/// process.
///
/// Chromium's switches among the arguments are honoured:
/// `--remote-debugging-port=9222` (CDP, as Chrome serves it),
/// `--user-data-dir`, `--headless`, `--no-sandbox`, `--enable-logging`.
/// Call it once, from the program's main thread.
pub fn run<F: FnOnce() + 'static>(ready: F) -> i32 {
    run_with_args(std::env::args_os(), ready)
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
    let ready = move || {
        ENGINE_THREAD.with(|engine| engine.set(true));
        ready();
    };
    let trampoline = call_once_for(&ready);
    let data = Box::into_raw(Box::new(ready));
    unsafe { sys::zebkit_main(argc, argv.as_mut_ptr(), Some(trampoline), data.cast()) }
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

/// Runs `function` on the engine's thread. Any thread may call it.
pub fn post<F: FnOnce() + Send + 'static>(function: F) {
    let data = Box::into_raw(Box::new(function));
    unsafe { sys::zebkit_post_task(call_once::<F>, data.cast()) }
}

/// Runs `function` on the engine's thread `delay` from now. Any thread may
/// call it.
pub fn post_after<F: FnOnce() + Send + 'static>(delay: Duration, function: F) {
    let data = Box::into_raw(Box::new(function));
    unsafe { sys::zebkit_post_delayed_task(call_once::<F>, data.cast(), milliseconds(delay)) }
}

/// Runs `function`, which need not be `Send`, `delay` from now. On the
/// engine's thread only, which it stays on.
pub fn after<F: FnOnce() + 'static>(delay: Duration, function: F) {
    require_engine_thread("after()");
    let data = Box::into_raw(Box::new(function));
    unsafe { sys::zebkit_post_delayed_task(call_once::<F>, data.cast(), milliseconds(delay)) }
}

fn milliseconds(delay: Duration) -> u32 {
    u32::try_from(delay.as_millis()).unwrap_or(u32::MAX)
}

/// Whether views are without windows: there is no display, or the
/// arguments said `--headless`.
pub fn is_headless() -> bool {
    unsafe { sys::zebkit_is_headless() }
}

/// zebkit's version, as in `"0.1.0"`.
pub fn version() -> String {
    unsafe { string_from(sys::zebkit_version()) }.unwrap_or_default()
}

/// The engine's, as in `"Chromium 155.0.8059.84"`.
pub fn engine_version() -> String {
    unsafe { string_from(sys::zebkit_engine_version()) }.unwrap_or_default()
}
