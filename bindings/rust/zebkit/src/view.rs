//! Views: a page each, in a window or without one.

use crate::engine::require_engine_thread;
use crate::{c_string, script_result, string_from, sys, take_string, Error};
use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ops::BitOr;
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;
use std::rc::Rc;

/// How far a page load has got.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoadEvent {
    /// A load of the page was asked for.
    Started,
    /// The new document replaced the old one.
    Committed,
    /// The page and its resources have loaded.
    Finished,
    /// The load failed or was stopped.
    Failed,
}

/// What a page may use of `zebkit.*`, for [`View::grant`]. Combine with `|`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Allow(u32);

impl Allow {
    /// Nothing: takes a grant back.
    pub const NONE: Allow = Allow(0);
    /// `zebkit.app.<name>()`: the functions [`View::bind`] gives.
    pub const APP: Allow = Allow(sys::ZEBKIT_ALLOW_APP);
    /// `zebkit.fetch()`: HTTP through the engine, without CORS.
    pub const FETCH: Allow = Allow(sys::ZEBKIT_ALLOW_FETCH);
    /// `zebkit.fs`, inside the directory the grant names.
    pub const FILES: Allow = Allow(sys::ZEBKIT_ALLOW_FILES);
    /// `zebkit.compress()` and `zebkit.decompress()`.
    pub const COMPRESS: Allow = Allow(sys::ZEBKIT_ALLOW_COMPRESS);
    /// `zebkit.crypto`.
    pub const CRYPTO: Allow = Allow(sys::ZEBKIT_ALLOW_CRYPTO);
    /// `zebkit.image`.
    pub const IMAGE: Allow = Allow(sys::ZEBKIT_ALLOW_IMAGE);
    /// `zebkit.url`.
    pub const URL: Allow = Allow(sys::ZEBKIT_ALLOW_URL);
    /// `zebkit.pdf`: documents the page opens, in the browser process.
    pub const PDF: Allow = Allow(sys::ZEBKIT_ALLOW_PDF);
    /// All of it.
    pub const ALL: Allow = Allow(sys::ZEBKIT_ALLOW_ALL);
}

impl BitOr for Allow {
    type Output = Allow;
    fn bitor(self, other: Allow) -> Allow {
        Allow(self.0 | other.0)
    }
}

pub(crate) use crate::Slot;
type LoadFunction = dyn FnMut(LoadEvent, &str);

/// The policy for future new windows of one view.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum PopupPolicy {
    BlockWithoutGesture = 0,
    Allow = 1,
    Block = 2,
}

/// A permission decision; Prompt dismisses a request or asks the host for future requests.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum PermissionState { Prompt = 0, Granted = 1, Denied = 2 }

/// A borrowed system window containing a view's page. No ownership transfers.
/// Requery after attaching/detaching. Never destroy or release this handle.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NativeWindow {
    /// X11 window ID.
    X11(usize),
    /// Windows HWND.
    Win32(usize),
    /// AppKit NSWindow* (not the NSView* used for attaching).
    AppKit(usize),
}

/// A decision for one stage of a navigation. Redirect starts a GET in the same frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NavigationAction<'a> { Continue, Cancel, Redirect(&'a str) }

// The view's closures, each in a slot: written once here, so that closing
// the view lets every one of them go.
macro_rules! inner {
    ($($slot:ident: $type:ty,)*) => {
        pub(crate) struct Inner {
            /// Null once the view is closed.
            pub(crate) raw: Cell<*mut sys::zebkit_view>,
            $(pub(crate) $slot: Slot<$type>,)*
            pub(crate) hooks: crate::view_hooks::Slots,
            /// What each bound name calls. Boxed, so that the address the engine
            /// holds stays where it is.
            bindings: RefCell<HashMap<String, Box<Binding>>>,
            /// Bindings replaced or removed, kept until the view closes: one of them
            /// may be running. Boxed: the engine held their addresses.
            #[allow(clippy::vec_box)]
            retired: RefCell<Vec<Box<Binding>>>,
            /// An off-screen view's (offscreen.rs).
            pub(crate) offscreen: crate::offscreen::Slots,
            /// Dialogs, drags (page.rs).
            pub(crate) page: crate::page::Slots,
        }

        impl Inner {
            fn new(raw: *mut sys::zebkit_view) -> Inner {
                Inner {
                    raw: Cell::new(raw),
                    $($slot: Slot::new(),)*
                    hooks: Default::default(),
                    bindings: RefCell::new(HashMap::new()),
                    retired: RefCell::new(Vec::new()),
                    offscreen: Default::default(),
                    page: Default::default(),
                }
            }

            /// Lets every closure go but the automation's (automation.rs):
            /// what they hold may hold the view.
            fn clear(&self) {
                $(if stringify!($slot) != "automation_event" { self.$slot.set(None); })*
                self.hooks.clear();
                let bindings = std::mem::take(&mut *self.bindings.borrow_mut());
                let retired = std::mem::take(&mut *self.retired.borrow_mut());
                drop((bindings, retired));
                self.offscreen.clear();
                self.page.clear();
            }
        }
    };
}

