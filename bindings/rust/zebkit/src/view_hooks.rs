//! The hooks of `<zebkit/hooks.h>` that a page's requests reach: links to other
//! programs, downloads, client certificates, file choosers, media, keys, the
//! context menu and fullscreen. Requests are engine-thread, single-use objects.

use std::cell::Cell;
use std::ffi::{c_char, c_void, CString};
use std::rc::Rc;

use zebkit_rs as sys;

use crate::view::{hold, View};
use crate::{string_from, take_string, Error, NavigationAction, PermissionState, Slot};

/// Text for the engine, refused when it has a NUL, which would cut it short.
fn cstring(value: &str) -> Result<CString, Error> {
    CString::new(value).map_err(|_| Error::new("a string contains NUL"))
}
fn optional(value: &Option<CString>) -> *const c_char {
    value.as_ref().map_or(std::ptr::null(), |value| value.as_ptr())
}

/// A call that sets `char **error` when it fails: `Err` with what the
/// engine said, or `fallback`.
fn answered(fallback: &str, call: impl FnOnce(*mut *mut c_char) -> bool) -> Result<(), Error> {
    let mut error = std::ptr::null_mut();
    if call(&mut error) {
        Ok(())
    } else {
        Err(unsafe { crate::take_error(error, fallback) })
    }
}

type Hook<T> = Slot<T>;
type Asked<T> = Hook<dyn FnMut(T)>;
type Told = Hook<dyn FnMut(&str)>;
type NetworkBody = Hook<dyn FnMut(&str, &[u8], bool, bool)>;

macro_rules! slots {
    ($($asked:ident: $request:ty),*; $($told:ident),*) => {
        #[derive(Default)]
        pub(crate) struct Slots {
            $($asked: Asked<$request>,)*
            $($told: Told,)*
            network_body: NetworkBody,
        }
        impl Slots {
            pub(crate) fn clear(&self) {
                $(self.$asked.set(None);)*
                $(self.$told.set(None);)*
                self.network_body.set(None);
            }
        }
    };
}

