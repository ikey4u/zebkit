//! The hooks of `<zebkit/hooks.h>` that a page's requests reach: links to other
//! programs, downloads, client certificates, file choosers, media, keys, the
//! context menu and fullscreen. Requests are engine-thread, single-use objects.

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_void, CString};
use std::rc::Rc;

use zebkit_rs as sys;

use crate::view::{Inner, View};
use crate::Error;

fn text(json: *const c_char) -> String {
    if json.is_null() { return String::new(); }
    unsafe { std::ffi::CStr::from_ptr(json) }.to_string_lossy().into_owned()
}
fn owned(raw: *mut c_char) -> Option<String> {
    if raw.is_null() { return None; }
    let value = text(raw);
    unsafe { sys::zebkit_free(raw.cast()) };
    Some(value)
}
fn cstring(value: &str) -> Result<CString, Error> {
    CString::new(value).map_err(|_| Error::new("a string contains NUL"))
}
fn optional(value: &Option<CString>) -> *const c_char {
    value.as_ref().map_or(std::ptr::null(), |value| value.as_ptr())
}

// A function and how many times it was replaced, so that one a handler replaced
// from inside itself is not put back.
pub(crate) struct Hook<T: ?Sized> {
    function: RefCell<Option<Box<T>>>,
    generation: Cell<u64>,
}
impl<T: ?Sized> Default for Hook<T> {
    fn default() -> Self {
        Hook { function: RefCell::new(None), generation: Cell::new(0) }
    }
}
type Asked<T> = Hook<dyn FnMut(T)>;
type Told = Hook<dyn FnMut(&str)>;

macro_rules! slots {
    ($($asked:ident: $request:ty),*; $($told:ident),*) => {
        #[derive(Default)]
        pub(crate) struct Slots {
            $($asked: Asked<$request>,)*
            $($told: Told,)*
        }
        impl Slots {
            pub(crate) fn clear(&self) {
                $(self.$asked.function.borrow_mut().take();)*
                $(self.$told.function.borrow_mut().take();)*
            }
        }
    };
}

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
            pub fn info(&self) -> Option<String> { owned(unsafe { $info(self.raw) }) }
        }
    };
}

request!(/// A download asked about.
    DownloadRequest, sys::zebkit_download_request, sys::zebkit_download_request_info);
request!(/// A client certificate a server asked for.
    ClientCertificateRequest, sys::zebkit_client_certificate_request, sys::zebkit_client_certificate_request_info);
request!(/// A file chooser a page opened.
    FileChooserRequest, sys::zebkit_file_chooser_request, sys::zebkit_file_chooser_request_info);
request!(/// A page's request for camera or microphone.
    MediaAccessRequest, sys::zebkit_media_access_request, sys::zebkit_media_access_request_info);

request!(/// A request or response a network rule holds.
    NetworkRequest, sys::zebkit_network_request, sys::zebkit_network_request_info);

slots!(network: NetworkRequest, download: DownloadRequest, client_certificate: ClientCertificateRequest,
       file_chooser: FileChooserRequest, media_access: MediaAccessRequest;
       external_protocol, download_progress, client_certificate_result, file_chooser_result,
       media_access_result, network_event, reserved_key, context_menu, fullscreen);

impl DownloadRequest {
    /// Save at an absolute path; false when stale or invalid.
    pub fn save(&self, path: &str) -> Result<bool, Error> {
        let path = cstring(path)?;
        Ok(unsafe { sys::zebkit_download_respond(self.raw, 0, path.as_ptr(), std::ptr::null_mut()) })
    }
    pub fn cancel(&self) -> bool {
        unsafe { sys::zebkit_download_respond(self.raw, 1, std::ptr::null(), std::ptr::null_mut()) }
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
        let action = if action == ClientCertificateAction::None { 0 } else { 2 };
        unsafe { sys::zebkit_client_certificate_respond(self.raw, action, std::ptr::null(), std::ptr::null(), std::ptr::null_mut()) }
    }
    /// A PEM chain, leaf first, with its PEM private key.
    pub fn provide(&self, certificate_pem: &str, key_pem: &str) -> Result<bool, Error> {
        let (certificate, key) = (cstring(certificate_pem)?, cstring(key_pem)?);
        Ok(unsafe { sys::zebkit_client_certificate_respond(self.raw, 1, certificate.as_ptr(), key.as_ptr(), std::ptr::null_mut()) })
    }
}