inner! {
    on_load: LoadFunction,
    on_title: dyn FnMut(&str),
    on_close: dyn FnOnce(),
    on_new_window: dyn FnMut(&str),
    on_window_open: dyn FnMut(View, &str),
    on_drop: dyn FnMut(&str),
    automation_event: dyn FnMut(&str, &str),
}

/// The view's state, held while a function the engine called runs: the
/// closure it calls may close the view, letting the last other reference go.
pub(crate) unsafe fn hold(data: *mut c_void) -> Rc<Inner> {
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    Rc::from_raw(pointer)
}

struct Binding {
    function: RefCell<Box<dyn FnMut(Call)>>,
}

/// A page, as Chromium shows it: in a window when the machine has a
/// display, and without one otherwise, or with `--headless`. It loads, runs
/// and can be driven over CDP either way.
///
/// A view lives until it is closed: by [`View::close`], by the user closing
/// its window, by the page's `window.close()`, or by [`crate::quit`]. A
/// clone is the same view. What a closed view is asked to do is not done.
#[derive(Clone)]
pub struct View {
    pub(crate) inner: Rc<Inner>,
}

impl View {
    /// A view loading `url` (`None` for none, until [`View::load_url`]), `width` by `height`
    /// pixels, or a default size when either is 0.
    ///
    /// # Panics
    ///
    /// When the engine makes no view (its window could not be made); use
    /// [`View::try_new`] to get that as an error.
    pub fn new(url: Option<&str>, width: u32, height: u32) -> View {
        match View::try_new(url, width, height) {
            Ok(view) => view,
            Err(error) => panic!("View::new(): {error}"),
        }
    }

    /// [`View::new`], with an error where the engine makes no view.
    pub fn try_new(url: Option<&str>, width: u32, height: u32) -> Result<View, Error> {
        require_engine_thread("View::try_new()");
        let url = url.map(c_string);
        let raw = unsafe {
            sys::zebkit_view_new(
                url.as_ref().map_or(std::ptr::null(), |url| url.as_ptr()),
                c_int::try_from(width).unwrap_or(c_int::MAX),
                c_int::try_from(height).unwrap_or(c_int::MAX),
            )
        };
        if raw.is_null() {
            return Err(Error::new("the engine made no view (its window could not be made)"));
        }
        Ok(View::wrap(raw))
    }

    /// A view whose window is made with `options_json` (`<zebkit/window.h>`):
    /// its frame (`"system"`, `"overlay"`, `"none"`), whether it starts shown,
    /// its place, colours and drag regions. Invalid options: an error, and no view.
    pub fn new_with_options(url: Option<&str>, options_json: &str) -> Result<View, Error> {
        require_engine_thread("View::new_with_options()");
        View::new_with_options_in(std::ptr::null_mut(), url, options_json)
    }

    /// [`View::new_with_options`] in `context` (null: the engine's own).
    pub(crate) fn new_with_options_in(context: *mut sys::zebkit_context, url: Option<&str>, options_json: &str) -> Result<View, Error> {
        let url = url.map(c_string);
        let mut error = std::ptr::null_mut();
        let raw = unsafe {
            sys::zebkit_view_new_with_options(
                context,
                url.as_ref().map_or(std::ptr::null(), |url| url.as_ptr()),
                c_string(options_json).as_ptr(),
                &mut error,
            )
        };
        if raw.is_null() {
            return Err(unsafe { crate::take_error(error, "the window options were refused") });
        }
        Ok(View::wrap(raw))
    }

    /// The window's state as JSON: state, bounds, frame, titlebarArea, controls, capabilities.
    pub fn window(&self) -> Option<String> {
        self.raw().and_then(|raw| unsafe { take_string(sys::zebkit_view_get_window(raw)) })
    }

