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
//!   compression, digests and encryption, URLs, images and HTTP; [`canvas`]
//!   draws on the engine's Skia and shapes text on its HarfBuzz; [`pdf`]
//!   is PDF documents on the engine's PDFium.
//! - [`system`] is what the system tells of itself and does for the
//!   application: screens, power, sleep, notifications, the system's
//!   handlers; [`Tray`] is the tray icon; [`app`] the menu, Dock and taskbar.
//! - [`scheme`] makes URL schemes of the application's own, which it
//!   answers: its interface and its files, without a server.
//! - [`vendor`] is the libraries inside the engine: FFmpeg, so far.
//! - [`offscreen`] is views without a window, whose frames the application
//!   draws itself (shared surfaces: IOSurfaces, shared memory, dmabufs) and
//!   whose input it sends: [`View::new_offscreen`].
//! - [`page`] is what a page asks of its window: JavaScript dialogs, held
//!   for the application to answer ([`View::on_dialog`]); the page printed
//!   to PDF ([`View::print_to_pdf`]); drag and drop, from the page and into
//!   it ([`View::on_drag_start`], [`View::drop_data`]).
//!
//! - [`automation`] is Playwright's API on a view's page, inside the
//!   engine: pages, frames, locators, handles, routes, dialogs, downloads,
//!   workers, tracing, `expect`; [`json`] reads and writes the JSON the
//!   engine speaks; [`Bytes`] is a buffer the engine made, not copied.
//!
//! This crate is safe Rust over [`zebkit_rs`], the raw bindings, which it
//! re-exports as [`sys`] for whatever is not wrapped here yet. The C API it
//! wraps is the same on Linux, macOS and Windows, and so is this crate.
//!
//! # Threads
//!
//! The engine has one thread: the one that called [`run`]. Views and
//! scripts are neither `Send` nor `Sync`, so the compiler holds a program to
//! that; [`post`] runs a closure there from any thread.
//!
//! # Panics in callbacks
//!
//! A closure the engine calls that panics does not unwind into the engine:
//! the panic is kept, the loop is asked to end ([`quit`]), and [`run`] (or
//! [`pump`], for an application's own loop) resumes it once the engine has
//! returned.

pub mod app;
pub mod automation;
pub mod canvas;
pub mod cdp;
mod context;
pub mod json;
mod engine;
pub mod media;
pub mod media_tools;
pub mod offscreen;
pub mod page;
pub mod pdf;
pub mod scheme;
mod script;
pub mod services;
pub mod system;
mod tray;
pub mod vendor;
mod view;

pub use engine::{
    after, clear_wakeup, engine_settings, engine_version, init, init_with_settings, is_headless, post, post_after, pump, quit, run, run_with_args,
    set_cookie_encryption_key, set_cookie_storage, set_engine_settings, set_wakeup, shutdown, version, CookieStorage,
};
pub use script::Script;

pub use context::Context;
pub use media::{MediaCapture, MediaTap};
mod view_hooks;
pub use tray::Tray;
pub use view_hooks::{
    CertificateRequest, ClientCertificateAction, ClientCertificateRequest, DeviceChooserRequest, DownloadRequest, EyeDropperRequest,
    FileChooserRequest, FullscreenPolicy, HttpAuthRequest, MediaAccessRequest, NavigationRequest, NetworkRequest, NetworkStream,
    PermissionRequest, ShareRequest,
};
/// On Windows, with the static C runtime (`+crt-static`): this executable as the
/// engine's sandboxed child process, which Chromium's sandbox there needs. Call it
/// first in `main`; `Some(code)` when this process was a child and has finished,
/// to exit with. Without it the engine runs only with `--no-sandbox`
/// (`docs/WINDOWS.md`).
#[cfg(all(windows, target_feature = "crt-static"))]
pub fn win_startup() -> Option<i32> {
    let mut code = 0;
    // SAFETY: called with a place for the code; the function takes nothing else.
    unsafe { zebkit_rs::zebkit_win_startup(&mut code) }.then_some(code)
}

pub use view::{Allow, Call, LoadEvent, NativeWindow, NavigationAction, PermissionState, PopupPolicy, View};

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

