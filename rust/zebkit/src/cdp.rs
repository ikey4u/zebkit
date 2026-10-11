//! The Chrome DevTools Protocol endpoint.
//!
//! Once it listens, every [`crate::WebView`] of the process is a page that
//! Puppeteer, Playwright and other CDP clients can drive, at
//! `http://127.0.0.1:<port>`. The main loop has to be running for it to
//! answer. `docs/CDP.md` in the repository says what is covered.
//!
//! The endpoint has no password: anything that can reach the port can
//! drive the pages. It listens on this machine only unless told otherwise.

use crate::sys::cdp;
use crate::sys::glib;
use crate::{c_string, string_from, Error};

/// Starts the endpoint and returns the port it listens on.
///
/// `address` is the interface to listen on; `None` is 127.0.0.1, which is
/// what to use unless the clients are on another machine.
pub fn listen(address: Option<&str>, port: u16) -> Result<u16, Error> {
    let address = address.map(c_string);
    let mut error = std::ptr::null_mut();
    let port = unsafe {
        cdp::zebkit_cdp_listen(
            address.as_ref().map_or(std::ptr::null(), |text| text.as_ptr()),
            port.into(),
            &mut error,
        )
    };
    if !error.is_null() {
        return Err(unsafe { Error::from_glib(error) });
    }
    u16::try_from(port)
        .ok()
        .filter(|port| *port != 0)
        .ok_or_else(|| Error::new("the endpoint did not start"))
}

/// The port the endpoint listens on, if it is running.
pub fn port() -> Option<u16> {
    u16::try_from(unsafe { cdp::zebkit_cdp_get_port() }).ok().filter(|port| *port != 0)
}

/// The WebSocket address of the browser, what Puppeteer calls
/// `browserWSEndpoint`, if the endpoint is running.
pub fn url() -> Option<String> {
    unsafe {
        let text = cdp::zebkit_cdp_get_url();
        let url = string_from(text);
        glib::g_free(text.cast());
        url
    }
}

/// With discovery off, the endpoint does not tell anyone where its sockets
/// are (no `/json/version`, no `/json/list`): only a client that was given
/// [`url`], which holds a random id, can connect. Call before [`listen`].
pub fn set_discovery_enabled(enabled: bool) {
    unsafe { cdp::zebkit_cdp_set_discovery_enabled(enabled.into()) }
}

/// Stops the endpoint: clients are disconnected and the pages the endpoint
/// itself opened are closed. The application's views are left alone.
pub fn stop() {
    unsafe { cdp::zebkit_cdp_stop() }
}