    /// Changes the window's runtime options, merged by key; the creation ones
    /// are `requires-recreate`.
    pub fn set_window(&self, options_json: &str) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_set_window(raw, c_string(options_json).as_ptr(), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "window options rejected") }) }
    }

    /// `show`, `hide`, `minimize`, `maximize`, `restore`, `fullscreen`,
    /// `exit-fullscreen`, `close`: asked of the system; the state tells.
    pub fn window_action(&self, action: &str) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_window_action(raw, c_string(action).as_ptr(), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "window action refused") }) }
    }


    /// With `dragRegions.source` `"host"`: `[{"x", "y", "width", "height", "drag"}]`.
    pub fn set_drag_regions(&self, regions_json: &str) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_set_drag_regions(raw, c_string(regions_json).as_ptr(), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "drag regions rejected") }) }
    }

    /// Options for the windows this view's pages open; `None`: the system's frame.
    pub fn set_popup_window_options(&self, options_json: Option<&str>) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let options = options_json.map(c_string);
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_view_set_popup_window_options(raw, options.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()), &mut error)
        };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "popup window options rejected") }) }
    }

    /// The view the engine made, with the callbacks every view has.
    pub(crate) fn wrap(raw: *mut sys::zebkit_view) -> View {
        let inner = Rc::new(Inner::new(raw));
        // The engine holds one reference until the view closes.
        let data = Rc::into_raw(inner.clone()).cast_mut().cast::<c_void>();
        unsafe {
            sys::zebkit_view_on_load(raw, Some(on_load), data);
            sys::zebkit_view_on_title(raw, Some(on_title), data);
            sys::zebkit_view_on_close(raw, Some(on_close), data);
        }
        View { inner }
    }

    pub(crate) fn raw(&self) -> Option<*mut sys::zebkit_view> {
        let raw = self.inner.raw.get();
        (!raw.is_null()).then_some(raw)
    }

    /// Whether the view is closed.
    pub fn is_closed(&self) -> bool {
        self.raw().is_none()
    }

    pub fn load_url(&self, url: &str) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_load_url(raw, c_string(url).as_ptr()) }
        }
    }

    pub fn reload(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_reload(raw) }
        }
    }

    pub fn go_back(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_go_back(raw) }
        }
    }

    pub fn go_forward(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_go_forward(raw) }
        }
    }

    pub fn resize(&self, width: u32, height: u32) {
        if let Some(raw) = self.raw() {
            unsafe {
                sys::zebkit_view_resize(
                    raw,
                    c_int::try_from(width).unwrap_or(c_int::MAX),
                    c_int::try_from(height).unwrap_or(c_int::MAX),
                )
            }
        }
    }

    /// The smallest the window may be made (0 for no limit), and whether it
    /// may be resized: Electron's `minWidth`, `minHeight` and `resizable`.
    pub fn set_size_limits(&self, min_width: u32, min_height: u32, resizable: bool) {
        if let Some(raw) = self.raw() {
            unsafe {
                sys::zebkit_view_set_size_limits(
                    raw,
                    c_int::try_from(min_width).unwrap_or(c_int::MAX),
                    c_int::try_from(min_height).unwrap_or(c_int::MAX),
                    resizable,
                )
            }
        }
    }

    /// Closes the view, once the current callback has returned; the
    /// [`View::on_close`] closure is called then.
    pub fn close(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_close(raw) }
        }
    }

    /// The page's URL.
    pub fn url(&self) -> Option<String> {
        self.raw().and_then(|raw| unsafe { take_string(sys::zebkit_view_get_url(raw)) })
    }

    /// The page's title.
    pub fn title(&self) -> Option<String> {
        self.raw().and_then(|raw| unsafe { take_string(sys::zebkit_view_get_title(raw)) })
    }

    /// Calls `function` with each step of every load and the URL it is of.
    /// Replaces what was given before.
    pub fn on_load<F: FnMut(LoadEvent, &str) + 'static>(&self, function: F) {
        self.inner.on_load.set(Some(Box::new(function)));
    }

    /// Calls `function` with the page's title whenever it changes.
    pub fn on_title<F: FnMut(&str) + 'static>(&self, function: F) {
        self.inner.on_title.set(Some(Box::new(function)));
    }

    /// Removes the [`View::on_load`] closure, including from inside it.
    pub fn clear_load_handler(&self) {
        self.inner.on_load.set(None);
    }

    /// Removes the [`View::on_title`] closure, including from inside it.
    pub fn clear_title_handler(&self) {
        self.inner.on_title.set(None);
    }

    /// Calls `function` when the view closes, whatever closed it: the last
    /// thing the view does.
    pub fn on_close<F: FnOnce() + 'static>(&self, function: F) {
        self.inner.on_close.set(Some(Box::new(function)));
    }

    /// The title of the view's window; `None` for the page's.
    pub fn set_title(&self, title: Option<&str>) {
        let Some(raw) = self.raw() else { return };
        let title = title.map(c_string);
        unsafe { sys::zebkit_view_set_title(raw, title.as_ref().map_or(std::ptr::null(), |t| t.as_ptr())) }
    }

    /// Brings the view's window to the front, with the keyboard.
    pub fn focus(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_focus(raw) }
        }
    }

    /// Places this view inside `parent`'s window, over its page, at
    /// (x, y), `width` by `height`; its own window is hidden. What the
    /// engine refuses is ignored: [`View::try_attach`] says.
    pub fn attach(&self, parent: &View, x: i32, y: i32, width: i32, height: i32) {
        let _ = self.try_attach(parent, x, y, width, height);
    }

    /// [`View::attach`], failing for a closed view or parent, the view
    /// itself as `parent`, or a parent without a window to be in.
    pub fn try_attach(&self, parent: &View, x: i32, y: i32, width: i32, height: i32) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let parent = parent.raw().ok_or_else(|| Error::new("the parent view is closed"))?;
        if unsafe { sys::zebkit_view_attach(raw, parent, x, y, width, height) } {
            Ok(())
        } else {
            Err(Error::new("the view was not attached: it is the parent, or the parent has no window to be in"))
        }
    }

    /// Places this view's window inside a window of the application's:
    /// `native_parent` is an X11 window id, on macOS an `NSView*`. Fails,
    /// naming off-screen views, on Wayland and under `--headless`.
    pub fn attach_native(&self, native_parent: usize, x: i32, y: i32, width: i32, height: i32) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_attach_native(raw, native_parent, x, y, width, height, &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "the window cannot be attached") }) }
    }

    /// The page's window as the system's portals name a parent window,
    /// `"x11:<XID>"` or `"wayland:<handle>"` (exported through xdg-foreign),
    /// for a portal's dialog to be the window's own. `done` runs once, later,
    /// on the engine's thread: `None` when there is none (headless,
    /// off-screen, no xdg-foreign; always on Windows and macOS, where
    /// [`View::native_window`] is the handle), and for a view that closes
    /// first, before its [`View::on_close`] function.
    pub fn export_native_window<F: FnOnce(Option<String>) + 'static>(&self, done: F) {
        let Some(raw) = self.raw() else {
            done(None);
            return;
        };
        let data = Box::into_raw(Box::new(done));
        if !unsafe { sys::zebkit_view_export_native_window(raw, Some(on_native_window_export::<F>), data.cast()) } {
            let done = unsafe { Box::from_raw(data) };
            done(None);
        }
    }

    /// Borrow the containing system window, or None when no supported window
    /// exists (headless, off-screen, Wayland or a closed view).
    pub fn native_window(&self) -> Option<NativeWindow> {
        let raw = self.raw()?;
        let mut info = sys::zebkit_native_window::default();
        if !unsafe { sys::zebkit_view_get_native_window(raw, &mut info) } { return None; }
        match info.kind {
            sys::ZEBKIT_NATIVE_WINDOW_X11 => Some(NativeWindow::X11(info.handle)),
            sys::ZEBKIT_NATIVE_WINDOW_WIN32 => Some(NativeWindow::Win32(info.handle)),
            sys::ZEBKIT_NATIVE_WINDOW_APPKIT => Some(NativeWindow::AppKit(info.handle)),
            _ => None,
        }
    }

    /// Makes the view its own window again; false when it was in none of the
    /// application's.
    pub fn detach_native(&self) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_detach_native(raw) })
    }

    /// Where an attached view is in its parent's window.
    pub fn set_bounds(&self, x: i32, y: i32, width: i32, height: i32) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_bounds(raw, x, y, width, height) }
        }
    }

    /// Shows or hides an attached view.
    pub fn set_visible(&self, visible: bool) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_visible(raw, visible) }
        }
    }

    /// Runs `source` in every document the view loads from now on, before
    /// the page's own script (a preload).
    pub fn add_script(&self, source: &str) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_add_script(raw, c_string(source).as_ptr()) }
        }
    }

    pub fn stop(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_stop(raw) }
        }
    }

    pub fn is_loading(&self) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_is_loading(raw) })
    }

    /// The User-Agent of the view's requests and pages; `None` for the engine's.
    pub fn set_user_agent(&self, user_agent: Option<&str>) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_user_agent(raw, c_string(user_agent.unwrap_or("")).as_ptr()) }
        }
    }

    /// Sets the UA string and optional client hints using CDP parameter JSON.
    /// Completion reports Chromium validation; rejected updates do not persist.
    /// True when it was taken: `done` is then called exactly once, after this
    /// returns, with `{}` or the error (options it cannot read, a NUL in
    /// them, the view closing first). False, and `done` never called, only
    /// when the engine refused it at once: a closed view (a `View` is not
    /// `Send`, so never the wrong thread). Every binding has this rule, the
    /// engine's.
    pub fn set_user_agent_override<F: FnOnce(Result<String, Error>) + 'static>(&self, options: &str, done: F) -> bool {
        let Some(raw) = self.raw() else { return false };
        let Ok(options) = std::ffi::CString::new(options) else {
            // Invalid input, as the engine tells it: once, after this returns.
            crate::after(std::time::Duration::ZERO, move || done(Err(Error::new("user agent options must not contain a literal NUL"))));
            return true;
        };
        // The engine gets a number, not the closure: an answer after a refusal,
        // or a second one, finds nothing to call.
        let id = USER_AGENT_DONE.with(|pending| {
            let id = pending.next.get().wrapping_add(1).max(1);
            pending.next.set(id);
            pending.waiting.borrow_mut().insert(id, Box::new(done));
            id
        });
        let outer = USER_AGENT_DONE.with(|pending| pending.calling.replace(id));
        let queued = unsafe {
            sys::zebkit_view_set_user_agent_override(raw, options.as_ptr(), Some(on_user_agent_done), id as *mut c_void)
        };
        let early = USER_AGENT_DONE.with(|pending| {
            pending.calling.set(outer);
            pending.early.borrow_mut().take()
        });
        if !queued {
            // Refused at once: done is never called, whatever the engine told.
            let refused = USER_AGENT_DONE.with(|pending| pending.waiting.borrow_mut().remove(&id));
            drop(refused);
        } else if let Some(result) = early {
            // Told inside the call: told to done after it has returned.
            if let Some(done) = USER_AGENT_DONE.with(|pending| pending.waiting.borrow_mut().remove(&id)) {
                crate::after(std::time::Duration::ZERO, move || done(result));
            }
        }
        queued
    }

    /// A PNG of what the view shows: the engine's bytes, for as long as
    /// `done` runs (copy them to keep them).
    pub fn capture<F: FnOnce(Result<&[u8], Error>) + 'static>(&self, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_capture(raw, on_capture::<F>, data.cast()) }
    }

    /// Restores normal Chromium popup creation after a URL-only handler.
    pub fn clear_new_window_handler(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_on_new_window(raw, None, std::ptr::null_mut()) }
        }
        self.inner.on_new_window.set(None);
    }

    /// The CDP target ID of this existing view; does not navigate or recreate it.
    pub fn target_id(&self) -> Option<String> {
        self.raw()
            .and_then(|raw| unsafe { crate::take_string(sys::zebkit_view_get_target_id(raw)) })
    }

    /// What a page opens in a new window comes here as its URL, and no
    /// window is made.
    pub fn on_new_window<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.on_new_window.set(Some(Box::new(function)));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_new_window(raw, Some(on_new_window), data) }
    }

    /// Merge validated native preferences. Null values reset individual keys.
    pub fn set_settings(&self, json: &str) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        if json.contains('\0') { return Err(Error::new("view settings contain NUL")); }
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_set_settings(raw, c_string(json).as_ptr(), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "view settings rejected") }) }
    }

    /// Explicit native preferences, as JSON; temporary CDP emulation is separate.
    pub fn settings(&self) -> Option<String> {
        self.raw().and_then(|raw| unsafe { take_string(sys::zebkit_view_get_settings(raw)) })
    }

    /// What zebkit decides for this view, while it runs (`<zebkit/hooks.h>`):
    /// thirdPartyCookies, dialogs, downloads, newWindows. All of it applies or none.
    pub fn set_runtime_settings(&self, json: &str) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        if json.contains('\0') { return Err(Error::new("runtime settings contain NUL")); }
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_set_runtime_settings(raw, c_string(json).as_ptr(), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "runtime settings rejected") }) }
    }

    /// `{"explicit": ..., "effective": ...}`, as JSON.
    pub fn runtime_settings(&self) -> Option<String> {
        self.raw().and_then(|raw| unsafe { take_string(sys::zebkit_view_get_runtime_settings(raw)) })
    }

    /// Set a view rule. None uses the current origin; Some("*") sets a default.
    pub fn set_permission(&self, permission: &str, state: PermissionState, origin: Option<&str>) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        if permission.contains('\0') || origin.is_some_and(|s| s.contains('\0')) { return Err(Error::new("permission name or origin contains NUL")); }
        let origin = origin.map(c_string);
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_set_permission(raw, origin.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()), c_string(permission).as_ptr(), state as i32, &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "permission rule rejected") }) }
    }
    /// Effective policy for a concrete origin. Document security checks still apply.
    pub fn permission(&self, permission: &str, origin: Option<&str>) -> Option<PermissionState> {
        let raw = self.raw()?;
        if permission.contains('\0') || origin.is_some_and(|s| s.contains('\0')) { return None; }
        let origin = origin.map(c_string);
        match unsafe { sys::zebkit_view_get_permission(raw, origin.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()), c_string(permission).as_ptr()) } {
            0 => Some(PermissionState::Prompt), 1 => Some(PermissionState::Granted), 2 => Some(PermissionState::Denied), _ => None,
        }
    }
    /// Remove all view rules, or only the supplied origin/default rules.
    pub fn reset_permissions(&self, origin: Option<&str>) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        if origin.is_some_and(|s| s.contains('\0')) { return Err(Error::new("permission origin contains NUL")); }
        let origin = origin.map(c_string);
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_view_reset_permissions(raw, origin.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "permission reset rejected") }) }
    }
    /// Deadline for future prompts: 1..=300000 milliseconds.
    pub fn set_permission_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_permission_timeout(raw, milliseconds) })
    }
    /// Deadline for future document certificate requests, from 1 to 300000 ms.
    pub fn set_certificate_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_certificate_timeout(raw, milliseconds) })
    }
    /// Deadline for future HTTP authentication challenges, from 1 to 300000 ms.
    pub fn set_http_auth_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_http_auth_timeout(raw, milliseconds) })
    }
    /// Deadline for future navigation stages, from 1 to 300000 milliseconds.
    pub fn set_navigation_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_navigation_timeout(raw, milliseconds) })
    }
    /// With true, an unanswered navigation is cancelled instead of continued.
    pub fn set_navigation_fail_closed(&self, fail_closed: bool) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_navigation_fail_closed(raw, fail_closed) })
    }

    /// Creation JSON for a popup, or an empty JSON object for an ordinary view.
    pub fn window_open_info(&self) -> Option<String> {
        self.raw().and_then(|raw| unsafe { take_string(sys::zebkit_view_get_window_open_info(raw)) })
    }

    /// Changes future popup decisions immediately, independently of other views.
    pub fn set_popup_policy(&self, policy: PopupPolicy) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_popup_policy(raw, policy as i32) })
    }

    /// The effective policy, including the startup switch until explicitly set.
    pub fn popup_policy(&self) -> Option<PopupPolicy> {
        self.raw().map(|raw| match unsafe { sys::zebkit_view_get_popup_policy(raw) } {
            1 => PopupPolicy::Allow,
            2 => PopupPolicy::Block,
            _ => PopupPolicy::BlockWithoutGesture,
        })
    }


    /// A window a page opens comes here as a view of its own, with the URL
    /// it opened, still linked to the page that opened it (`window.opener`,
    /// `postMessage()`, `window.closed`), as a site's sign-in through a
    /// popup needs. Off-screen when this view is. The application shows it
    /// and closes it; [`View::on_new_window`], when set, takes the URL
    /// instead.
    pub fn on_window_open<F: FnMut(View, &str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.on_window_open.set(Some(Box::new(function)));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_window_open(raw, Some(on_window_open), data) }
    }

    /// Removes the [`View::on_window_open`] closure: windows the page opens
    /// are the engine's again.
    pub fn clear_window_open_handler(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_on_window_open(raw, None, std::ptr::null_mut()) }
        }
        self.inner.on_window_open.set(None);
    }

    /// The paths of files dropped on the view, as a JSON array, before the
    /// page gets the drop.
    pub fn on_drop<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.on_drop.set(Some(Box::new(function)));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_drop(raw, Some(on_drop), data) }
    }

    /// Removes the [`View::on_drop`] closure, including from inside it.
    pub fn clear_drop_handler(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_on_drop(raw, None, std::ptr::null_mut()) }
        }
        self.inner.on_drop.set(None);
    }

    /// Runs `script` in the page's main frame, as the page's own script
    /// would run, and calls `done` with its value as JSON, or with what it
    /// threw. A promise is waited for.
    pub fn evaluate<F: FnOnce(Result<String, Error>) + 'static>(&self, script: &str, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_evaluate(raw, c_string(script).as_ptr(), Some(on_result::<F>), data.cast()) }
    }

    /// Gives pages of `origin` in this view what `allow` says of
    /// `zebkit.*` ([`Allow::NONE`] takes it back). `origin` is as a browser
    /// writes it, as in `"https://app.example"`, or `"file://"`, or `"*"`
    /// for every origin, which is for pages the application wrote itself.
    /// `files_root` is the directory `zebkit.fs` works in.
    pub fn grant(&self, origin: &str, allow: Allow, files_root: Option<&Path>) {
        let Some(raw) = self.raw() else { return };
        let root = files_root.map(|root| c_string(&root.to_string_lossy()));
        unsafe {
            sys::zebkit_view_grant(
                raw,
                c_string(origin).as_ptr(),
                allow.0,
                root.as_ref().map_or(std::ptr::null(), |root| root.as_ptr()),
            )
        }
    }

    /// Makes `zebkit.app.<name>(...)` in pages granted [`Allow::APP`] call
    /// `function`, which answers the page's promise through the [`Call`].
    pub fn bind<F: FnMut(Call) + 'static>(&self, name: &str, function: F) {
        let Some(raw) = self.raw() else { return };
        let binding = Box::new(Binding { function: RefCell::new(Box::new(function)) });
        let data = (&*binding as *const Binding).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_bind(raw, c_string(name).as_ptr(), Some(on_call), data) }
        if let Some(old) = self.inner.bindings.borrow_mut().insert(name.to_string(), binding) {
            self.inner.retired.borrow_mut().push(old);
        }
    }

    /// Takes `zebkit.app.<name>` away.
    pub fn unbind(&self, name: &str) {
        let Some(raw) = self.raw() else { return };
        unsafe { sys::zebkit_view_bind(raw, c_string(name).as_ptr(), None, std::ptr::null_mut()) }
        if let Some(old) = self.inner.bindings.borrow_mut().remove(name) {
            self.inner.retired.borrow_mut().push(old);
        }
    }
}

