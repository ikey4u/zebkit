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

pub(crate) type Slot<T> = RefCell<Option<Box<T>>>;
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

/// An engine-thread permission request. Clones refer to the same single-use prompt.
/// Holding this object does not extend its deadline or the document lifetime.
#[derive(Clone)]
pub struct PermissionRequest {
    raw: *mut sys::zebkit_permission_request,
    metadata: String,
    _thread: std::marker::PhantomData<Rc<()>>,
}
impl PermissionRequest {
    /// Original request metadata as JSON, retained after the prompt is settled.
    pub fn metadata(&self) -> &str { &self.metadata }
    /// Current metadata, or None once answered, expired or cancelled.
    pub fn info(&self) -> Option<String> {
        require_engine_thread("PermissionRequest::info()");
        unsafe { take_string(sys::zebkit_permission_request_info(self.raw)) }
    }
    /// Reply once, optionally storing a view rule for the requesting origin.
    /// False means the request was already settled or the reply was invalid.
    pub fn respond(&self, state: PermissionState, remember: bool) -> bool {
        require_engine_thread("PermissionRequest::respond()");
        unsafe { sys::zebkit_permission_respond(self.raw, state as i32, remember) }
    }
}

/// A decision for one stage of a navigation. Redirect starts a GET in the same frame.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NavigationAction<'a> { Continue, Cancel, Redirect(&'a str) }

/// An engine-thread navigation request. Clones share a single-use, expiring token.
#[derive(Clone)]
pub struct NavigationRequest {
    raw: *mut sys::zebkit_navigation_request,
    metadata: String,
    _thread: std::marker::PhantomData<Rc<()>>,
}
impl NavigationRequest {
    /// Original JSON metadata, retained after settlement.
    pub fn metadata(&self) -> &str { &self.metadata }
    /// Current JSON metadata, or None after settlement, cancellation or timeout.
    pub fn info(&self) -> Option<String> {
        require_engine_thread("NavigationRequest::info()");
        unsafe { take_string(sys::zebkit_navigation_request_info(self.raw)) }
    }
    /// Reply once. Invalid URLs and stale or expired requests return an error.
    pub fn respond(&self, action: NavigationAction<'_>) -> Result<(), Error> {
        require_engine_thread("NavigationRequest::respond()");
        let (kind, replacement) = match action {
            NavigationAction::Continue => (sys::ZEBKIT_NAVIGATION_CONTINUE, None),
            NavigationAction::Cancel => (sys::ZEBKIT_NAVIGATION_CANCEL, None),
            NavigationAction::Redirect(url) => {
                if url.contains('\0') { return Err(Error::new("replacement URL contains NUL")); }
                (sys::ZEBKIT_NAVIGATION_REDIRECT, Some(c_string(url)))
            }
        };
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_navigation_respond(self.raw, kind,
            replacement.as_ref().map_or(std::ptr::null(), |url| url.as_ptr()), &mut error) };
        if ok { Ok(()) } else { Err(unsafe { crate::take_error(error, "navigation reply rejected") }) }
    }
}

/// An engine-thread document certificate error with a single-use, expiring token.
#[derive(Clone)]
pub struct CertificateRequest {
    raw: *mut sys::zebkit_certificate_request,
    metadata: String,
    _thread: std::marker::PhantomData<Rc<()>>,
}
impl CertificateRequest {
    /// Original JSON, retained after the request is settled.
    pub fn metadata(&self) -> &str { &self.metadata }
    /// Current JSON, or None after reply, expiration or cancellation.
    pub fn info(&self) -> Option<String> {
        require_engine_thread("CertificateRequest::info()");
        unsafe { take_string(sys::zebkit_certificate_request_info(self.raw)) }
    }
    /// Reply once. False means stale, expired or a forbidden strict-error bypass.
    pub fn respond(&self, allow: bool) -> bool {
        require_engine_thread("CertificateRequest::respond()");
        unsafe { sys::zebkit_certificate_respond(self.raw, allow) }
    }
}

