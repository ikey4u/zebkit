//! The Chrome DevTools Protocol, as Chrome serves it.
//!
//! Once it listens, every [`crate::View`] of the process is a page that
//! Puppeteer, Playwright and other CDP clients can drive, at
//! `http://127.0.0.1:<port>`. The `--remote-debugging-port` argument to
//! [`crate::run`] starts it too. It is Chromium's own DevTools server
//! (`docs/ARCHITECTURE.md` in the repository).
//!
//! The endpoint has no password: anything that can reach the port can
//! drive the pages. It listens on this machine only unless told otherwise.

use std::ffi::{c_char, c_uint, c_void};

use crate::engine::require_engine_thread;
use crate::{c_string, string_from, sys, take_error, take_string, Error};

/// Starts the endpoint and returns the port it listens on; if it already
/// runs, its port (another `address` or `port` then is an error). Waits on
/// the engine's thread for the server to bind: [`listen_async`] does not.
///
/// `address` is the interface to listen on, a numeric IP address; `None`
/// is 127.0.0.1, which is what to use unless the clients are on another
/// machine. `port` 0 lets the system choose.
pub fn listen(address: Option<&str>, port: u16) -> Result<u16, Error> {
    require_engine_thread("cdp::listen()");
    let address = address.map(c_string);
    let mut error = std::ptr::null_mut();
    let port = unsafe {
        sys::zebkit_cdp_listen(
            address.as_ref().map_or(std::ptr::null(), |address| address.as_ptr()),
            u32::from(port),
            &mut error,
        )
    };
    if port == 0 {
        return Err(unsafe { take_error(error, "the endpoint did not start") });
    }
    u16::try_from(port).map_err(|_| Error::new("the endpoint reported no port"))
}

unsafe extern "C" fn on_listen<F: FnOnce(Result<(u16, String), Error>)>(
    port: c_uint,
    url: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let function = Box::from_raw(data.cast::<F>());
    let result = match u16::try_from(port) {
        Ok(port) if port != 0 => Ok((port, string_from(url).unwrap_or_default())),
        _ => Err(Error::new(string_from(error).unwrap_or_else(|| "the endpoint did not start".into()))),
    };
    crate::guard(|| function(result));
}

/// [`listen`] without waiting for the server's thread: `done` gets the port
/// and the browser's WebSocket address (as [`url`] gives it), or the error,
/// once, on the engine's thread, after this returns. [`stop`] while it
/// starts answers with an error.
pub fn listen_async<F: FnOnce(Result<(u16, String), Error>) + 'static>(address: Option<&str>, port: u16, done: F) {
    require_engine_thread("cdp::listen_async()");
    let address = address.map(c_string);
    let data = Box::into_raw(Box::new(done)).cast::<c_void>();
    unsafe {
        sys::zebkit_cdp_listen_async(
            address.as_ref().map_or(std::ptr::null(), |address| address.as_ptr()),
            u32::from(port),
            Some(on_listen::<F>),
            data,
        )
    }
}

/// The port the endpoint listens on, if it is running.
pub fn port() -> Option<u16> {
    u16::try_from(unsafe { sys::zebkit_cdp_get_port() }).ok().filter(|port| *port != 0)
}

/// The WebSocket address of the browser, what Puppeteer calls
/// `browserWSEndpoint`, if the endpoint is running.
pub fn url() -> Option<String> {
    unsafe { take_string(sys::zebkit_cdp_get_url()) }
}

/// Disconnects every client and stops the endpoint.
pub fn stop() {
    require_engine_thread("cdp::stop()");
    unsafe { sys::zebkit_cdp_stop() }
}