/// A call from a page: `zebkit.app.<name>(...args)`. Answer it, now or
/// later, from any thread, with [`Call::resolve`] or [`Call::reject`]; one that is dropped
/// unanswered is freed (`zebkit_call_free`), which rejects the page's
/// promise with "the application dropped the call".
pub struct Call {
    raw: *mut sys::zebkit_call,
    origin: String,
    args: String,
}

// The engine takes a call's answer, or its zebkit_call_free(), from any
// thread; an answer consumes the Call, so only one of them reaches it.
unsafe impl Send for Call {}

impl Call {
    /// The origin of the page that called.
    pub fn origin(&self) -> &str {
        &self.origin
    }

    /// The arguments.
    pub fn args(&self) -> Vec<crate::json::Value> {
        match crate::json::Value::parse(&self.args) {
            Ok(crate::json::Value::Array(args)) => args,
            _ => Vec::new(),
        }
    }

    /// The arguments as the page sent them: a JSON array.
    pub fn args_json(&self) -> &str {
        &self.args
    }

    /// Resolves the page's promise with `value`.
    pub fn resolve_value(self, value: &crate::json::Value) {
        self.resolve(&value.to_string());
    }

    /// Resolves the page's promise with `json`, a JSON value.
    pub fn resolve(mut self, json: &str) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        // With its length: no copy of it to end it in a NUL.
        unsafe { sys::zebkit_call_resolve_json(raw, json.as_ptr().cast(), json.len()) }
    }

    /// Resolves the page's promise with `undefined`.
    pub fn resolve_undefined(mut self) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        unsafe { sys::zebkit_call_resolve(raw, std::ptr::null()) }
    }

    /// Rejects the page's promise with an `Error` of `message`.
    pub fn reject(mut self, message: &str) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        unsafe { sys::zebkit_call_reject(raw, c_string(message).as_ptr()) }
    }
}