/// A buffer the engine gave away, kept as it is: freed when dropped.
pub(crate) unsafe fn take_bytes(data: *mut u8, size: usize) -> Bytes {
    if data.is_null() {
        return Bytes(Repr::Vec(Vec::new()));
    }
    Bytes(Repr::Engine(data, size))
}

/// Bytes the engine made (pixels, files, compressed or decrypted data): its
/// own buffer, not copied, given back when this is dropped; or bytes of the
/// application's (`Bytes::from(vec)`). Use it as a `&[u8]`;
/// [`Bytes::into_vec`] copies an engine's buffer into a `Vec`.
#[derive(Default)]
pub struct Bytes(Repr);

enum Repr {
    Engine(*mut u8, usize),
    Vec(Vec<u8>),
}

impl Default for Repr {
    fn default() -> Self {
        Repr::Vec(Vec::new())
    }
}

// The engine's buffer is plain memory of its allocator, freed from any thread.
unsafe impl Send for Bytes {}
unsafe impl Sync for Bytes {}

impl Bytes {
    /// The bytes in a `Vec`: the application's as they are, the engine's copied.
    pub fn into_vec(mut self) -> Vec<u8> {
        match std::mem::take(&mut self.0) {
            Repr::Vec(vec) => vec,
            Repr::Engine(data, size) => {
                let vec = unsafe { std::slice::from_raw_parts(data, size) }.to_vec();
                unsafe { sys::zebkit_free(data.cast()) };
                vec
            }
        }
    }
}

impl Drop for Bytes {
    fn drop(&mut self) {
        if let Repr::Engine(data, _) = self.0 {
            unsafe { sys::zebkit_free(data.cast()) }
        }
    }
}

impl std::ops::Deref for Bytes {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        match &self.0 {
            Repr::Engine(data, size) => unsafe { std::slice::from_raw_parts(*data, *size) },
            Repr::Vec(vec) => vec,
        }
    }
}

impl std::ops::DerefMut for Bytes {
    fn deref_mut(&mut self) -> &mut [u8] {
        match &mut self.0 {
            Repr::Engine(data, size) => unsafe { std::slice::from_raw_parts_mut(*data, *size) },
            Repr::Vec(vec) => vec,
        }
    }
}

impl AsRef<[u8]> for Bytes {
    fn as_ref(&self) -> &[u8] {
        self
    }
}

impl From<Vec<u8>> for Bytes {
    fn from(vec: Vec<u8>) -> Bytes {
        Bytes(Repr::Vec(vec))
    }
}

impl From<Bytes> for Vec<u8> {
    fn from(bytes: Bytes) -> Vec<u8> {
        bytes.into_vec()
    }
}

impl Clone for Bytes {
    /// A copy in a `Vec`.
    fn clone(&self) -> Bytes {
        Bytes(Repr::Vec(self.to_vec()))
    }
}

impl std::fmt::Debug for Bytes {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "Bytes({} bytes)", self.len())
    }
}

impl PartialEq for Bytes {
    fn eq(&self, other: &Bytes) -> bool {
        **self == **other
    }
}

impl Eq for Bytes {}

impl PartialEq<[u8]> for Bytes {
    fn eq(&self, other: &[u8]) -> bool {
        **self == *other
    }
}

impl PartialEq<&[u8]> for Bytes {
    fn eq(&self, other: &&[u8]) -> bool {
        **self == **other
    }
}

impl PartialEq<Vec<u8>> for Bytes {
    fn eq(&self, other: &Vec<u8>) -> bool {
        **self == **other
    }
}

impl<const N: usize> PartialEq<[u8; N]> for Bytes {
    fn eq(&self, other: &[u8; N]) -> bool {
        **self == other[..]
    }
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
    guard(function);
}

thread_local! {
    /// The first panic of a closure the engine called, for [`resume_panic`].
    static PANIC: std::cell::RefCell<Option<Box<dyn std::any::Any + Send>>> = const { std::cell::RefCell::new(None) };
}