/// An expiring engine-thread HTTP/proxy challenge. Credentials are not retained.
#[derive(Clone)]
pub struct HttpAuthRequest {
    raw: *mut sys::zebkit_http_auth_request,
    metadata: String,
    _thread: std::marker::PhantomData<Rc<()>>,
}
impl HttpAuthRequest {
    /// Original metadata JSON, retained after settlement.
    pub fn metadata(&self) -> &str { &self.metadata }
    /// Current JSON, or None after reply, expiration or cancellation.
    pub fn info(&self) -> Option<String> {
        require_engine_thread("HttpAuthRequest::info()");
        unsafe { take_string(sys::zebkit_http_auth_request_info(self.raw)) }
    }
    /// Provide credentials once; false means the request has already settled.
    pub fn provide(&self, username: &str, password: &str) -> Result<bool, Error> {
        require_engine_thread("HttpAuthRequest::provide()");
        if username.len() > 65536 || password.len() > 65536 {
            return Err(Error::new("each HTTP authentication credential is limited to 65536 UTF-8 bytes"));
        }
        let username = std::ffi::CString::new(username)
            .map_err(|_| Error::new("HTTP authentication credentials cannot contain NUL"))?;
        let password = std::ffi::CString::new(password)
            .map_err(|_| Error::new("HTTP authentication credentials cannot contain NUL"))?;
        Ok(unsafe { sys::zebkit_http_auth_respond(self.raw, username.as_ptr(), password.as_ptr()) })
    }
    /// Cancel once. Keeping this object does not extend the deadline.
    pub fn cancel(&self) -> bool {
        require_engine_thread("HttpAuthRequest::cancel()");
        unsafe { sys::zebkit_http_auth_respond(self.raw, std::ptr::null(), std::ptr::null()) }
    }
}

pub(crate) struct Inner {
    /// Null once the view is closed.
    pub(crate) raw: Cell<*mut sys::zebkit_view>,
    on_load: Slot<LoadFunction>,
    on_title: Slot<dyn FnMut(&str)>,
    on_close: Slot<dyn FnOnce()>,
    on_new_window: Slot<dyn FnMut(&str)>,
    on_window_open: Slot<dyn FnMut(View, &str)>,
    on_popup_blocked: Slot<dyn FnMut(&str)>,
    on_permission: Slot<dyn FnMut(PermissionRequest)>,
    permission_generation: Cell<u64>,
    on_permission_result: Slot<dyn FnMut(&str)>,
    permission_result_generation: Cell<u64>,
    on_navigation: Slot<dyn FnMut(NavigationRequest)>,
    navigation_generation: Cell<u64>,
    on_navigation_result: Slot<dyn FnMut(&str)>,
    navigation_result_generation: Cell<u64>,
    on_window_open_event: Slot<dyn FnMut(&str)>,
    window_open_event_generation: Cell<u64>,
    on_console: Slot<dyn FnMut(&str)>,
    console_generation: Cell<u64>,
    on_certificate: Slot<dyn FnMut(CertificateRequest)>,
    certificate_generation: Cell<u64>,
    on_certificate_result: Slot<dyn FnMut(&str)>,
    certificate_result_generation: Cell<u64>,
    pub(crate) hooks: crate::view_hooks::Slots,
    on_http_auth: Slot<dyn FnMut(HttpAuthRequest)>,
    http_auth_generation: Cell<u64>,
    on_http_auth_result: Slot<dyn FnMut(&str)>,
    http_auth_result_generation: Cell<u64>,
    on_drop: Slot<dyn FnMut(&str)>,
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
    pub fn new(url: Option<&str>, width: u32, height: u32) -> View {
        require_engine_thread("View::new()");
        let url = url.map(c_string);
        let raw = unsafe {
            sys::zebkit_view_new(
                url.as_ref().map_or(std::ptr::null(), |url| url.as_ptr()),
                c_int::try_from(width).unwrap_or(c_int::MAX),
                c_int::try_from(height).unwrap_or(c_int::MAX),
            )
        };
        View::wrap(raw)
    }

