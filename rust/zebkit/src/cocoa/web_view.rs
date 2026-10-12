use super::free_text;
use crate::sys::cocoa::{self, ZebkitShimView};
use crate::{c_string, string_from, LoadEvent};
use std::marker::PhantomData;
use std::os::raw::{c_char, c_int, c_void};

/// A web page.
///
/// It is shown in a window of its own when the machine has a display, and
/// exists without one otherwise. Dropping it closes the page.
pub struct WebView {
    raw: *mut ZebkitShimView,
    _one_thread: PhantomData<*mut ()>,
}

impl WebView {
    /// A page on whatever display the machine has.
    pub fn new() -> Self {
        Self {
            raw: unsafe { cocoa::zebkit_shim_view_new(0) },
            _one_thread: PhantomData,
        }
    }

    /// A page that is never shown, whatever the machine has. It loads,
    /// runs scripts and can be driven over [`crate::cdp`] like any other.
    pub fn headless() -> Self {
        Self {
            raw: unsafe { cocoa::zebkit_shim_view_new(1) },
            _one_thread: PhantomData,
        }
    }

    /// Starts loading an address. Returns at once; see
    /// [`WebView::on_load_changed`].
    pub fn load_uri(&self, uri: &str) {
        unsafe { cocoa::zebkit_shim_view_load_uri(self.raw, c_string(uri).as_ptr()) }
    }

    /// Shows HTML given as text. Relative addresses in it are resolved
    /// against `base_uri`.
    pub fn load_html(&self, html: &str, base_uri: Option<&str>) {
        let base = base_uri.map(c_string);
        unsafe {
            cocoa::zebkit_shim_view_load_html(
                self.raw,
                c_string(html).as_ptr(),
                base.as_ref().map_or(std::ptr::null(), |text| text.as_ptr()),
            )
        }
    }

    /// The address of the page, or of the one being loaded.
    pub fn uri(&self) -> Option<String> {
        unsafe { take_text(cocoa::zebkit_shim_view_copy_uri(self.raw)) }
    }

    /// The page's title, once it has one.
    pub fn title(&self) -> Option<String> {
        unsafe { take_text(cocoa::zebkit_shim_view_copy_title(self.raw)) }.filter(|title| !title.is_empty())
    }

    pub fn is_loading(&self) -> bool {
        unsafe { cocoa::zebkit_shim_view_is_loading(self.raw) != 0 }
    }

    pub fn reload(&self) {
        unsafe { cocoa::zebkit_shim_view_reload(self.raw) }
    }

    pub fn stop_loading(&self) {
        unsafe { cocoa::zebkit_shim_view_stop_loading(self.raw) }
    }

    pub fn can_go_back(&self) -> bool {
        unsafe { cocoa::zebkit_shim_view_can_go_back(self.raw) != 0 }
    }

    /// Goes to the previous page in the history, if there is one.
    pub fn go_back(&self) {
        unsafe { cocoa::zebkit_shim_view_go_back(self.raw) }
    }

    pub fn can_go_forward(&self) -> bool {
        unsafe { cocoa::zebkit_shim_view_can_go_forward(self.raw) != 0 }
    }

    /// Goes to the next page in the history, if there is one.
    pub fn go_forward(&self) {
        unsafe { cocoa::zebkit_shim_view_go_forward(self.raw) }
    }

    /// The window the page is in. A page that is not shown, on a machine
    /// with no display or made with [`WebView::headless`], has one too: it
    /// has a size and a title and is never on any screen.
    pub fn window(&self) -> Option<Window> {
        Some(Window {
            raw: unsafe { cocoa::zebkit_shim_view_retain(self.raw) },
            _one_thread: PhantomData,
        })
    }

    /// Calls `function` with the new title whenever the page's title
    /// changes.
    pub fn on_title_changed(&self, function: impl Fn(&str) + 'static) {
        self.on_text(cocoa::ZEBKIT_SHIM_TITLE, function);
    }