// A request is the engine's thread's alone: not Send (the PhantomData), so
// that its methods need no check of the thread.
macro_rules! request {
    ($(#[$doc:meta])* $name:ident, $raw:ty, $info:path) => {
        $(#[$doc])*
        #[derive(Clone)]
        pub struct $name {
            raw: *mut $raw,
            metadata: String,
            _thread: std::marker::PhantomData<Rc<()>>,
        }
        impl $name {
            /// Metadata JSON, retained after settlement.
            pub fn metadata(&self) -> &str { &self.metadata }
            /// Current JSON, or None after a reply, the deadline or cancellation.
            pub fn info(&self) -> Option<String> { unsafe { take_string($info(self.raw)) } }
        }
    };
}

request!(/// A permission prompt. Clones refer to the same single-use prompt;
    /// holding one does not extend its deadline or the document's life.
    PermissionRequest, sys::zebkit_permission_request, sys::zebkit_permission_request_info);
request!(/// A stage of a navigation. Clones share a single-use, expiring token.
    NavigationRequest, sys::zebkit_navigation_request, sys::zebkit_navigation_request_info);
request!(/// A document's certificate error, with a single-use, expiring token.
    CertificateRequest, sys::zebkit_certificate_request, sys::zebkit_certificate_request_info);
request!(/// An expiring HTTP or proxy challenge. Credentials are not retained.
    HttpAuthRequest, sys::zebkit_http_auth_request, sys::zebkit_http_auth_request_info);

impl PermissionRequest {
    /// Reply once, optionally storing a view rule for the requesting origin.
    /// False means the request was already settled or the reply was invalid.
    pub fn respond(&self, state: PermissionState, remember: bool) -> bool {
        unsafe { sys::zebkit_permission_respond(self.raw, state as i32, remember) }
    }
}

impl NavigationRequest {
    /// Reply once. Invalid URLs and stale or expired requests return an error.
    pub fn respond(&self, action: NavigationAction<'_>) -> Result<(), Error> {
        let (kind, replacement) = match action {
            NavigationAction::Continue => (sys::ZEBKIT_NAVIGATION_CONTINUE, None),
            NavigationAction::Cancel => (sys::ZEBKIT_NAVIGATION_CANCEL, None),
            NavigationAction::Redirect(url) => {
                (sys::ZEBKIT_NAVIGATION_REDIRECT, Some(CString::new(url).map_err(|_| Error::new("replacement URL contains NUL"))?))
            }
        };
        answered("navigation reply rejected", |error| unsafe { sys::zebkit_navigation_respond(self.raw, kind, optional(&replacement), error) })
    }
}

impl CertificateRequest {
    /// Reply once. False means stale, expired or a forbidden strict-error bypass.
    pub fn respond(&self, allow: bool) -> bool {
        unsafe { sys::zebkit_certificate_respond(self.raw, allow) }
    }
}

impl HttpAuthRequest {
    /// Provide credentials once; false means the request has already settled.
    pub fn provide(&self, username: &str, password: &str) -> Result<bool, Error> {
        if username.len() > 65536 || password.len() > 65536 {
            return Err(Error::new("each HTTP authentication credential is limited to 65536 UTF-8 bytes"));
        }
        let username = CString::new(username).map_err(|_| Error::new("HTTP authentication credentials cannot contain NUL"))?;
        let password = CString::new(password).map_err(|_| Error::new("HTTP authentication credentials cannot contain NUL"))?;
        Ok(unsafe { sys::zebkit_http_auth_respond(self.raw, username.as_ptr(), password.as_ptr()) })
    }
    /// Cancel once. Keeping this object does not extend the deadline.
    pub fn cancel(&self) -> bool {
        unsafe { sys::zebkit_http_auth_respond(self.raw, std::ptr::null(), std::ptr::null()) }
    }
}

request!(/// A download asked about.
    DownloadRequest, sys::zebkit_download_request, sys::zebkit_download_request_info);
request!(/// A client certificate a server asked for.
    ClientCertificateRequest, sys::zebkit_client_certificate_request, sys::zebkit_client_certificate_request_info);
request!(/// A file chooser a page opened.
    FileChooserRequest, sys::zebkit_file_chooser_request, sys::zebkit_file_chooser_request_info);
request!(/// A page's request for camera or microphone.
    MediaAccessRequest, sys::zebkit_media_access_request, sys::zebkit_media_access_request_info);

request!(/// A page's EyeDropper.open().
    EyeDropperRequest, sys::zebkit_eye_dropper_request, sys::zebkit_eye_dropper_request_info);
request!(/// A page's USB, HID or serial chooser: `kind`, `devices` (`info` has the list as it is now).
    DeviceChooserRequest, sys::zebkit_device_chooser_request, sys::zebkit_device_chooser_request_info);
request!(/// A page's navigator.share().
    ShareRequest, sys::zebkit_share_request, sys::zebkit_share_request_info);

request!(/// A request or response a network rule holds.
    NetworkRequest, sys::zebkit_network_request, sys::zebkit_network_request_info);

slots!(network: NetworkRequest, download: DownloadRequest, client_certificate: ClientCertificateRequest,
       file_chooser: FileChooserRequest, media_access: MediaAccessRequest, eye_dropper: EyeDropperRequest,
       device_chooser: DeviceChooserRequest, share: ShareRequest, permission: PermissionRequest,
       navigation: NavigationRequest, certificate: CertificateRequest, http_auth: HttpAuthRequest;
       eye_dropper_result, device_chooser_result, share_result, external_protocol, download_progress, client_certificate_result, file_chooser_result,
       media_access_result, network_event, reserved_key, context_menu, fullscreen, permission_result, navigation_result,
       certificate_result, http_auth_result, window_open_event, console, window_state, popup_blocked, download_result,
       process_gone, responsiveness);

// The methods that answer with a bool are older than their `try_` forms,
// which give what the engine said when it refused.

impl DownloadRequest {
    /// Save at an absolute path; false when stale or invalid ([`DownloadRequest::try_save`] says why).
    pub fn save(&self, path: &str) -> Result<bool, Error> {
        let path = cstring(path)?;
        Ok(self.respond(0, path.as_ptr()).is_ok())
    }
    pub fn cancel(&self) -> bool {
        self.respond(1, std::ptr::null()).is_ok()
    }
    /// [`DownloadRequest::save`], with the engine's reason when it refuses.
    pub fn try_save(&self, path: &str) -> Result<(), Error> {
        let path = cstring(path)?;
        self.respond(0, path.as_ptr())
    }
    /// [`DownloadRequest::cancel`], with the engine's reason when it refuses.
    pub fn try_cancel(&self) -> Result<(), Error> {
        self.respond(1, std::ptr::null())
    }
    fn respond(&self, action: std::os::raw::c_int, path: *const c_char) -> Result<(), Error> {
        answered("the download was not answered", |error| unsafe { sys::zebkit_download_respond(self.raw, action, path, error) })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClientCertificateAction {
    /// Continue without a certificate; Chromium remembers it for the host.
    None,
    /// Fail the request; nothing is remembered.
    Cancel,
}

impl ClientCertificateRequest {
    pub fn respond(&self, action: ClientCertificateAction) -> bool {
        self.try_respond(action).is_ok()
    }
    /// A PEM chain, leaf first, with its PEM private key.
    pub fn provide(&self, certificate_pem: &str, key_pem: &str) -> Result<bool, Error> {
        let (certificate, key) = (cstring(certificate_pem)?, cstring(key_pem)?);
        Ok(self.answer(1, certificate.as_ptr(), key.as_ptr()).is_ok())
    }
    /// One of the metadata's `identities`, the system store's (the engine
    /// setting `clientCertificateStores`): its key stays in the store.
    pub fn choose(&self, index: usize) -> bool {
        self.try_choose(index).is_ok()
    }
    /// [`ClientCertificateRequest::respond`], with the engine's reason when it refuses.
    pub fn try_respond(&self, action: ClientCertificateAction) -> Result<(), Error> {
        let action = if action == ClientCertificateAction::None { 0 } else { 2 };
        self.answer(action, std::ptr::null(), std::ptr::null())
    }
    /// [`ClientCertificateRequest::provide`], with the engine's reason when it refuses.
    pub fn try_provide(&self, certificate_pem: &str, key_pem: &str) -> Result<(), Error> {
        let (certificate, key) = (cstring(certificate_pem)?, cstring(key_pem)?);
        self.answer(1, certificate.as_ptr(), key.as_ptr())
    }
    /// [`ClientCertificateRequest::choose`], with the engine's reason when it refuses.
    pub fn try_choose(&self, index: usize) -> Result<(), Error> {
        answered("the certificate was not chosen", |error| unsafe { sys::zebkit_client_certificate_choose(self.raw, index, error) })
    }
    fn answer(&self, action: std::os::raw::c_int, certificate: *const c_char, key: *const c_char) -> Result<(), Error> {
        answered("the certificate request was not answered", |error| unsafe {
            sys::zebkit_client_certificate_respond(self.raw, action, certificate, key, error)
        })
    }
}

impl FileChooserRequest {
    /// Absolute paths; none cancels.
    pub fn respond(&self, paths: &[&str]) -> Result<bool, Error> {
        let owned = paths.iter().map(|path| cstring(path)).collect::<Result<Vec<_>, _>>()?;
        Ok(self.answer(&owned).is_ok())
    }
    /// [`FileChooserRequest::respond`], with the engine's reason when it refuses.
    pub fn try_respond(&self, paths: &[&str]) -> Result<(), Error> {
        let owned = paths.iter().map(|path| cstring(path)).collect::<Result<Vec<_>, _>>()?;
        self.answer(&owned)
    }
    fn answer(&self, paths: &[CString]) -> Result<(), Error> {
        let pointers: Vec<_> = paths.iter().map(|path| path.as_ptr()).collect();
        answered("the file chooser was not answered", |error| unsafe {
            sys::zebkit_file_chooser_respond(self.raw, pointers.as_ptr(), pointers.len(), error)
        })
    }
}

impl MediaAccessRequest {
    pub fn respond(&self, allow: bool, audio_device: Option<&str>, video_device: Option<&str>) -> Result<bool, Error> {
        let audio = audio_device.map(cstring).transpose()?;
        let video = video_device.map(cstring).transpose()?;
        Ok(unsafe { sys::zebkit_media_access_respond(self.raw, allow, optional(&audio), optional(&video)) })
    }
}

impl EyeDropperRequest {
    /// `Some("#rrggbb")` gives the page that colour; `None` cancels.
    pub fn respond(&self, color: Option<&str>) -> Result<bool, Error> {
        let color = color.map(cstring).transpose()?;
        Ok(unsafe { sys::zebkit_eye_dropper_respond(self.raw, optional(&color)) })
    }
}

impl DeviceChooserRequest {
    /// `Some(id)` of a listed device gives the page that device; `None` gives none.
    pub fn respond(&self, device_id: Option<&str>) -> Result<bool, Error> {
        let id = device_id.map(cstring).transpose()?;
        Ok(unsafe { sys::zebkit_device_chooser_respond(self.raw, optional(&id)) })
    }
}

impl ShareRequest {
    /// Shared, or not (the page's promise rejects).
    pub fn respond(&self, shared: bool) -> bool {
        unsafe { sys::zebkit_share_respond(self.raw, shared) }
    }
}

impl NetworkRequest {
    /// A reply as `<zebkit/hooks.h>` says, as JSON; false when stale or invalid
    /// ([`NetworkRequest::try_respond`] says why).
    pub fn respond(&self, reply_json: &str, body: &[u8]) -> Result<bool, Error> {
        let reply = cstring(reply_json)?;
        Ok(self.answer(&reply, body).is_ok())
    }
    /// Fulfills from an absolute path; `reply_json` is `{status, headers}`.
    pub fn respond_file(&self, reply_json: &str, path: &str) -> Result<bool, Error> {
        let (reply, path) = (cstring(reply_json)?, cstring(path)?);
        Ok(self.answer_file(&reply, &path).is_ok())
    }
    /// Fulfills with what the stream is given; None when stale or invalid.
    pub fn respond_stream(&self, reply_json: &str) -> Result<Option<NetworkStream>, Error> {
        let reply = cstring(reply_json)?;
        Ok(self.answer_stream(&reply).ok())
    }
    /// [`NetworkRequest::respond`], with the engine's reason when it refuses.
    pub fn try_respond(&self, reply_json: &str, body: &[u8]) -> Result<(), Error> {
        self.answer(&cstring(reply_json)?, body)
    }
    /// [`NetworkRequest::respond_file`], with the engine's reason when it refuses.
    pub fn try_respond_file(&self, reply_json: &str, path: &str) -> Result<(), Error> {
        self.answer_file(&cstring(reply_json)?, &cstring(path)?)
    }
    /// [`NetworkRequest::respond_stream`], with the engine's reason when it refuses.
    pub fn try_respond_stream(&self, reply_json: &str) -> Result<NetworkStream, Error> {
        self.answer_stream(&cstring(reply_json)?)
    }
    fn answer(&self, reply: &CString, body: &[u8]) -> Result<(), Error> {
        answered("the request was not answered", |error| unsafe {
            sys::zebkit_network_respond(self.raw, reply.as_ptr(), body.as_ptr().cast(), body.len(), error)
        })
    }
    fn answer_file(&self, reply: &CString, path: &CString) -> Result<(), Error> {
        answered("the request was not answered", |error| unsafe {
            sys::zebkit_network_respond_file(self.raw, reply.as_ptr(), path.as_ptr(), error)
        })
    }
    fn answer_stream(&self, reply: &CString) -> Result<NetworkStream, Error> {
        let mut error = std::ptr::null_mut();
        let raw = unsafe { sys::zebkit_network_respond_stream(self.raw, reply.as_ptr(), &mut error) };
        if raw.is_null() {
            return Err(unsafe { crate::take_error(error, "the request was not answered") });
        }
        Ok(NetworkStream { raw: Cell::new(raw), writable: Box::default(), _thread: std::marker::PhantomData })
    }
}

/// An answer written as it goes. Dropped without `end`, it ends as failed: a body
/// cut short does not look whole to the page.
pub struct NetworkStream {
    raw: Cell<*mut sys::zebkit_network_stream>,
    // Boxed: the engine has its address.
    writable: Box<Slot<dyn FnMut()>>,
    _thread: std::marker::PhantomData<Rc<()>>,
}
impl NetworkStream {
    /// All the bytes or none: false when they do not fit (at most 1 MiB waits) or nobody reads.
    pub fn write(&self, bytes: &[u8]) -> bool {
        let raw = self.raw.get();
        !raw.is_null() && unsafe { sys::zebkit_network_stream_write(raw, bytes.as_ptr().cast(), bytes.len()) }
    }
    /// Once after a write that returned false: it fits, or `is_open` is false.
    pub fn on_writable<F: FnMut() + 'static>(&self, function: F) {
        unsafe extern "C" fn trampoline(_: *mut sys::zebkit_network_stream, data: *mut c_void) {
            let slot = &*data.cast_const().cast::<Slot<dyn FnMut()>>();
            slot.call(|function| function());
        }
        let raw = self.raw.get();
        if raw.is_null() { return; }
        self.writable.set(Some(Box::new(function)));
        let data = (&*self.writable as *const Slot<dyn FnMut()>).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_network_stream_on_writable(raw, Some(trampoline), data) };
    }
    pub fn is_open(&self) -> bool {
        let raw = self.raw.get();
        !raw.is_null() && unsafe { sys::zebkit_network_stream_is_open(raw) }
    }
    /// Ends the response after what waits, or as failed at once.
    pub fn end(&self, failed: bool) {
        let raw = self.raw.replace(std::ptr::null_mut());
        if !raw.is_null() { unsafe { sys::zebkit_network_stream_end(raw, failed) }; }
    }
}
impl Drop for NetworkStream {
    fn drop(&mut self) { self.end(true); }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FullscreenPolicy { Allow, Deny }

// Handlers may close the view or replace themselves: the function is out of its
// slot while it runs, and goes back only when neither happened. Setting one
// first removes the engine's function: that settles what the old one had
// pending, which may close the view.
macro_rules! asked {
    ($(#[$doc:meta])* $on:ident, $clear:ident, $slot:ident, $request:ident, $raw:ty, $setter:path) => {
        impl View {
            $(#[$doc])*
            pub fn $on<F: FnMut($request) + 'static>(&self, function: F) {
                unsafe extern "C" fn trampoline(_: *mut sys::zebkit_view, raw: *mut $raw, json: *const c_char, data: *mut c_void) {
                    let inner = hold(data);
                    let metadata = string_from(json).unwrap_or_default();
                    inner.hooks.$slot.call(|function| function($request { raw, metadata, _thread: std::marker::PhantomData }));
                }
                let Some(raw) = self.raw() else { return };
                self.inner.hooks.$slot.set(Some(Box::new(function)));
                unsafe { $setter(raw, None, std::ptr::null_mut()) };
                let Some(raw) = self.raw() else { return };
                let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
                unsafe { $setter(raw, Some(trampoline), data) };
            }
            #[doc = concat!("Removes [`View::", stringify!($on), "`]'s function (settling what it has pending), including from inside it.")]
            pub fn $clear(&self) {
                let Some(raw) = self.raw() else { return };
                self.inner.hooks.$slot.set(None);
                unsafe { $setter(raw, None, std::ptr::null_mut()) };
            }
        }
    };
}
macro_rules! told {
    ($(#[$doc:meta])* $on:ident, $clear:ident, $slot:ident, $setter:path) => {
        impl View {
            $(#[$doc])*
            pub fn $on<F: FnMut(&str) + 'static>(&self, function: F) {
                unsafe extern "C" fn trampoline(_: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
                    let inner = hold(data);
                    let json = string_from(json).unwrap_or_default();
                    inner.hooks.$slot.call(|function| function(&json));
                }
                let Some(raw) = self.raw() else { return };
                self.inner.hooks.$slot.set(Some(Box::new(function)));
                let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
                unsafe { $setter(raw, Some(trampoline), data) };
            }
            #[doc = concat!("Removes [`View::", stringify!($on), "`]'s function, including from inside it.")]
            pub fn $clear(&self) {
                let Some(raw) = self.raw() else { return };
                self.inner.hooks.$slot.set(None);
                unsafe { $setter(raw, None, std::ptr::null_mut()) };
            }
        }
    };
}

asked!(/// Handle permission prompts. Replacing the handler settles its pending prompts.
    on_permission, clear_permission_handler, permission, PermissionRequest, sys::zebkit_permission_request, sys::zebkit_view_on_permission);
asked!(/// Decide document navigations at start, redirects, response, failure or loader-less
    /// commit. Unanswered stages continue; replacing the handler continues its pending stages.
    on_navigation, clear_navigation_handler, navigation, NavigationRequest, sys::zebkit_navigation_request, sys::zebkit_view_on_navigation);
asked!(/// Handle document certificate errors; the default is denial.
    on_certificate, clear_certificate_handler, certificate, CertificateRequest, sys::zebkit_certificate_request, sys::zebkit_view_on_certificate);
asked!(/// Own HTTP and proxy challenges at the browser's login delegate.
    on_http_auth, clear_http_auth_handler, http_auth, HttpAuthRequest, sys::zebkit_http_auth_request, sys::zebkit_view_on_http_auth);
told!(/// Observe asynchronous permission outcomes as JSON.
    on_permission_result, clear_permission_result_handler, permission_result, sys::zebkit_view_on_permission_result);
told!(/// Observe asynchronous navigation decisions as JSON: the stage's metadata with `state`
    /// `"continued"`, `"cancelled"` or `"redirected"` and `reason`, as every hook's outcome.
    on_navigation_result, clear_navigation_result_handler, navigation_result, sys::zebkit_view_on_navigation_result);
told!(/// Observe document certificate outcomes as JSON: `state` `"allowed"` or `"denied"` and
    /// `reason` (`"shutdown"` when the engine stops, `"opener-closed"` for an orphaned popup's).
    on_certificate_result, clear_certificate_result_handler, certificate_result, sys::zebkit_view_on_certificate_result);
told!(/// Observe credential provision and cancellation as JSON, not HTTP success: `state`
    /// `"provided"` or `"cancelled"` and `reason` (`"opener-closed"` for an orphaned popup's).
    on_http_auth_result, clear_http_auth_result_handler, http_auth_result, sys::zebkit_view_on_http_auth_result);
told!(/// Observe asynchronous new-window decisions and creation metadata as JSON.
    on_window_open_event, clear_window_open_event_handler, window_open_event, sys::zebkit_view_on_window_open_event);
told!(/// Observe asynchronous native document-console messages as JSON.
    on_console, clear_console_handler, console, sys::zebkit_view_on_console);
told!(/// Observe every change of the window's state, as JSON.
    on_window_state, clear_window_state_handler, window_state, sys::zebkit_view_on_window_state);
told!(/// Reports a blocked popup as JSON after the decision. The function can navigate
    /// or close this view; it does not reopen the blocked popup.
    on_popup_blocked, clear_popup_blocked_handler, popup_blocked, sys::zebkit_view_on_popup_blocked);
told!(/// How the view's downloads go, asked or not, as JSON: `downloadId`, `guid`, `url`,
    /// `state` (`in-progress`, `complete`, `cancelled`, `interrupted`), `outcome` (`in-progress`,
    /// `complete`, `cancelled`, `failed`), `path`, `savedPath` (once complete), `receivedBytes`,
    /// `totalBytes`, `mimeType`, `reason`, `error` (`{category, code, name, message, resumable}`
    /// for one cancelled or failed, else null), `done`. How an [`View::on_download`] decision
    /// ended is [`View::on_download_result`]'s, not this.
    on_download_progress, clear_download_progress_handler, download_progress, sys::zebkit_view_on_download_progress);
told!(/// The outcome of each download [`View::on_download`]'s function was asked about, as JSON:
    /// the request's metadata with `state` `"saved"` or `"cancelled"` and `reason` (`"answered"`,
    /// `"timeout"`, `"handler-changed"`, `"cancelled"`, `"opener-closed"`, `"shutdown"`), as
    /// every hook's outcome. A popup's go to its opener's function while that decides for it.
    on_download_result, clear_download_result_handler, download_result, sys::zebkit_view_on_download_result);
told!(/// The page's process went (Electron's `render-process-gone`), as JSON: `{"reason",
    /// "exitCode"}`, `reason` one of `"crashed"`, `"killed"`, `"oom"`, `"abnormal-exit"`,
    /// `"clean-exit"`, `"launch-failed"`, `"integrity-failure"`, `"memory-eviction"`. The view
    /// stays open with a blank page: [`View::reload`] or [`View::load_url`] start a process again.
    on_process_gone, clear_process_gone_handler, process_gone, sys::zebkit_view_on_process_gone);
asked!(on_download, clear_download_handler, download, DownloadRequest, sys::zebkit_download_request, sys::zebkit_view_on_download);
asked!(on_client_certificate, clear_client_certificate_handler, client_certificate, ClientCertificateRequest, sys::zebkit_client_certificate_request, sys::zebkit_view_on_client_certificate);
asked!(on_file_chooser, clear_file_chooser_handler, file_chooser, FileChooserRequest, sys::zebkit_file_chooser_request, sys::zebkit_view_on_file_chooser);
asked!(on_media_access, clear_media_access_handler, media_access, MediaAccessRequest, sys::zebkit_media_access_request, sys::zebkit_view_on_media_access);
asked!(on_network, clear_network_handler, network, NetworkRequest, sys::zebkit_network_request, sys::zebkit_view_on_network);
asked!(on_eye_dropper, clear_eye_dropper_handler, eye_dropper, EyeDropperRequest, sys::zebkit_eye_dropper_request, sys::zebkit_view_on_eye_dropper);
asked!(on_device_chooser, clear_device_chooser_handler, device_chooser, DeviceChooserRequest, sys::zebkit_device_chooser_request, sys::zebkit_view_on_device_chooser);
asked!(on_share, clear_share_handler, share, ShareRequest, sys::zebkit_share_request, sys::zebkit_view_on_share);
told!(on_eye_dropper_result, clear_eye_dropper_result_handler, eye_dropper_result, sys::zebkit_view_on_eye_dropper_result);
told!(on_device_chooser_result, clear_device_chooser_result_handler, device_chooser_result, sys::zebkit_view_on_device_chooser_result);
told!(on_share_result, clear_share_result_handler, share_result, sys::zebkit_view_on_share_result);
told!(/// What network rules told, and the outcome of each request they held: `state` `"continued"`,
    /// `"blocked"`, `"redirected"`, `"fulfilled"` or `"refused"` and `reason`; `dropped` (a number)
    /// counts events lost to the cap.
    on_network_event, clear_network_event_handler, network_event, sys::zebkit_view_on_network_event);
told!(on_external_protocol, clear_external_protocol_handler, external_protocol, sys::zebkit_view_on_external_protocol);
told!(on_client_certificate_result, clear_client_certificate_result_handler, client_certificate_result, sys::zebkit_view_on_client_certificate_result);
told!(on_file_chooser_result, clear_file_chooser_result_handler, file_chooser_result, sys::zebkit_view_on_file_chooser_result);
told!(on_media_access_result, clear_media_access_result_handler, media_access_result, sys::zebkit_view_on_media_access_result);
told!(on_reserved_key, clear_reserved_key_handler, reserved_key, sys::zebkit_view_on_reserved_key);
told!(on_context_menu, clear_context_menu_handler, context_menu, sys::zebkit_view_on_context_menu);
told!(on_fullscreen, clear_fullscreen_handler, fullscreen, sys::zebkit_view_on_fullscreen);

impl View {
    /// `function(request id, bytes, end, truncated)` for what a rule with `body: true` matches.
    pub fn on_network_body<F: FnMut(&str, &[u8], bool, bool) + 'static>(&self, function: F) {
        unsafe extern "C" fn trampoline(_: *mut sys::zebkit_view, id: *const c_char, bytes: *const c_void, size: usize, end: bool, truncated: bool, data: *mut c_void) {
            let inner = hold(data);
            let chunk = if size == 0 { &[][..] } else { std::slice::from_raw_parts(bytes.cast::<u8>(), size) };
            let id = string_from(id).unwrap_or_default();
            inner.hooks.network_body.call(|function| function(&id, chunk, end, truncated));
        }
        let Some(raw) = self.raw() else { return };
        self.inner.hooks.network_body.set(Some(Box::new(function)));
        let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_view_on_network_body(raw, Some(trampoline), data) };
    }
    pub fn clear_network_body_handler(&self) {
        let Some(raw) = self.raw() else { return };
        self.inner.hooks.network_body.set(None);
        unsafe { sys::zebkit_view_on_network_body(raw, None, std::ptr::null_mut()) };
    }
    /// This view's filter also decides for its profile's shared and service workers.
    pub fn set_network_filter_for_workers(&self, on: bool) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_network_filter_for_workers(raw, on) })
    }
    /// A JSON array of rules, as `<zebkit/hooks.h>` says; false when refused
    /// ([`View::try_set_network_filter`] says why).
    pub fn set_network_filter(&self, rules_json: &str) -> Result<bool, Error> {
        let rules = cstring(rules_json)?;
        Ok(self.network_filter(&rules).is_ok())
    }
    /// [`View::set_network_filter`], with the engine's reason when it refuses.
    pub fn try_set_network_filter(&self, rules_json: &str) -> Result<(), Error> {
        self.network_filter(&cstring(rules_json)?)
    }
    fn network_filter(&self, rules: &CString) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        answered("network filter rejected", |error| unsafe { sys::zebkit_view_set_network_filter(raw, rules.as_ptr(), error) })
    }
    pub fn set_network_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_network_timeout(raw, milliseconds) })
    }
    pub fn set_download_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_download_timeout(raw, milliseconds) })
    }
    pub fn cancel_download(&self, id: &str) -> bool {
        let Ok(id) = cstring(id) else { return false };
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_cancel_download(raw, id.as_ptr()) })
    }
    pub fn set_client_certificate_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_client_certificate_timeout(raw, milliseconds) })
    }
    pub fn set_file_chooser_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_file_chooser_timeout(raw, milliseconds) })
    }
    pub fn set_media_access_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_media_access_timeout(raw, milliseconds) })
    }
    pub fn set_eye_dropper_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_eye_dropper_timeout(raw, milliseconds) })
    }
    pub fn set_device_chooser_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_device_chooser_timeout(raw, milliseconds) })
    }
    pub fn set_share_timeout(&self, milliseconds: u32) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_share_timeout(raw, milliseconds) })
    }
    /// Runs a default item's command (its id in the context menu event)
    /// against what was right-clicked in menu `menu_id`.
    pub fn context_menu_run(&self, menu_id: u64, command: &str) -> Result<(), Error> {
        let command = cstring(command)?;
        let Some(raw) = self.raw() else { return Err(Error::new("the view is closed")) };
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_view_context_menu_run(raw, menu_id, command.as_ptr(), &mut error) } {
            Ok(())
        } else {
            Err(unsafe { crate::take_error(error, "the context menu's command was refused") })
        }
    }
    /// Shows a menu where menu `menu_id` was asked for: the default items
    /// (`None`) or a JSON list mixing default ids and the application's own;
    /// `function` gets an item of the application's ("" when none is chosen).
    pub fn context_menu_show<F: FnOnce(&str) + 'static>(&self, menu_id: u64, items_json: Option<&str>, function: Option<F>) -> Result<(), Error> {
        unsafe extern "C" fn trampoline<F: FnOnce(&str)>(_: *mut sys::zebkit_view, item: *const c_char, data: *mut c_void) {
            let function = Box::from_raw(data.cast::<F>());
            let item = string_from(item).unwrap_or_default();
            crate::guard(|| function(&item));
        }
        let items = items_json.map(cstring).transpose()?;
        let Some(raw) = self.raw() else { return Err(Error::new("the view is closed")) };
        let (callback, data) = match function {
            Some(function) => (Some(trampoline::<F> as sys::zebkit_context_menu_item_func), Box::into_raw(Box::new(function)).cast::<c_void>()),
            None => (None, std::ptr::null_mut()),
        };
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_view_context_menu_show(raw, menu_id, optional(&items), callback, data, &mut error) } {
            Ok(())
        } else {
            if !data.is_null() {
                drop(unsafe { Box::from_raw(data.cast::<F>()) });
            }
            Err(unsafe { crate::take_error(error, "the context menu was refused") })
        }
    }
    /// A script before the page's, in the named isolated world or (None) the page's.
    pub fn add_world_script(&self, source: &str, world: Option<&str>, main_frame_only: bool) -> Result<bool, Error> {
        let source = cstring(source)?;
        let world = world.map(cstring).transpose()?;
        Ok(self.world_script(&source, &world, main_frame_only).is_ok())
    }
    /// [`View::add_world_script`], with the engine's reason when it refuses.
    pub fn try_add_world_script(&self, source: &str, world: Option<&str>, main_frame_only: bool) -> Result<(), Error> {
        let source = cstring(source)?;
        let world = world.map(cstring).transpose()?;
        self.world_script(&source, &world, main_frame_only)
    }
    fn world_script(&self, source: &CString, world: &Option<CString>, main_frame_only: bool) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        answered("script or world name rejected", |error| unsafe {
            sys::zebkit_view_add_world_script(raw, optional(world), source.as_ptr(), main_frame_only, error)
        })
    }
    /// Keys the page does not get, as a JSON array of "Ctrl+Shift+KeyI".
    pub fn set_reserved_keys(&self, keys_json: &str) -> Result<bool, Error> {
        let keys = cstring(keys_json)?;
        Ok(self.reserved_keys(&keys).is_ok())
    }
    /// [`View::set_reserved_keys`], with the engine's reason when it refuses.
    pub fn try_set_reserved_keys(&self, keys_json: &str) -> Result<(), Error> {
        self.reserved_keys(&cstring(keys_json)?)
    }
    fn reserved_keys(&self, keys: &CString) -> Result<(), Error> {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        answered("reserved keys rejected", |error| unsafe { sys::zebkit_view_set_reserved_keys(raw, keys.as_ptr(), error) })
    }
    pub fn set_fullscreen_policy(&self, policy: FullscreenPolicy) -> bool {
        let policy = if policy == FullscreenPolicy::Allow { 0 } else { 1 };
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_fullscreen_policy(raw, policy) })
    }
}

// zebkit_view_on_responsiveness, kept apart: its engine side is not settled yet.
told!(/// The page's process stopped answering input for a while, `{"responsive": false}`, and
    /// answers again, `{"responsive": true}` (as Chrome's "page unresponsive" dialog, which
    /// zebkit does not show).
    on_responsiveness, clear_responsiveness_handler, responsiveness, sys::zebkit_view_on_responsiveness);