    /// The view the engine made, with the callbacks every view has.
    pub(crate) fn wrap(raw: *mut sys::zebkit_view) -> View {
        let inner = Rc::new(Inner {
            raw: Cell::new(raw),
            on_load: RefCell::new(None),
            on_title: RefCell::new(None),
            on_close: RefCell::new(None),
            on_new_window: RefCell::new(None),
            on_window_open: RefCell::new(None),
            on_popup_blocked: RefCell::new(None),
            on_permission: RefCell::new(None),
            permission_generation: Cell::new(0),
            on_permission_result: RefCell::new(None),
            permission_result_generation: Cell::new(0),
            on_navigation: RefCell::new(None),
            navigation_generation: Cell::new(0),
            on_navigation_result: RefCell::new(None),
            navigation_result_generation: Cell::new(0),
            on_window_open_event: RefCell::new(None),
            window_open_event_generation: Cell::new(0),
            on_console: RefCell::new(None),
            console_generation: Cell::new(0),
            on_certificate: RefCell::new(None),
            certificate_generation: Cell::new(0),
            on_certificate_result: RefCell::new(None),
            certificate_result_generation: Cell::new(0),
            hooks: Default::default(),
            on_http_auth: RefCell::new(None),
            http_auth_generation: Cell::new(0),
            on_http_auth_result: RefCell::new(None),
            http_auth_result_generation: Cell::new(0),
            on_drop: RefCell::new(None),
            bindings: RefCell::new(HashMap::new()),
            retired: RefCell::new(Vec::new()),
            offscreen: Default::default(),
            page: Default::default(),
        });
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
        *self.inner.on_load.borrow_mut() = Some(Box::new(function));
    }