impl FileChooserRequest {
    /// Absolute paths; none cancels.
    pub fn respond(&self, paths: &[&str]) -> Result<bool, Error> {
        let owned = paths.iter().map(|path| cstring(path)).collect::<Result<Vec<_>, _>>()?;
        let pointers: Vec<_> = owned.iter().map(|path| path.as_ptr()).collect();
        Ok(unsafe { sys::zebkit_file_chooser_respond(self.raw, pointers.as_ptr(), pointers.len(), std::ptr::null_mut()) })
    }
}

impl MediaAccessRequest {
    pub fn respond(&self, allow: bool, audio_device: Option<&str>, video_device: Option<&str>) -> Result<bool, Error> {
        let audio = audio_device.map(cstring).transpose()?;
        let video = video_device.map(cstring).transpose()?;
        Ok(unsafe { sys::zebkit_media_access_respond(self.raw, allow, optional(&audio), optional(&video)) })
    }
}

impl NetworkRequest {
    /// A reply as `<zebkit/hooks.h>` says, as JSON; false when stale or invalid.
    pub fn respond(&self, reply_json: &str, body: &[u8]) -> Result<bool, Error> {
        let reply = cstring(reply_json)?;
        Ok(unsafe { sys::zebkit_network_respond(self.raw, reply.as_ptr(), body.as_ptr().cast(), body.len(), std::ptr::null_mut()) })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FullscreenPolicy { Allow, Deny }

// Handlers may close the view or replace themselves: the function is out of its
// slot while it runs, and goes back only when neither happened.
macro_rules! asked {
    ($on:ident, $clear:ident, $slot:ident, $request:ident, $raw:ty, $setter:path) => {
        impl View {
            pub fn $on<F: FnMut($request) + 'static>(&self, function: F) {
                unsafe extern "C" fn trampoline(_: *mut sys::zebkit_view, raw: *mut $raw, json: *const c_char, data: *mut c_void) {
                    let pointer = data.cast_const().cast::<Inner>();
                    Rc::increment_strong_count(pointer);
                    let inner = Rc::from_raw(pointer);
                    let hook = &inner.hooks.$slot;
                    let generation = hook.generation.get();
                    let Some(mut function) = hook.function.borrow_mut().take() else { return };
                    function($request { raw, metadata: text(json), _thread: std::marker::PhantomData });
                    if hook.generation.get() == generation && !inner.raw.get().is_null() {
                        *hook.function.borrow_mut() = Some(function);
                    }
                }
                let Some(raw) = self.raw() else { return };
                let hook = &self.inner.hooks.$slot;
                hook.generation.set(hook.generation.get().wrapping_add(1));
                *hook.function.borrow_mut() = Some(Box::new(function));
                unsafe { $setter(raw, None, std::ptr::null_mut()) };
                let Some(raw) = self.raw() else { return };
                let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
                unsafe { $setter(raw, Some(trampoline), data) };
            }
            pub fn $clear(&self) {
                let Some(raw) = self.raw() else { return };
                let hook = &self.inner.hooks.$slot;
                hook.generation.set(hook.generation.get().wrapping_add(1));
                hook.function.borrow_mut().take();
                unsafe { $setter(raw, None, std::ptr::null_mut()) };
            }
        }
    };
}
macro_rules! told {
    ($on:ident, $clear:ident, $slot:ident, $setter:path) => {
        impl View {
            pub fn $on<F: FnMut(&str) + 'static>(&self, function: F) {
                unsafe extern "C" fn trampoline(_: *mut sys::zebkit_view, json: *const c_char, data: *mut c_void) {
                    let pointer = data.cast_const().cast::<Inner>();
                    Rc::increment_strong_count(pointer);
                    let inner = Rc::from_raw(pointer);
                    let hook = &inner.hooks.$slot;
                    let generation = hook.generation.get();
                    let Some(mut function) = hook.function.borrow_mut().take() else { return };
                    function(&text(json));
                    if hook.generation.get() == generation && !inner.raw.get().is_null() {
                        *hook.function.borrow_mut() = Some(function);
                    }
                }
                let Some(raw) = self.raw() else { return };
                let hook = &self.inner.hooks.$slot;
                hook.generation.set(hook.generation.get().wrapping_add(1));
                *hook.function.borrow_mut() = Some(Box::new(function));
                let data = Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>();
                unsafe { $setter(raw, Some(trampoline), data) };
            }
            pub fn $clear(&self) {
                let Some(raw) = self.raw() else { return };
                let hook = &self.inner.hooks.$slot;
                hook.generation.set(hook.generation.get().wrapping_add(1));
                hook.function.borrow_mut().take();
                unsafe { $setter(raw, None, std::ptr::null_mut()) };
            }
        }
    };
}

