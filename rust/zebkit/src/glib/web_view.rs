use crate::sys::glib::{self, GCallback, GClosure, GParamSpec};
use crate::sys::webkit::{self, WebKitWebView};
use crate::sys::wpe::{self, WPEToplevel, WPEView};
use crate::sys::{c_int, gpointer};
use crate::{c_string, string_from, LoadEvent};
use std::marker::PhantomData;
use std::mem::transmute;

/// A web page.
///
/// It is shown in a window of its own when the machine has a display
/// (Wayland, else X11) and exists without one otherwise. Dropping it closes
/// the page.
pub struct WebView {
    raw: *mut WebKitWebView,
    _one_thread: PhantomData<*mut ()>,
}

impl WebView {
    /// A page on whatever display the machine has.
    pub fn new() -> Self {
        let raw = unsafe { glib::g_object_new(webkit::webkit_web_view_get_type(), std::ptr::null()) };
        Self {
            raw: raw.cast(),
            _one_thread: PhantomData,
        }
    }

    /// A page that is never shown, whatever the machine has. It loads,
    /// runs scripts and can be driven over [`crate::cdp`] like any other.
    pub fn headless() -> Self {
        unsafe {
            let display = wpe::wpe_display_headless_new();
            let raw = glib::g_object_new(
                webkit::webkit_web_view_get_type(),
                c"display".as_ptr(),
                display,
                std::ptr::null::<std::os::raw::c_char>(),
            );
            glib::g_object_unref(display.cast());
            Self {
                raw: raw.cast(),
                _one_thread: PhantomData,
            }
        }
    }

    /// Starts loading an address. Returns at once; see
    /// [`WebView::on_load_changed`].
    pub fn load_uri(&self, uri: &str) {
        unsafe { webkit::webkit_web_view_load_uri(self.raw, c_string(uri).as_ptr()) }
    }

    /// Shows HTML given as text. Relative addresses in it are resolved
    /// against `base_uri`.
    pub fn load_html(&self, html: &str, base_uri: Option<&str>) {
        let base = base_uri.map(c_string);
        unsafe {
            webkit::webkit_web_view_load_html(
                self.raw,
                c_string(html).as_ptr(),
                base.as_ref().map_or(std::ptr::null(), |text| text.as_ptr()),
            )
        }
    }

    /// The address of the page, or of the one being loaded.
    pub fn uri(&self) -> Option<String> {
        unsafe { string_from(webkit::webkit_web_view_get_uri(self.raw)) }
    }

    /// The page's title, once it has one.
    pub fn title(&self) -> Option<String> {
        unsafe { string_from(webkit::webkit_web_view_get_title(self.raw)) }.filter(|title| !title.is_empty())
    }

    pub fn is_loading(&self) -> bool {
        unsafe { webkit::webkit_web_view_is_loading(self.raw) != 0 }
    }

    pub fn reload(&self) {
        unsafe { webkit::webkit_web_view_reload(self.raw) }
    }

    pub fn stop_loading(&self) {
        unsafe { webkit::webkit_web_view_stop_loading(self.raw) }
    }

    pub fn can_go_back(&self) -> bool {
        unsafe { webkit::webkit_web_view_can_go_back(self.raw) != 0 }
    }

    /// Goes to the previous page in the history, if there is one.
    pub fn go_back(&self) {
        unsafe { webkit::webkit_web_view_go_back(self.raw) }
    }

    pub fn can_go_forward(&self) -> bool {
        unsafe { webkit::webkit_web_view_can_go_forward(self.raw) != 0 }
    }

    /// Goes to the next page in the history, if there is one.
    pub fn go_forward(&self) {
        unsafe { webkit::webkit_web_view_go_forward(self.raw) }
    }

    /// The window the page is in. A page that is not shown, on a machine
    /// with no display or made with [`WebView::headless`], has one too: it
    /// has a size and a title and is never on any screen.
    pub fn window(&self) -> Option<Window> {
        unsafe {
            let surface = webkit::webkit_web_view_get_wpe_view(self.raw);
            if surface.is_null() {
                return None;
            }
            let toplevel = wpe::wpe_view_get_toplevel(surface);
            (!toplevel.is_null()).then(|| Window {
                raw: glib::g_object_ref(toplevel.cast()).cast(),
                _one_thread: PhantomData,
            })
        }
    }

    /// Calls `function` with the new title whenever the page's title
    /// changes.
    pub fn on_title_changed(&self, function: impl Fn(&str) + 'static) {
        unsafe extern "C" fn call(view: *mut WebKitWebView, _: *mut GParamSpec, data: gpointer) {
            let function = &*(data as *const Box<dyn Fn(&str)>);
            let title = string_from(webkit::webkit_web_view_get_title(view)).unwrap_or_default();
            function(&title);
        }
        let handler: unsafe extern "C" fn(*mut WebKitWebView, *mut GParamSpec, gpointer) = call;
        self.connect(
            c"notify::title",
            unsafe { transmute::<unsafe extern "C" fn(*mut WebKitWebView, *mut GParamSpec, gpointer), GCallback>(handler) },
            Box::new(function) as Box<dyn Fn(&str)>,
        );
    }