    /// Calls `function` with the page's title whenever it changes.
    pub fn on_title<F: FnMut(&str) + 'static>(&self, function: F) {
        *self.inner.on_title.borrow_mut() = Some(Box::new(function));
    }

    /// Calls `function` when the view closes, whatever closed it: the last
    /// thing the view does.
    pub fn on_close<F: FnOnce() + 'static>(&self, function: F) {
        *self.inner.on_close.borrow_mut() = Some(Box::new(function));
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
    /// (x, y), `width` by `height`; its own window is hidden.
    pub fn attach(&self, parent: &View, x: i32, y: i32, width: i32, height: i32) {
        if let (Some(raw), Some(parent)) = (self.raw(), parent.raw()) {
            unsafe { sys::zebkit_view_attach(raw, parent, x, y, width, height) }
        }
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
    pub fn set_user_agent_override<F: FnOnce(Result<String, Error>) + 'static>(&self, options: &str, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let Ok(options) = std::ffi::CString::new(options) else {
            done(Err(Error::new("user agent options must not contain a literal NUL")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_set_user_agent_override(raw, options.as_ptr(), Some(on_result::<F>), data.cast()); }
    }

    /// A PNG of what the view shows.
    pub fn capture<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, done: F) {
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
        self.inner.on_new_window.borrow_mut().take();
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
        *self.inner.on_new_window.borrow_mut() = Some(Box::new(function));
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
    /// Handle permission prompts. Replacing the handler settles its pending prompts.
    pub fn on_permission<F: FnMut(PermissionRequest) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.permission_generation.set(self.inner.permission_generation.get().wrapping_add(1));
        *self.inner.on_permission.borrow_mut() = Some(Box::new(function));
        // Native callback/data remain the same when only the Rust closure changes.
        unsafe { sys::zebkit_view_on_permission(raw, None, std::ptr::null_mut()); }
        // Clearing settles pending prompts, whose handlers can close the view.
        let Some(raw) = self.raw() else { return };
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_permission(raw, Some(on_permission), data); }
    }
    /// Restore default permission handling and settle this handler's pending prompts.
    pub fn clear_permission_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.permission_generation.set(self.inner.permission_generation.get().wrapping_add(1));
        self.inner.on_permission.borrow_mut().take();
        unsafe { sys::zebkit_view_on_permission(raw, None, std::ptr::null_mut()); }
    }
    /// Observe asynchronous permission outcomes as JSON.
    pub fn on_permission_result<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.permission_result_generation.set(self.inner.permission_result_generation.get().wrapping_add(1));
        *self.inner.on_permission_result.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_permission_result(raw, Some(on_permission_result), data); }
    }
    /// Remove the permission outcome observer, including from inside that observer.
    pub fn clear_permission_result_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.permission_result_generation.set(self.inner.permission_result_generation.get().wrapping_add(1));
        self.inner.on_permission_result.borrow_mut().take();
        unsafe { sys::zebkit_view_on_permission_result(raw, None, std::ptr::null_mut()); }
    }

    /// Deadline for future document certificate requests, from 1 to 300000 ms.
    pub fn set_certificate_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_certificate_timeout(raw, milliseconds) })
    }
    /// Handle document certificate errors; the default is denial.
    pub fn on_certificate<F: FnMut(CertificateRequest) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.certificate_generation.set(self.inner.certificate_generation.get().wrapping_add(1));
        *self.inner.on_certificate.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_certificate(raw, None, std::ptr::null_mut()); }
        // Cancelling a pending TLS request can synchronously close the view.
        let Some(raw) = self.raw() else { return };
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_certificate(raw, Some(on_certificate), data); }
    }
    /// Remove the handler and deny its outstanding requests.
    pub fn clear_certificate_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.certificate_generation.set(self.inner.certificate_generation.get().wrapping_add(1));
        self.inner.on_certificate.borrow_mut().take();
        unsafe { sys::zebkit_view_on_certificate(raw, None, std::ptr::null_mut()); }
    }
    /// Observe document certificate outcomes as JSON.
    pub fn on_certificate_result<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.certificate_result_generation.set(self.inner.certificate_result_generation.get().wrapping_add(1));
        *self.inner.on_certificate_result.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_certificate_result(raw, Some(on_certificate_result), data); }
    }
    /// Remove the result observer, including inside its callback.
    pub fn clear_certificate_result_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.certificate_result_generation.set(self.inner.certificate_result_generation.get().wrapping_add(1));
        self.inner.on_certificate_result.borrow_mut().take();
        unsafe { sys::zebkit_view_on_certificate_result(raw, None, std::ptr::null_mut()); }
    }

    /// Deadline for future HTTP authentication challenges, from 1 to 300000 ms.
    pub fn set_http_auth_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_http_auth_timeout(raw, milliseconds) })
    }
    /// Own HTTP/proxy challenges at the browser login delegate.
    pub fn on_http_auth<F: FnMut(HttpAuthRequest) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.http_auth_generation.set(self.inner.http_auth_generation.get().wrapping_add(1));
        *self.inner.on_http_auth.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_http_auth(raw, None, std::ptr::null_mut()); }
        let Some(raw) = self.raw() else { return };
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_http_auth(raw, Some(on_http_auth), data); }
    }
    /// Cancel pending challenges and remove the handler, including from its callback.
    pub fn clear_http_auth_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.http_auth_generation.set(self.inner.http_auth_generation.get().wrapping_add(1));
        self.inner.on_http_auth.borrow_mut().take();
        unsafe { sys::zebkit_view_on_http_auth(raw, None, std::ptr::null_mut()); }
    }
    /// Observe credential provision/cancellation as JSON, not HTTP success.
    pub fn on_http_auth_result<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.http_auth_result_generation.set(self.inner.http_auth_result_generation.get().wrapping_add(1));
        *self.inner.on_http_auth_result.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_http_auth_result(raw, Some(on_http_auth_result), data); }
    }
    /// Remove the outcome observer and cancel queued deliveries.
    pub fn clear_http_auth_result_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.http_auth_result_generation.set(self.inner.http_auth_result_generation.get().wrapping_add(1));
        self.inner.on_http_auth_result.borrow_mut().take();
        unsafe { sys::zebkit_view_on_http_auth_result(raw, None, std::ptr::null_mut()); }
    }

    /// Deadline for future navigation stages, from 1 to 300000 milliseconds.
    pub fn set_navigation_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_navigation_timeout(raw, milliseconds) })
    }
    /// With true, an unanswered navigation is cancelled instead of continued.
    pub fn set_navigation_fail_closed(&self, fail_closed: bool) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_navigation_fail_closed(raw, fail_closed) })
    }
    /// Decide document navigations at start, redirects, response, failure or loader-less commit.
    /// Unanswered stages continue. Replacing the handler continues its pending stages.
    pub fn on_navigation<F: FnMut(NavigationRequest) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.navigation_generation.set(self.inner.navigation_generation.get().wrapping_add(1));
        *self.inner.on_navigation.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_navigation(raw, None, std::ptr::null_mut()); }
        // Clearing settles pending stages, whose handlers can close the view.
        let Some(raw) = self.raw() else { return };
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_navigation(raw, Some(on_navigation), data); }
    }
    /// Remove navigation policy and continue its pending stages.
    pub fn clear_navigation_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.navigation_generation.set(self.inner.navigation_generation.get().wrapping_add(1));
        self.inner.on_navigation.borrow_mut().take();
        unsafe { sys::zebkit_view_on_navigation(raw, None, std::ptr::null_mut()); }
    }
    /// Observe asynchronous navigation decisions as JSON.
    pub fn on_navigation_result<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.navigation_result_generation.set(self.inner.navigation_result_generation.get().wrapping_add(1));
        *self.inner.on_navigation_result.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_navigation_result(raw, Some(on_navigation_result), data); }
    }
    /// Remove the navigation outcome observer, including inside that observer.
    pub fn clear_navigation_result_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.navigation_result_generation.set(self.inner.navigation_result_generation.get().wrapping_add(1));
        self.inner.on_navigation_result.borrow_mut().take();
        unsafe { sys::zebkit_view_on_navigation_result(raw, None, std::ptr::null_mut()); }
    }

    /// Observe asynchronous new-window decisions and creation metadata as JSON.
    pub fn on_window_open_event<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.window_open_event_generation.set(self.inner.window_open_event_generation.get().wrapping_add(1));
        *self.inner.on_window_open_event.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_window_open_event(raw, Some(on_window_open_event), data); }
    }
    /// Remove the new-window metadata observer, including inside that observer.
    pub fn clear_window_open_event_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.window_open_event_generation.set(self.inner.window_open_event_generation.get().wrapping_add(1));
        self.inner.on_window_open_event.borrow_mut().take();
        unsafe { sys::zebkit_view_on_window_open_event(raw, None, std::ptr::null_mut()); }
    }

    /// Observe asynchronous native document-console messages as JSON.
    pub fn on_console<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        self.inner.console_generation.set(self.inner.console_generation.get().wrapping_add(1));
        *self.inner.on_console.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_console(raw, Some(on_console), data); }
    }
    /// Remove the console observer, including inside that observer.
    pub fn clear_console_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.console_generation.set(self.inner.console_generation.get().wrapping_add(1));
        self.inner.on_console.borrow_mut().take();
        unsafe { sys::zebkit_view_on_console(raw, None, std::ptr::null_mut()); }
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

    /// Reports a blocked popup as JSON after the decision. The callback can
    /// navigate or close this view. It does not reopen the blocked popup.
    pub fn on_popup_blocked<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.on_popup_blocked.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_popup_blocked(raw, Some(on_popup_blocked), data) }
    }

    /// A window a page opens comes here as a view of its own, with the URL
    /// it opened, still linked to the page that opened it (`window.opener`,
    /// `postMessage()`, `window.closed`), as a site's sign-in through a
    /// popup needs. Off-screen when this view is. The application shows it
    /// and closes it; [`View::on_new_window`], when set, takes the URL
    /// instead.
    pub fn on_window_open<F: FnMut(View, &str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.on_window_open.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_window_open(raw, Some(on_window_open), data) }
    }

    /// The paths of files dropped on the view, as a JSON array, before the
    /// page gets the drop.
    pub fn on_drop<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.on_drop.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_drop(raw, Some(on_drop), data) }
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
/// later, with [`Call::resolve`] or [`Call::reject`]; one that is dropped
/// unanswered rejects the page's promise.
pub struct Call {
    raw: *mut sys::zebkit_call,
    origin: String,
    args: String,
}

