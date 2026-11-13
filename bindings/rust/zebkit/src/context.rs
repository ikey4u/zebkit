//! A browser context of its own (`zebkit_context_new()`): cookies, storage,
//! cache and permissions in memory, as an incognito window's, apart from the
//! engine's profile and from every other context. Playwright's BrowserContext.

use std::cell::Cell;
use std::os::raw::c_int;

use zebkit_rs as sys;

use crate::{c_string, engine::require_engine_thread, Error, View};

/// A context and the views made in it. [`Context::close`] closes them;
/// dropping a context does not.
pub struct Context {
    raw: Cell<*mut sys::zebkit_context>,
}

fn int(value: u32) -> c_int {
    c_int::try_from(value).unwrap_or(c_int::MAX)
}

impl Context {
    /// A new context, on the engine's thread.
    pub fn new() -> Result<Context, Error> {
        require_engine_thread("Context::new()");
        let raw = unsafe { sys::zebkit_context_new() };
        if raw.is_null() {
            return Err(Error::new("the context was not made"));
        }
        Ok(Context { raw: Cell::new(raw) })
    }

    fn raw(&self) -> *mut sys::zebkit_context {
        self.raw.get()
    }

    /// A view in this context, as [`View::new`].
    pub fn new_view(&self, url: Option<&str>, width: u32, height: u32) -> Option<View> {
        require_engine_thread("Context::new_view()");
        if self.raw().is_null() {
            return None;
        }
        let url = url.map(c_string);
        let raw = unsafe {
            sys::zebkit_view_new_in(self.raw(), url.as_ref().map_or(std::ptr::null(), |url| url.as_ptr()), int(width), int(height))
        };
        (!raw.is_null()).then(|| View::wrap(raw))
    }

    /// A view in this context whose window is made with `options_json`, as
    /// [`View::new_with_options`].
    pub fn new_view_with_options(&self, url: Option<&str>, options_json: &str) -> Result<View, Error> {
        require_engine_thread("Context::new_view_with_options()");
        if self.raw().is_null() {
            return Err(Error::new("the context is closed"));
        }
        View::new_with_options_in(self.raw(), url, options_json)
    }

    /// An off-screen view in this context, as [`View::new_offscreen`].
    pub fn new_offscreen_view(&self, url: Option<&str>, width: u32, height: u32, scale: f32) -> Result<View, Error> {
        require_engine_thread("Context::new_offscreen_view()");
        if self.raw().is_null() {
            return Err(Error::new("the context is closed"));
        }
        View::new_offscreen_in(self.raw(), url, width, height, scale)
    }

    /// Closes the context's views; its data is let go when the engine stops.
    pub fn close(&self) {
        let raw = self.raw.replace(std::ptr::null_mut());
        if !raw.is_null() {
            unsafe { sys::zebkit_context_close(raw) }
        }
    }
}
