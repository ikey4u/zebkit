//! zebkit: a web engine to embed.
//!
//! ```no_run
//! use zebkit::{MainLoop, WebView};
//!
//! let main_loop = MainLoop::new();
//! let view = WebView::new();
//! view.load_uri("https://example.com/");
//! main_loop.run();
//! ```
//!
//! - [`WebView`] is a page. It opens a window on Wayland or X11 when the
//!   machine has one; [`WebView::headless`] never does.
//! - [`MainLoop`] runs everything. The engine does its work, and calls the
//!   closures given to it, on the thread that runs the loop.
//! - [`cdp`] starts the Chrome DevTools Protocol endpoint, through which
//!   Puppeteer, Playwright and other clients drive the views.
//! - [`vendor`] is the libraries inside the engine that an application can
//!   use without bringing its own: FFmpeg, so far.
//!
//! This crate is safe Rust over [`zebkit_rs`], the raw bindings, which it
//! re-exports as [`sys`] for whatever is not wrapped here yet.
//!
//! # Threads
//!
//! The engine belongs to one thread: the first that creates a [`WebView`]
//! or a [`MainLoop`]. The types here are neither `Send` nor `Sync`, so the
//! compiler holds a program to that.

pub mod cdp;
pub mod vendor;

mod main_loop;
mod web_view;

pub use main_loop::{timeout, MainLoop};
pub use web_view::{LoadEvent, WebView, Window};

/// The raw bindings this crate is built on.
pub use zebkit_rs as sys;

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

/// An error reported by the engine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    message: String,
}

impl Error {
    pub(crate) fn new(message: impl Into<String>) -> Self {
        Self { message: message.into() }
    }

    /// Takes a `GError` the engine handed over, and frees it.
    pub(crate) unsafe fn from_glib(error: *mut sys::glib::GError) -> Self {
        let message = string_from((*error).message).unwrap_or_default();
        sys::glib::g_error_free(error);
        Self { message }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for Error {}

/// A copy of a C string the engine still owns. `None` for a null pointer.
pub(crate) unsafe fn string_from(text: *const c_char) -> Option<String> {
    (!text.is_null()).then(|| CStr::from_ptr(text).to_string_lossy().into_owned())
}

/// Text for the engine. A zero byte ends a C string, so nothing after one
/// would arrive; the text is cut there.
pub(crate) fn c_string(text: &str) -> CString {
    let end = text.find('\0').unwrap_or(text.len());
    CString::new(&text[..end]).expect("no zero byte is left")
}