impl Call {
    /// The origin of the page that called.
    pub fn origin(&self) -> &str {
        &self.origin
    }

    /// The arguments, as a JSON array.
    pub fn args_json(&self) -> &str {
        &self.args
    }

    /// Resolves the page's promise with `json`, a JSON value.
    pub fn resolve(mut self, json: &str) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        unsafe { sys::zebkit_call_resolve(raw, c_string(json).as_ptr()) }
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
            unsafe { sys::zebkit_call_reject(self.raw, c_string("the application did not answer").as_ptr()) }
        }
    }
}

/// Calls the closure in `slot` without holding it borrowed, so that the
/// closure may replace it; put back unless it was.
pub(crate) fn call_slot<T: ?Sized>(slot: &Slot<T>, call: impl FnOnce(&mut T)) {
    let Some(mut function) = slot.borrow_mut().take() else { return };
    call(&mut function);
    let mut current = slot.borrow_mut();
    if current.is_none() {
        *current = Some(function);
    }
}

unsafe extern "C" fn on_load(
    _view: *mut sys::zebkit_view,
    event: sys::zebkit_load_event,
    url: *const c_char,
    data: *mut c_void,
) {
    let inner = &*data.cast::<Inner>();
    let event = match event {
        sys::ZEBKIT_LOAD_STARTED => LoadEvent::Started,
        sys::ZEBKIT_LOAD_COMMITTED => LoadEvent::Committed,
        sys::ZEBKIT_LOAD_FINISHED => LoadEvent::Finished,
        _ => LoadEvent::Failed,
    };
    let url = string_from(url).unwrap_or_default();
    call_slot(&inner.on_load, |function| function(event, &url));
}