/// Runs a closure of the application's from a function the engine called.
/// A panic does not unwind into C, which would abort: it is kept, the
/// engine's loop is asked to end (with [`run`]) or the host's wakeup is asked
/// for a pump (with [`init`]), and [`run`], [`pump`] or [`shutdown`]
/// resumes it once the engine has returned. Only the first panic is kept
/// until then; later ones are dropped. `None` when it panicked.
pub(crate) fn guard<R>(function: impl FnOnce() -> R) -> Option<R> {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(function)) {
        Ok(value) => Some(value),
        Err(panic) => {
            let first = PANIC.with(|slot| {
                let mut slot = slot.borrow_mut();
                let first = slot.is_none();
                if first {
                    *slot = Some(panic);
                }
                first
            });
            if first {
                engine::stop_after_panic();
            }
            None
        }
    }
}

/// Resumes the panic [`guard`] kept, outside the engine.
pub(crate) fn resume_panic() {
    if let Some(panic) = PANIC.with(|slot| slot.borrow_mut().take()) {
        std::panic::resume_unwind(panic);
    }
}

/// A closure the engine calls, which may replace or remove itself while it
/// runs: it is out of its slot then, and goes back only when nothing was
/// set meanwhile (the generation counts what was).
pub(crate) struct Slot<T: ?Sized> {
    function: std::cell::RefCell<Option<Box<T>>>,
    generation: std::cell::Cell<u64>,
}

impl<T: ?Sized> Default for Slot<T> {
    fn default() -> Self {
        Slot::new()
    }
}

impl<T: ?Sized> Slot<T> {
    pub(crate) const fn new() -> Self {
        Slot { function: std::cell::RefCell::new(None), generation: std::cell::Cell::new(0) }
    }

    /// Replaces the closure (`None` removes it). The old one is dropped
    /// after the slot is let go, so that its drop may use the slot.
    pub(crate) fn set(&self, function: Option<Box<T>>) {
        self.generation.set(self.generation.get().wrapping_add(1));
        let old = self.function.replace(function);
        drop(old);
    }

    /// Takes the closure out for good, as [`Slot::set`] with `None`.
    pub(crate) fn take(&self) -> Option<Box<T>> {
        self.generation.set(self.generation.get().wrapping_add(1));
        self.function.borrow_mut().take()
    }

    pub(crate) fn is_set(&self) -> bool {
        self.function.borrow().is_some()
    }

    /// Calls the closure, if there is one, through [`guard`]; `None` when
    /// there is none or it panicked.
    pub(crate) fn call<R>(&self, call: impl FnOnce(&mut T) -> R) -> Option<R> {
        let generation = self.generation.get();
        let mut function = self.function.borrow_mut().take()?;
        let result = guard(|| call(&mut function));
        if self.generation.get() == generation {
            *self.function.borrow_mut() = Some(function);
        }
        result
    }
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

#[cfg(test)]
mod tests {
    use super::{guard, resume_panic, Slot};
    use std::cell::Cell;
    use std::rc::Rc;

    #[test]
    fn a_panic_is_kept_and_resumed_outside() {
        assert_eq!(guard(|| 1), Some(1));
        assert_eq!(guard(|| -> i32 { panic!("in a callback") }), None);
        let resumed = std::panic::catch_unwind(resume_panic).expect_err("the kept panic");
        assert_eq!(resumed.downcast_ref::<&str>(), Some(&"in a callback"));
        // Once only.
        resume_panic();
    }

    #[test]
    fn a_slot_removed_or_replaced_inside_its_call_stays_so() {
        let slot: Rc<Slot<dyn FnMut()>> = Rc::new(Slot::new());
        let calls = Rc::new(Cell::new(0));
        let (inside, counted) = (slot.clone(), calls.clone());
        slot.set(Some(Box::new(move || {
            counted.set(counted.get() + 1);
            inside.set(None);
        })));
        slot.call(|function| function());
        assert!(!slot.is_set());
        let (inside, counted) = (slot.clone(), calls.clone());
        slot.set(Some(Box::new(move || {
            let again = counted.clone();
            inside.set(Some(Box::new(move || again.set(again.get() + 10))));
        })));
        slot.call(|function| function());
        slot.call(|function| function());
        assert_eq!(calls.get(), 11);
        // A closure that is kept is put back.
        slot.call(|function| function());
        assert_eq!(calls.get(), 21);
        slot.set(None);
    }
}
