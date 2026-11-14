//! The page's `zebkit` in the system's web view (`<zebkit/viewer.h>`,
//! docs/VIEWER.md): for an application on Android or iOS that drives
//! WKWebView or Android's WebView from Rust. The web view carries the
//! messages; a [`Viewer`] answers them as zebkit's views would. The Swift
//! and Java bindings (`ZebkitViewer`) are built on the same functions.

use crate::{c_string, sys, take_string};
use std::ptr;

pub use crate::view::Allow;

/// What a page's posted call came to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Handled {
    /// JavaScript to run in the frame that called: it answers the call.
    Answer(String),
    /// The application's call (`zebkit.app`, `zebkit.fetch`), as JSON
    /// `{"id", "module", "method", "args"}`: answer it with [`answer`].
    Application(String),
    /// Not a call of the viewer's script; nothing to answer.
    Nothing,
}

/// A viewer: grants, and the documents its pages opened.
pub struct Viewer {
    raw: *mut sys::zebkit_viewer,
}

// libzebkit's viewer takes a lock of its own: any thread, one call at a time
// per document.
unsafe impl Send for Viewer {}
unsafe impl Sync for Viewer {}

impl Viewer {
    pub fn new() -> Viewer {
        Viewer { raw: unsafe { sys::zebkit_viewer_new() } }
    }

    /// Gives pages of `origin` (or `"*"` for the application's own) what
    /// `allow` says; an empty `allow` takes it back.
    pub fn grant(&self, origin: &str, allow: Allow, files_root: Option<&str>) {
        let origin = c_string(origin);
        let root = files_root.map(c_string);
        unsafe {
            sys::zebkit_viewer_grant(
                self.raw,
                origin.as_ptr(),
                allow.bits(),
                root.as_ref().map_or(ptr::null(), |r| r.as_ptr()),
            )
        }
    }

    /// Closes the documents the viewer's pages opened (a new document).
    pub fn reset(&self) {
        unsafe { sys::zebkit_viewer_reset(self.raw) }
    }

    /// One string a page posted, from a frame of `origin`.
    pub fn handle(&self, origin: &str, message: &str) -> Handled {
        let origin = c_string(origin);
        let message = c_string(message);
        let mut app: *mut std::os::raw::c_char = ptr::null_mut();
        let answer = unsafe {
            take_string(sys::zebkit_viewer_handle(self.raw, origin.as_ptr(), message.as_ptr(), &mut app))
        };
        match (answer, unsafe { take_string(app) }) {
            (Some(script), _) => Handled::Answer(script),
            (None, Some(call)) => Handled::Application(call),
            (None, None) => Handled::Nothing,
        }
    }
}

impl Default for Viewer {
    fn default() -> Self {
        Viewer::new()
    }
}

impl Drop for Viewer {
    fn drop(&mut self) {
        unsafe { sys::zebkit_viewer_free(self.raw) }
    }
}

/// The script to add before each document's own: `post` is the source of a
/// JavaScript function that posts one string to the application.
pub fn script(post: &str) -> String {
    let post = c_string(post);
    unsafe { take_string(sys::zebkit_viewer_script(post.as_ptr())) }.unwrap_or_default()
}

/// The JavaScript that answers the application's call `id`: `Ok` with a JSON
/// value (bytes as `{"$bytes": base64}`; `None` for undefined), `Err` with a
/// message.
pub fn answer(id: i64, result: Result<Option<&str>, &str>) -> String {
    let (ok, text) = match result {
        Ok(json) => (true, json.map(c_string)),
        Err(message) => (false, Some(c_string(message))),
    };
    unsafe {
        take_string(sys::zebkit_viewer_answer(id, ok, text.as_ref().map_or(ptr::null(), |t| t.as_ptr())))
    }
    .unwrap_or_default()
}