unsafe extern "C" fn on_title(_view: *mut sys::zebkit_view, title: *const c_char, data: *mut c_void) {
    let inner = &*data.cast::<Inner>();
    let title = string_from(title).unwrap_or_default();
    call_slot(&inner.on_title, |function| function(&title));
}

unsafe extern "C" fn on_close(_view: *mut sys::zebkit_view, data: *mut c_void) {
    // The engine's reference, which this is the last use of.
    let inner = Rc::from_raw(data.cast_const().cast::<Inner>());
    inner.raw.set(std::ptr::null_mut());
    let function = inner.on_close.borrow_mut().take();
    if let Some(function) = function {
        function();
    }
    // What the closures hold may hold the view: let it go.
    inner.on_load.borrow_mut().take();
    inner.on_title.borrow_mut().take();
    inner.on_new_window.borrow_mut().take();
    inner.on_window_open.borrow_mut().take();
    inner.on_popup_blocked.borrow_mut().take();
    inner.on_permission.borrow_mut().take();
    inner.on_permission_result.borrow_mut().take();
    inner.on_navigation.borrow_mut().take();
    inner.on_navigation_result.borrow_mut().take();
    inner.on_window_open_event.borrow_mut().take();
    inner.on_console.borrow_mut().take();
    inner.on_certificate.borrow_mut().take();
    inner.on_certificate_result.borrow_mut().take();
    inner.hooks.clear();
    inner.on_http_auth.borrow_mut().take();
    inner.on_http_auth_result.borrow_mut().take();
    inner.on_drop.borrow_mut().take();
    inner.bindings.borrow_mut().clear();
    inner.retired.borrow_mut().clear();
    inner.offscreen.clear();
    inner.page.clear();
}

unsafe extern "C" fn on_capture<F: FnOnce(Result<Vec<u8>, Error>)>(
    _view: *mut sys::zebkit_view,
    png: *const u8,
    size: usize,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    if png.is_null() {
        done(Err(Error::new(string_from(error).unwrap_or_else(|| "no picture".to_string()))));
    } else {
        done(Ok(std::slice::from_raw_parts(png, size).to_vec()));
    }
}

unsafe extern "C" fn on_new_window(_view: *mut sys::zebkit_view, url: *const c_char, data: *mut c_void) {
    let inner = &*data.cast::<Inner>();
    let url = string_from(url).unwrap_or_default();
    call_slot(&inner.on_new_window, |function| function(&url));
}

unsafe extern "C" fn on_certificate(_view: *mut sys::zebkit_view, raw: *mut sys::zebkit_certificate_request, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.certificate_generation.get();
    let Some(mut function) = inner.on_certificate.borrow_mut().take() else { return };
    function(CertificateRequest { raw, metadata: string_from(json).unwrap_or_default(), _thread: std::marker::PhantomData });
    if inner.certificate_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_certificate.borrow_mut() = Some(function); }
}
unsafe extern "C" fn on_certificate_result(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.certificate_result_generation.get();
    let Some(mut function) = inner.on_certificate_result.borrow_mut().take() else { return };
    function(&string_from(json).unwrap_or_default());
    if inner.certificate_result_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_certificate_result.borrow_mut() = Some(function); }
}

