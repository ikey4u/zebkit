//! URL schemes of the application's own: [`register`] before
//! [`crate::run`], then [`handle`] answers their requests, page loads,
//! subresources, `fetch()` and media alike.

use crate::engine::require_engine_thread;
use crate::{c_string, string_from, sys};
use std::cell::RefCell;
use std::os::raw::c_void;
use std::path::Path;

/// Like https:, with a host and a path: an origin, relative URLs, storage.
pub const STANDARD: u32 = sys::ZEBKIT_SCHEME_STANDARD;
/// A secure context, as https: is.
pub const SECURE: u32 = sys::ZEBKIT_SCHEME_SECURE;
/// `fetch()` and XMLHttpRequest from pages, with CORS.
pub const FETCH: u32 = sys::ZEBKIT_SCHEME_FETCH;
/// Pages' Content-Security-Policy does not restrict loads from it.
pub const BYPASS_CSP: u32 = sys::ZEBKIT_SCHEME_BYPASS_CSP;

/// Makes `scheme` one of the application's, as `flags` say. Before
/// [`crate::run`]. False for a scheme the engine has already.
pub fn register(scheme: &str, flags: u32) -> bool {
    unsafe { sys::zebkit_scheme_register(c_string(scheme).as_ptr(), flags) }
}

/// A request for a URL of the application's scheme. Answer it once, with
/// [`Request::respond`] or [`Request::respond_file`], from any thread; one
/// dropped unanswered gets 404.
pub struct Request {
    raw: *mut sys::zebkit_request,
}

// The C API takes an answer from any thread.
unsafe impl Send for Request {}

impl Request {
    pub fn url(&self) -> String {
        unsafe { string_from(sys::zebkit_request_url(self.raw)) }.unwrap_or_default()
    }

    pub fn method(&self) -> String {
        unsafe { string_from(sys::zebkit_request_method(self.raw)) }.unwrap_or_default()
    }

    /// "Name: value" lines separated by "\r\n".
    pub fn headers(&self) -> String {
        unsafe { string_from(sys::zebkit_request_headers(self.raw)) }.unwrap_or_default()
    }

    /// The body sent with it.
    pub fn body(&self) -> Option<Vec<u8>> {
        let mut size = 0;
        let body = unsafe { sys::zebkit_request_body(self.raw, &mut size) };
        (!body.is_null()).then(|| unsafe { std::slice::from_raw_parts(body, size) }.to_vec())
    }

    /// Answers with `status`, `headers` ("Name: value" lines separated by
    /// "\r\n") and `body`.
    pub fn respond(mut self, status: u16, headers: &str, body: &[u8]) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        unsafe {
            sys::zebkit_request_respond(raw, i32::from(status), c_string(headers).as_ptr(), body.as_ptr().cast(),
                                        body.len())
        }
    }

    /// Answers with the file at `path`, in the range a Range header asks for.
    pub fn respond_file(mut self, path: &Path, headers: &str) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        unsafe {
            sys::zebkit_request_respond_file(raw, c_string(&path.to_string_lossy()).as_ptr(),
                                             c_string(headers).as_ptr())
        }
    }
}

impl Drop for Request {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::zebkit_request_respond(self.raw, 404, std::ptr::null(), std::ptr::null(), 0) }
        }
    }
}

type HandlerFunction = Box<dyn FnMut(Request)>;

thread_local! {
    /// The handlers the engine holds, kept for as long as the process.
    /// Boxed: the engine holds their addresses.
    #[allow(clippy::vec_box)]
    static HANDLERS: RefCell<Vec<Box<RefCell<HandlerFunction>>>> = const { RefCell::new(Vec::new()) };
}

/// Answers `scheme`'s requests with `function`, on the engine's thread.
pub fn handle<F: FnMut(Request) + 'static>(scheme: &str, function: F) {
    require_engine_thread("scheme::handle()");
    let slot: Box<RefCell<HandlerFunction>> = Box::new(RefCell::new(Box::new(function)));
    let data = (&*slot as *const RefCell<HandlerFunction>).cast_mut().cast::<c_void>();
    unsafe { sys::zebkit_scheme_handle(c_string(scheme).as_ptr(), Some(on_request), data) }
    HANDLERS.with(|handlers| handlers.borrow_mut().push(slot));
}

unsafe extern "C" fn on_request(request: *mut sys::zebkit_request, data: *mut c_void) {
    let slot = &*data.cast::<RefCell<HandlerFunction>>();
    let request = Request { raw: request };
    match slot.try_borrow_mut() {
        Ok(mut function) => function(request),
        Err(_) => request.respond(500, "", b"the handler is already running"),
    }
}