impl Drop for Call {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            // Answered calls are consumed (their pointer taken), so only an
            // unanswered one is freed, once: the page's promise is rejected.
            unsafe { sys::zebkit_call_free(self.raw) }
        }
    }
}

/// Calls the closure in `slot` without holding it borrowed, so that the
/// closure may replace or remove it ([`Slot::call`]).
pub(crate) fn call_slot<T: ?Sized>(slot: &Slot<T>, call: impl FnOnce(&mut T)) {
    slot.call(call);
}

unsafe extern "C" fn on_load(
    _view: *mut sys::zebkit_view,
    event: sys::zebkit_load_event,
    url: *const c_char,
    data: *mut c_void,
) {
    let inner = hold(data);
    let event = match event {
        sys::ZEBKIT_LOAD_STARTED => LoadEvent::Started,
        sys::ZEBKIT_LOAD_COMMITTED => LoadEvent::Committed,
        sys::ZEBKIT_LOAD_FINISHED => LoadEvent::Finished,
        _ => LoadEvent::Failed,
    };
    let url = string_from(url).unwrap_or_default();
    inner.on_load.call(|function| function(event, &url));
}

unsafe extern "C" fn on_title(_view: *mut sys::zebkit_view, title: *const c_char, data: *mut c_void) {
    let inner = hold(data);
    let title = string_from(title).unwrap_or_default();
    inner.on_title.call(|function| function(&title));
}