unsafe extern "C" fn on_http_auth(_view: *mut sys::zebkit_view, raw: *mut sys::zebkit_http_auth_request, json: *const c_char, data: *mut c_void) {
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.http_auth_generation.get();
    let Some(mut function) = inner.on_http_auth.borrow_mut().take() else { return };
    function(HttpAuthRequest { raw, metadata: string_from(json).unwrap_or_default(), _thread: std::marker::PhantomData });
    if inner.http_auth_generation.get() == generation && !inner.raw.get().is_null() {
        *inner.on_http_auth.borrow_mut() = Some(function);
    }
}
unsafe extern "C" fn on_http_auth_result(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.http_auth_result_generation.get();
    let Some(mut function) = inner.on_http_auth_result.borrow_mut().take() else { return };
    function(&string_from(json).unwrap_or_default());
    if inner.http_auth_result_generation.get() == generation && !inner.raw.get().is_null() {
        *inner.on_http_auth_result.borrow_mut() = Some(function);
    }
}

unsafe extern "C" fn on_navigation(_view: *mut sys::zebkit_view, raw: *mut sys::zebkit_navigation_request, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.navigation_generation.get();
    let Some(mut function) = inner.on_navigation.borrow_mut().take() else { return };
    function(NavigationRequest { raw, metadata: string_from(json).unwrap_or_default(), _thread: std::marker::PhantomData });
    if inner.navigation_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_navigation.borrow_mut() = Some(function); }
}
unsafe extern "C" fn on_navigation_result(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.navigation_result_generation.get();
    let Some(mut function) = inner.on_navigation_result.borrow_mut().take() else { return };
    function(&string_from(json).unwrap_or_default());
    if inner.navigation_result_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_navigation_result.borrow_mut() = Some(function); }
}

unsafe extern "C" fn on_window_open_event(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.window_open_event_generation.get();
    let Some(mut function) = inner.on_window_open_event.borrow_mut().take() else { return };
    function(&string_from(json).unwrap_or_default());
    if inner.window_open_event_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_window_open_event.borrow_mut() = Some(function); }
}

unsafe extern "C" fn on_console(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.console_generation.get();
    let Some(mut function) = inner.on_console.borrow_mut().take() else { return };
    function(&string_from(json).unwrap_or_default());
    if inner.console_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_console.borrow_mut() = Some(function); }
}

unsafe extern "C" fn on_permission(_view: *mut sys::zebkit_view, raw: *mut sys::zebkit_permission_request, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.permission_generation.get();
    let Some(mut function) = inner.on_permission.borrow_mut().take() else { return };
    function(PermissionRequest { raw, metadata: string_from(json).unwrap_or_default(), _thread: std::marker::PhantomData });
    if inner.permission_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_permission.borrow_mut() = Some(function); }
}
unsafe extern "C" fn on_permission_result(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    // The handler may close the view, releasing the last other reference.
    let pointer = data.cast_const().cast::<Inner>();
    Rc::increment_strong_count(pointer);
    let inner = Rc::from_raw(pointer);
    let generation = inner.permission_result_generation.get();
    let Some(mut function) = inner.on_permission_result.borrow_mut().take() else { return };
    function(&string_from(json).unwrap_or_default());
    if inner.permission_result_generation.get() == generation && !inner.raw.get().is_null() { *inner.on_permission_result.borrow_mut() = Some(function); }
}

unsafe extern "C" fn on_popup_blocked(_view: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
    let inner = &*data.cast::<Inner>();
    let json = string_from(json).unwrap_or_default();
    call_slot(&inner.on_popup_blocked, |function| function(&json));
}

unsafe extern "C" fn on_window_open(
    _opener: *mut sys::zebkit_view,
    opened: *mut sys::zebkit_view,
    url: *const c_char,
    data: *mut c_void,
) {
    let inner = &*data.cast::<Inner>();
    let url = string_from(url).unwrap_or_default();
    let opened = View::wrap(opened);
    call_slot(&inner.on_window_open, |function| function(opened.clone(), &url));
}

unsafe extern "C" fn on_drop(_view: *mut sys::zebkit_view, paths: *const c_char, data: *mut c_void) {
    let inner = &*data.cast::<Inner>();
    let paths = string_from(paths).unwrap_or_else(|| "[]".to_string());
    call_slot(&inner.on_drop, |function| function(&paths));
}

unsafe extern "C" fn on_result<F: FnOnce(Result<String, Error>)>(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    done(script_result(json, error));
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
        Ok(mut function) => function(call),
        // The function called the page, which called it again.
        Err(_) => call.reject("the application's function is already running"),
    }
}
