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

pub(crate) struct Inner {
    /// Null once the view is closed.
    pub(crate) raw: Cell<*mut sys::zebkit_view>,
    on_load: Slot<LoadFunction>,
    on_title: Slot<dyn FnMut(&str)>,
    on_close: Slot<dyn FnOnce()>,
    on_new_window: Slot<dyn FnMut(&str)>,
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

    /// A PNG of what the view shows.
    pub fn capture<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_capture(raw, on_capture::<F>, data.cast()) }
    }

    /// What a page opens in a new window comes here as its URL, and no
    /// window is made.
    pub fn on_new_window<F: FnMut(&str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.on_new_window.borrow_mut() = Some(Box::new(function));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_new_window(raw, Some(on_new_window), data) }
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