unsafe extern "C" fn on_close(_view: *mut sys::zebkit_view, data: *mut c_void) {
    // The engine's reference, which this is the last use of.
    let inner = Rc::from_raw(data.cast_const().cast::<Inner>());
    inner.raw.set(std::ptr::null_mut());
    if let Some(function) = inner.on_close.take() {
        crate::guard(function);
    }
    inner.clear();
    // The page's last automation events can come after this, until the task
    // that closed the view has ended (<zebkit/automation.h>): the closure
    // and what it is called with go in a task of their own, or, when the
    // engine stops before that task runs, once it has stopped
    // (release_automation_closures()).
    if inner.automation_event.is_set() {
        let id = CLOSING_AUTOMATION.with(|closing| {
            let mut closing = closing.borrow_mut();
            closing.0 += 1;
            let id = closing.0;
            closing.1.push((id, inner));
            id
        });
        unsafe extern "C" fn release(data: *mut c_void) {
            let id = data as usize;
            let found = CLOSING_AUTOMATION.with(|closing| {
                let mut closing = closing.borrow_mut();
                let at = closing.1.iter().position(|(entry, _)| *entry == id);
                at.map(|at| closing.1.swap_remove(at).1)
            });
            if let Some(inner) = found {
                inner.automation_event.set(None);
            }
        }
        if !sys::zebkit_post_task(release, id as *mut c_void) {
            release(id as *mut c_void);
        }
    }
}

