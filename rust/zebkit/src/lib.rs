//! zebkit: a web engine to embed, with Chromium inside.
//!
//! ```no_run
//! use zebkit::View;
//!
//! std::process::exit(zebkit::run(|| {
//!     let view = View::new(Some("https://example.com/"), 1024, 768);
//!     view.on_close(zebkit::quit);
//! }));
//! ```
//!
//! - [`run`] starts the engine and runs its loop; the closure given to it
//!   runs once views can be made. [`quit`] ends the loop.
//! - [`View`] is a page, in a window when the machine has a display, and
//!   without one otherwise or with `--headless`. Its script runs with
//!   [`View::evaluate`]; [`View::grant`] and [`View::bind`] give the page
//!   `zebkit.*` and calls into the application.
//! - [`Script`] is JavaScript outside any page, with all of `zebkit.*`, as
//!   `zebkit-js` runs it.
//! - [`cdp`] serves the Chrome DevTools Protocol, through which Puppeteer,
//!   Playwright and other clients drive the views.
//! - [`services`] is what the engine has for applications besides pages:
//!   compression, digests and encryption, URLs, images and HTTP.
//! - [`vendor`] is the libraries inside the engine: FFmpeg, so far.
//!
//! This crate is safe Rust over [`zebkit_rs`], the raw bindings, which it
//! re-exports as [`sys`] for whatever is not wrapped here yet. The C API it
//! wraps is the same on Linux and macOS, and so is this crate.
//!
//! # Threads
//!
//! The engine has one thread: the one that called [`run`]. Views and
//! scripts are neither `Send` nor `Sync`, so the compiler holds a program to
//! that; [`post`] runs a closure there from any thread.

pub mod cdp;
mod engine;
mod script;
pub mod services;
pub mod vendor;
mod view;

pub use engine::{after, engine_version, is_headless, post, post_after, quit, run, run_with_args, version};
pub use script::Script;
pub use view::{Allow, Call, LoadEvent, View};

/// The raw bindings this crate is built on.
pub use zebkit_rs as sys;

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};

/// An error reported by the engine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    message: String,
}

impl Error {
    pub(crate) fn new(message: impl Into<String>) -> Self {
        Self { message: message.into() }
    }

    /// What the engine said.
    pub fn message(&self) -> &str {
        &self.message
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

/// A copy of a C string the engine gave away, which is then freed.
pub(crate) unsafe fn take_string(text: *mut c_char) -> Option<String> {
    let copy = string_from(text);
    sys::zebkit_free(text.cast());
    copy
}

/// A copy of a buffer the engine gave away, which is then freed.
pub(crate) unsafe fn take_bytes(data: *mut u8, size: usize) -> Vec<u8> {
    let copy = if data.is_null() { Vec::new() } else { std::slice::from_raw_parts(data, size).to_vec() };
    sys::zebkit_free(data.cast());
    copy
}

/// The error a failed call set, or `fallback` when it set none.
pub(crate) unsafe fn take_error(error: *mut c_char, fallback: &str) -> Error {
    Error::new(take_string(error).unwrap_or_else(|| fallback.to_string()))
}

/// Text for the engine. A zero byte ends a C string, so nothing after one
/// would arrive; the text is cut there.
pub(crate) fn c_string(text: &str) -> CString {
    let end = text.find('\0').unwrap_or(text.len());
    CString::new(&text[..end]).expect("no zero byte is left")
}

/// A closure the engine calls once: `data` is `Box::into_raw` of it.
pub(crate) unsafe extern "C" fn call_once<F: FnOnce()>(data: *mut c_void) {
    let function = Box::from_raw(data.cast::<F>());
    function();
}

/// [`call_once`] for the type of `function`, which may have no name.
pub(crate) fn call_once_for<F: FnOnce()>(_function: &F) -> unsafe extern "C" fn(*mut c_void) {
    call_once::<F>
}

/// The value of a script or a call: JSON, or what was thrown.
pub(crate) unsafe fn script_result(json: *const c_char, error: *const c_char) -> Result<String, Error> {
    match string_from(error) {
        Some(message) => Err(Error::new(message)),
        None => Ok(string_from(json).unwrap_or_else(|| "null".to_string())),
    }
}
