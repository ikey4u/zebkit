//! The engine on macOS: WebKit's own API there, which is Objective-C,
//! through the C functions `zebkit-rs` has for it.

mod main_loop;
mod web_view;

pub use main_loop::{timeout, MainLoop};
pub use web_view::{WebView, Window};

use crate::sys::cocoa;
use crate::{c_string, string_from, Error};
use std::os::raw::c_char;

/// Starts the CDP endpoint; the port, or what the engine said.
pub(crate) fn cdp_listen(address: Option<&str>, port: u16) -> Result<u32, Error> {
    let address = address.map(c_string);
    let mut error: *mut c_char = std::ptr::null_mut();
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
        let message = string_from(error).unwrap_or_default();
        free_text(error);
        Err(Error::new(message))
    }
}

pub(crate) fn cdp_set_discovery_enabled(enabled: bool) {
    unsafe { crate::sys::cdp::zebkit_cdp_set_discovery_enabled(enabled) }
}

/// Frees text the engine handed over.
pub(crate) unsafe fn free_text(text: *mut c_char) {
    cocoa::free(text.cast());
}