/// The views closed whose automation closure waits for its task: the last
/// id given, and the views by id.
type ClosingAutomation = (usize, Vec<(usize, Rc<Inner>)>);

thread_local! {
    static CLOSING_AUTOMATION: RefCell<ClosingAutomation> = const { RefCell::new((0, Vec::new())) };
}

/// Lets go the automation closures of closed views whose task the engine
/// did not run before it stopped: called once it has (shutdown(), the end
/// of run()). What those closures hold is dropped then, after the engine.
pub(crate) fn release_automation_closures() {
    let closing = CLOSING_AUTOMATION.with(|closing| std::mem::take(&mut closing.borrow_mut().1));
    for (_, inner) in closing {
        inner.automation_event.set(None);
    }
}

unsafe extern "C" fn on_capture<F: FnOnce(Result<&[u8], Error>)>(
    _view: *mut sys::zebkit_view,
    png: *const u8,
    size: usize,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    let result = if png.is_null() {
        Err(Error::new(string_from(error).unwrap_or_else(|| "no picture".to_string())))
    } else {
        Ok(std::slice::from_raw_parts(png, size))
    };
    crate::guard(|| done(result));
}

unsafe extern "C" fn on_native_window_export<F: FnOnce(Option<String>)>(
    _view: *mut sys::zebkit_view,
    handle: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    let handle = string_from(handle);
    crate::guard(|| done(handle));
}