asked!(on_download, clear_download_handler, download, DownloadRequest, sys::zebkit_download_request, sys::zebkit_view_on_download);
asked!(on_client_certificate, clear_client_certificate_handler, client_certificate, ClientCertificateRequest, sys::zebkit_client_certificate_request, sys::zebkit_view_on_client_certificate);
asked!(on_file_chooser, clear_file_chooser_handler, file_chooser, FileChooserRequest, sys::zebkit_file_chooser_request, sys::zebkit_view_on_file_chooser);
asked!(on_media_access, clear_media_access_handler, media_access, MediaAccessRequest, sys::zebkit_media_access_request, sys::zebkit_view_on_media_access);
asked!(on_network, clear_network_handler, network, NetworkRequest, sys::zebkit_network_request, sys::zebkit_view_on_network);
told!(on_network_event, clear_network_event_handler, network_event, sys::zebkit_view_on_network_event);
told!(on_external_protocol, clear_external_protocol_handler, external_protocol, sys::zebkit_view_on_external_protocol);
told!(on_download_progress, clear_download_progress_handler, download_progress, sys::zebkit_view_on_download_progress);
told!(on_client_certificate_result, clear_client_certificate_result_handler, client_certificate_result, sys::zebkit_view_on_client_certificate_result);
told!(on_file_chooser_result, clear_file_chooser_result_handler, file_chooser_result, sys::zebkit_view_on_file_chooser_result);
told!(on_media_access_result, clear_media_access_result_handler, media_access_result, sys::zebkit_view_on_media_access_result);
told!(on_reserved_key, clear_reserved_key_handler, reserved_key, sys::zebkit_view_on_reserved_key);
told!(on_context_menu, clear_context_menu_handler, context_menu, sys::zebkit_view_on_context_menu);
told!(on_fullscreen, clear_fullscreen_handler, fullscreen, sys::zebkit_view_on_fullscreen);

impl View {
    /// A JSON array of rules, as `<zebkit/hooks.h>` says.
    pub fn set_network_filter(&self, rules_json: &str) -> Result<bool, Error> {
        let rules = cstring(rules_json)?;
        Ok(self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_network_filter(raw, rules.as_ptr(), std::ptr::null_mut()) }))
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
    /// A script before the page's, in the named isolated world or (None) the page's.
    pub fn add_world_script(&self, source: &str, world: Option<&str>, main_frame_only: bool) -> Result<bool, Error> {
        let source = cstring(source)?;
        let world = world.map(cstring).transpose()?;
        Ok(self.raw().is_some_and(|raw| unsafe {
            sys::zebkit_view_add_world_script(raw, optional(&world), source.as_ptr(), main_frame_only, std::ptr::null_mut())
        }))
    }
    /// Keys the page does not get, as a JSON array of "Ctrl+Shift+KeyI".
    pub fn set_reserved_keys(&self, keys_json: &str) -> Result<bool, Error> {
        let keys = cstring(keys_json)?;
        Ok(self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_reserved_keys(raw, keys.as_ptr(), std::ptr::null_mut()) }))
    }
    pub fn set_fullscreen_policy(&self, policy: FullscreenPolicy) -> bool {
        let policy = if policy == FullscreenPolicy::Allow { 0 } else { 1 };
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_fullscreen_policy(raw, policy) })
    }
}