    /// Calls `function` with the new address whenever the page's address
    /// changes, also when the page changes it itself.
    pub fn on_uri_changed(&self, function: impl Fn(&str) + 'static) {
        unsafe extern "C" fn call(view: *mut WebKitWebView, _: *mut GParamSpec, data: gpointer) {
            let function = &*(data as *const Box<dyn Fn(&str)>);
            let uri = string_from(webkit::webkit_web_view_get_uri(view)).unwrap_or_default();
            function(&uri);
        }
        let handler: unsafe extern "C" fn(*mut WebKitWebView, *mut GParamSpec, gpointer) = call;
        self.connect(
            c"notify::uri",
            unsafe { transmute::<unsafe extern "C" fn(*mut WebKitWebView, *mut GParamSpec, gpointer), GCallback>(handler) },
            Box::new(function) as Box<dyn Fn(&str)>,
        );
    }

    /// Calls `function` at each step of every page load.
    pub fn on_load_changed(&self, function: impl Fn(LoadEvent) + 'static) {
        unsafe extern "C" fn call(_: *mut WebKitWebView, event: c_int, data: gpointer) {
            let function = &*(data as *const Box<dyn Fn(LoadEvent)>);
            function(match event {
                webkit::WEBKIT_LOAD_STARTED => LoadEvent::Started,
                webkit::WEBKIT_LOAD_REDIRECTED => LoadEvent::Redirected,
                webkit::WEBKIT_LOAD_COMMITTED => LoadEvent::Committed,
                _ => LoadEvent::Finished,
            });
        }
        let handler: unsafe extern "C" fn(*mut WebKitWebView, c_int, gpointer) = call;
        self.connect(
            c"load-changed",
            unsafe { transmute::<unsafe extern "C" fn(*mut WebKitWebView, c_int, gpointer), GCallback>(handler) },
            Box::new(function) as Box<dyn Fn(LoadEvent)>,
        );
    }

    /// Calls `function` when the user closes the page's window. The page
    /// goes on existing until the `WebView` is dropped.
    pub fn on_close(&self, function: impl Fn() + 'static) {
        unsafe extern "C" fn call(_: *mut WPEView, data: gpointer) {
            let function = &*(data as *const Box<dyn Fn()>);
            function();
        }
        unsafe extern "C" fn release(data: gpointer, _: *mut GClosure) {
            drop(Box::from_raw(data as *mut Box<dyn Fn()>));
        }
        unsafe {
            let surface = webkit::webkit_web_view_get_wpe_view(self.raw);
            if surface.is_null() {
                return;
            }
            let handler: unsafe extern "C" fn(*mut WPEView, gpointer) = call;
            let data = Box::into_raw(Box::new(Box::new(function) as Box<dyn Fn()>));
            glib::g_signal_connect_data(
                surface.cast(),
                c"closed".as_ptr(),
                transmute::<unsafe extern "C" fn(*mut WPEView, gpointer), GCallback>(handler),
                data.cast(),
                Some(release),
                0,
            );
        }
    }

    /// The engine's own object, for calls this crate does not wrap. It is
    /// valid for as long as this `WebView` is.
    pub fn as_ptr(&self) -> *mut WebKitWebView {
        self.raw
    }

    /// Connects a signal of the view to a boxed closure, which the engine
    /// frees when the view goes.
    fn connect<F: ?Sized>(&self, signal: &std::ffi::CStr, handler: GCallback, function: Box<F>) {
        unsafe extern "C" fn release<F: ?Sized>(data: gpointer, _: *mut GClosure) {
            drop(Box::from_raw(data as *mut Box<F>));
        }
        let data = Box::into_raw(Box::new(function));
        unsafe {
            glib::g_signal_connect_data(self.raw.cast(), signal.as_ptr(), handler, data.cast(), Some(release::<F>), 0);
        }
    }
}

impl Default for WebView {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for WebView {
    fn drop(&mut self) {
        unsafe { glib::g_object_unref(self.raw.cast()) }
    }
}

/// The window a [`WebView`] is shown in. Clones are handles to the same
/// window.
pub struct Window {
    raw: *mut WPEToplevel,
    _one_thread: PhantomData<*mut ()>,
}

impl Window {
    /// Asks for a size, in pixels. The window system may give another.
    pub fn resize(&self, width: u32, height: u32) {
        unsafe {
            wpe::wpe_toplevel_resize(self.raw, width.min(i32::MAX as u32) as c_int, height.min(i32::MAX as u32) as c_int);
        }
    }

    pub fn set_title(&self, title: &str) {
        unsafe { wpe::wpe_toplevel_set_title(self.raw, c_string(title).as_ptr()) }
    }
}

impl Clone for Window {
    fn clone(&self) -> Self {
        Self {
            raw: unsafe { glib::g_object_ref(self.raw.cast()).cast() },
            _one_thread: PhantomData,
        }
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        unsafe { glib::g_object_unref(self.raw.cast()) }
    }
}