unsafe extern "C" fn on_new_window(_view: *mut sys::zebkit_view, url: *const c_char, data: *mut c_void) {
    let inner = hold(data);
    let url = string_from(url).unwrap_or_default();
    inner.on_new_window.call(|function| function(&url));
}

unsafe extern "C" fn on_window_open(
    _opener: *mut sys::zebkit_view,
    opened: *mut sys::zebkit_view,
    url: *const c_char,
    data: *mut c_void,
) {
    let inner = hold(data);
    let url = string_from(url).unwrap_or_default();
    let opened = View::wrap(opened);
    inner.on_window_open.call(|function| function(opened.clone(), &url));
}

unsafe extern "C" fn on_drop(_view: *mut sys::zebkit_view, paths: *const c_char, data: *mut c_void) {
    let inner = hold(data);
    let paths = string_from(paths).unwrap_or_else(|| "[]".to_string());
    inner.on_drop.call(|function| function(&paths));
}

unsafe extern "C" fn on_result<F: FnOnce(Result<String, Error>)>(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    let result = script_result(json, error);
    crate::guard(|| done(result));
}

type UserAgentDone = Box<dyn FnOnce(Result<String, Error>)>;

#[derive(Default)]
struct PendingUserAgents {
    next: Cell<usize>,
    waiting: RefCell<HashMap<usize, UserAgentDone>>,
    /// The request inside its call now: an answer then is held, and told
    /// once the call has returned (and only if it was queued).
    calling: Cell<usize>,
    early: RefCell<Option<Result<String, Error>>>,
}

thread_local! {
    static USER_AGENT_DONE: PendingUserAgents = PendingUserAgents::default();
}

unsafe extern "C" fn on_user_agent_done(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let id = data as usize;
    let result = script_result(json, error);
    let told_inside = USER_AGENT_DONE.with(|pending| {
        if pending.calling.get() == id && pending.waiting.borrow().contains_key(&id) {
            *pending.early.borrow_mut() = Some(result.clone());
            true
        } else {
            false
        }
    });
    if told_inside {
        return;
    }
    let Some(done) = USER_AGENT_DONE.with(|pending| pending.waiting.borrow_mut().remove(&id)) else { return };
    crate::guard(|| done(result));
}

unsafe extern "C" fn on_call(
    _view: *mut sys::zebkit_view,
    origin: *const c_char,
    args_json: *const c_char,
    call: *mut sys::zebkit_call,
    data: *mut c_void,
) {
    let binding = &*data.cast::<Binding>();
    let call = Call {
        raw: call,
        origin: string_from(origin).unwrap_or_default(),
        args: string_from(args_json).unwrap_or_else(|| "[]".to_string()),
    };
    match binding.function.try_borrow_mut() {
        // A call a panicking function drops is rejected.
        Ok(mut function) => {
            crate::guard(|| function(call));
        }
        // The function called the page, which called it again.
        Err(_) => call.reject("the application's function is already running"),
    }
}