    /// Calls `function` with the new address whenever the page's address
    /// changes, also when the page changes it itself.
    pub fn on_uri_changed(&self, function: impl Fn(&str) + 'static) {
        self.on_text(cocoa::ZEBKIT_SHIM_URI, function);
    }

    /// Calls `function` at each step of every page load.
    pub fn on_load_changed(&self, function: impl Fn(LoadEvent) + 'static) {
        self.connect(cocoa::ZEBKIT_SHIM_LOAD, move |event, _| {
            function(match event {
                cocoa::ZEBKIT_SHIM_LOAD_STARTED => LoadEvent::Started,
                cocoa::ZEBKIT_SHIM_LOAD_REDIRECTED => LoadEvent::Redirected,
                cocoa::ZEBKIT_SHIM_LOAD_COMMITTED => LoadEvent::Committed,
                _ => LoadEvent::Finished,
            })
        });
    }

    /// Calls `function` when the user closes the page's window. The page
    /// goes on existing until the `WebView` is dropped.
    pub fn on_close(&self, function: impl Fn() + 'static) {
        self.connect(cocoa::ZEBKIT_SHIM_CLOSE, move |_, _| function());
    }

    /// The engine's own object, a `WKWebView`, for calls this crate does
    /// not wrap. It is valid for as long as this `WebView` is.
    pub fn as_ptr(&self) -> *mut c_void {
        unsafe { cocoa::zebkit_shim_view_get_web_view(self.raw) }
    }

    fn on_text(&self, kind: c_int, function: impl Fn(&str) + 'static) {
        self.connect(kind, move |_, text| function(&unsafe { string_from(text) }.unwrap_or_default()));
    }

    /// Hands a boxed closure to the view, which frees it when it goes.
    fn connect(&self, kind: c_int, function: impl Fn(c_int, *const c_char) + 'static) {
        type Handler = Box<dyn Fn(c_int, *const c_char)>;
        unsafe extern "C" fn call(event: c_int, text: *const c_char, data: *mut c_void) {
            let function = &*(data as *const Handler);
            function(event, text);
        }
        unsafe extern "C" fn release(data: *mut c_void) {
            drop(Box::from_raw(data as *mut Handler));
        }
        let data = Box::into_raw(Box::new(Box::new(function) as Handler));
        unsafe { cocoa::zebkit_shim_view_add_handler(self.raw, kind, call, data.cast(), Some(release)) }
    }
}

impl Default for WebView {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for WebView {
    fn drop(&mut self) {
        unsafe { cocoa::zebkit_shim_view_release(self.raw) }
    }
}

/// The window a [`WebView`] is shown in. Clones are handles to the same
/// window.
pub struct Window {
    raw: *mut ZebkitShimView,
    _one_thread: PhantomData<*mut ()>,
}

impl Window {
    /// Asks for a size, in points, for the page: the window's frame is
    /// around it.
    pub fn resize(&self, width: u32, height: u32) {
        unsafe {
            cocoa::zebkit_shim_window_resize(self.raw, width.min(i32::MAX as u32) as c_int, height.min(i32::MAX as u32) as c_int);
        }
    }

    pub fn set_title(&self, title: &str) {
        unsafe { cocoa::zebkit_shim_window_set_title(self.raw, c_string(title).as_ptr()) }
    }
}

impl Clone for Window {
    fn clone(&self) -> Self {
        Self {
            raw: unsafe { cocoa::zebkit_shim_view_retain(self.raw) },
            _one_thread: PhantomData,
        }
    }
}

impl Drop for Window {
    fn drop(&mut self) {
        unsafe { cocoa::zebkit_shim_view_release(self.raw) }
    }
}

/// A copy of text the engine allocated for the caller, which is freed.
unsafe fn take_text(text: *mut c_char) -> Option<String> {
    let copy = string_from(text);
    if !text.is_null() {
        free_text(text);
    }
    copy
}
