//! The engine on Linux: WPE WebKit's C API, on GLib.

mod main_loop;
mod web_view;

pub use main_loop::{timeout, MainLoop};
pub use web_view::{WebView, Window};

use crate::sys::glib;
use crate::{c_string, string_from, Error};
use std::os::raw::c_char;

/// Starts the CDP endpoint; the port, or what the engine said.
pub(crate) fn cdp_listen(address: Option<&str>, port: u16) -> Result<u32, Error> {
    let address = address.map(c_string);
    let mut error: *mut glib::GError = std::ptr::null_mut();
    let port = unsafe {
        crate::sys::cdp::zebkit_cdp_listen(
            address.as_ref().map_or(std::ptr::null(), |text| text.as_ptr()),
            port.into(),
            &mut error,
        )
    };
    if error.is_null() {
        return Ok(port);
    }
    unsafe {
        let message = string_from((*error).message).unwrap_or_default();
        glib::g_error_free(error);
        Err(Error::new(message))
    }
}

pub(crate) fn cdp_set_discovery_enabled(enabled: bool) {
    unsafe { crate::sys::cdp::zebkit_cdp_set_discovery_enabled(enabled.into()) }
}

/// Frees text the engine handed over.
pub(crate) unsafe fn free_text(text: *mut c_char) {
    glib::g_free(text.cast());
}
