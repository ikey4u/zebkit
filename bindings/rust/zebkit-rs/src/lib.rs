//! Raw bindings to the zebkit engine: `libzebkit.so.1` on Linux,
//! `Zebkit.framework` on macOS, `zebkit.dll` on Windows. Declarations only, and everything here is
//! `unsafe`; the `zebkit` crate is the safe interface over them.
//!
//! - The engine, views, the page's calls to the application, script
//!   outside any page and CDP: `<zebkit/zebkit.h>`.
//! - What the engine has for applications besides: compression, digests and
//!   encryption, URLs, images and HTTP (`<zebkit/services.h>`).
//! - Off-screen views, whose frames are shared surfaces and whose input
//!   the application sends (`<zebkit/offscreen.h>`).
//! - [`ffmpeg`]: the FFmpeg inside the engine (`<zebkit/vendor/ffmpeg.h>`),
//!   the part of it the `zebkit` crate uses.
//!
//! Names, types and rules are the C headers'; `docs/API.md` in the
//! repository is their reference. Strings and buffers the engine returns
//! are freed with [`zebkit_free`].

#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_uint, c_void};

/// An opaque type of the engine's, only ever behind a pointer.
macro_rules! opaque {
    ($name:ident) => {
        #[repr(C)]
        pub struct $name {
            _private: [u8; 0],
        }
    };
}

opaque!(zebkit_view);
opaque!(zebkit_context);
opaque!(zebkit_call);
opaque!(zebkit_script);
opaque!(zebkit_navigation_request);
pub type zebkit_navigation_action = c_int;
pub const ZEBKIT_NAVIGATION_CONTINUE: zebkit_navigation_action = 0;
pub const ZEBKIT_NAVIGATION_CANCEL: zebkit_navigation_action = 1;
pub const ZEBKIT_NAVIGATION_REDIRECT: zebkit_navigation_action = 2;
pub type zebkit_native_window_export_func = unsafe extern "C" fn(*mut c_void, *const c_char);
pub type zebkit_navigation_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_navigation_request, *const c_char, *mut c_void);
pub type zebkit_navigation_result_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
pub type zebkit_window_open_event_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
pub type zebkit_console_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
/// `<zebkit/window.h>`: a change of the window's state, as JSON.
pub type zebkit_window_state_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
opaque!(zebkit_certificate_request);
pub type zebkit_certificate_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_certificate_request, *const c_char, *mut c_void);
pub type zebkit_certificate_result_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
opaque!(zebkit_download_request);
pub type zebkit_download_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_download_request, *const c_char, *mut c_void);
opaque!(zebkit_client_certificate_request);
pub type zebkit_client_certificate_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_client_certificate_request, *const c_char, *mut c_void);
opaque!(zebkit_file_chooser_request);
pub type zebkit_file_chooser_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_file_chooser_request, *const c_char, *mut c_void);
opaque!(zebkit_media_access_request);
opaque!(zebkit_eye_dropper_request);
pub type zebkit_eye_dropper_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_eye_dropper_request, *const c_char, *mut c_void);
opaque!(zebkit_device_chooser_request);
pub type zebkit_device_chooser_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_device_chooser_request, *const c_char, *mut c_void);
opaque!(zebkit_share_request);
pub type zebkit_share_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_share_request, *const c_char, *mut c_void);
pub type zebkit_context_menu_item_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
pub type zebkit_media_access_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_media_access_request, *const c_char, *mut c_void);
opaque!(zebkit_network_request);
pub type zebkit_network_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_network_request, *const c_char, *mut c_void);
opaque!(zebkit_network_stream);
pub type zebkit_network_body_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *const c_void, usize, bool, bool, *mut c_void);
pub type zebkit_network_stream_writable_func = unsafe extern "C" fn(*mut zebkit_network_stream, *mut c_void);
pub type zebkit_told_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
opaque!(zebkit_http_auth_request);
pub type zebkit_http_auth_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_http_auth_request, *const c_char, *mut c_void);
pub type zebkit_http_auth_result_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);
opaque!(zebkit_permission_request);
pub type zebkit_permission_state = c_int;
pub const ZEBKIT_PERMISSION_INVALID: zebkit_permission_state = -1;
pub const ZEBKIT_PERMISSION_PROMPT: zebkit_permission_state = 0;
pub const ZEBKIT_PERMISSION_GRANTED: zebkit_permission_state = 1;
pub const ZEBKIT_PERMISSION_DENIED: zebkit_permission_state = 2;
pub type zebkit_permission_func = unsafe extern "C" fn(*mut zebkit_view, *mut zebkit_permission_request, *const c_char, *mut c_void);
pub type zebkit_permission_result_func = unsafe extern "C" fn(*mut zebkit_view, *const c_char, *mut c_void);

pub type zebkit_cookie_storage = c_int;
pub type zebkit_wakeup_func = unsafe extern "C" fn(delay_ms: i64, data: *mut c_void);
pub type zebkit_popup_policy = c_int;
pub const ZEBKIT_POPUP_POLICY_BLOCK_WITHOUT_GESTURE: zebkit_popup_policy = 0;
pub const ZEBKIT_POPUP_POLICY_ALLOW: zebkit_popup_policy = 1;
pub const ZEBKIT_POPUP_POLICY_BLOCK: zebkit_popup_policy = 2;
pub type zebkit_popup_blocked_func =
    unsafe extern "C" fn(view: *mut zebkit_view, event_json: *const c_char, data: *mut c_void);
pub const ZEBKIT_COOKIE_STORAGE_OS: zebkit_cookie_storage = 0;
pub const ZEBKIT_COOKIE_STORAGE_FILE: zebkit_cookie_storage = 1;

pub type zebkit_task_func = unsafe extern "C" fn(data: *mut c_void);

/// `zebkit_load_event`.
pub type zebkit_load_event = c_int;
pub const ZEBKIT_LOAD_STARTED: zebkit_load_event = 0;
pub const ZEBKIT_LOAD_COMMITTED: zebkit_load_event = 1;
pub const ZEBKIT_LOAD_FINISHED: zebkit_load_event = 2;
pub const ZEBKIT_LOAD_FAILED: zebkit_load_event = 3;

pub type zebkit_load_func =
    unsafe extern "C" fn(view: *mut zebkit_view, event: zebkit_load_event, url: *const c_char, data: *mut c_void);
pub type zebkit_view_func = unsafe extern "C" fn(view: *mut zebkit_view, data: *mut c_void);
pub type zebkit_authentication_func =
    unsafe extern "C" fn(view: *mut zebkit_view, event_json: *const c_char, data: *mut c_void);
pub type zebkit_title_func = unsafe extern "C" fn(view: *mut zebkit_view, title: *const c_char, data: *mut c_void);
pub type zebkit_automation_func =
    unsafe extern "C" fn(view: *mut zebkit_view, json: *const c_char, error: *const c_char, data: *mut c_void);
pub type zebkit_automation_pixels_func = unsafe extern "C" fn(
    view: *mut zebkit_view,
    pixels: *const u8,
    width: c_int,
    height: c_int,
    stride: c_int,
    error: *const c_char,
    data: *mut c_void,
);
pub type zebkit_automation_listen_func =
    unsafe extern "C" fn(url: *const c_char, error: *const c_char, data: *mut c_void);
pub type zebkit_automation_event_func =
    unsafe extern "C" fn(view: *mut zebkit_view, event: *const c_char, json: *const c_char, data: *mut c_void);
pub type zebkit_result_func =
    unsafe extern "C" fn(view: *mut zebkit_view, json: *const c_char, error: *const c_char, data: *mut c_void);
pub type zebkit_bind_func = unsafe extern "C" fn(
    view: *mut zebkit_view,
    origin: *const c_char,
    args_json: *const c_char,
    call: *mut zebkit_call,
    data: *mut c_void,
);

/// `zebkit_allow` bits.
pub const ZEBKIT_ALLOW_APP: c_uint = 1 << 0;
pub const ZEBKIT_ALLOW_FETCH: c_uint = 1 << 1;
pub const ZEBKIT_ALLOW_FILES: c_uint = 1 << 2;
pub const ZEBKIT_ALLOW_COMPRESS: c_uint = 1 << 3;
pub const ZEBKIT_ALLOW_CRYPTO: c_uint = 1 << 4;
pub const ZEBKIT_ALLOW_IMAGE: c_uint = 1 << 5;
pub const ZEBKIT_ALLOW_URL: c_uint = 1 << 6;
pub const ZEBKIT_ALLOW_PDF: c_uint = 1 << 7;
pub const ZEBKIT_ALLOW_ALL: c_uint = 0xff;

/// `zebkit_compression`.
pub type zebkit_compression = c_int;
pub const ZEBKIT_COMPRESS_GZIP: zebkit_compression = 1;
pub const ZEBKIT_COMPRESS_ZLIB: zebkit_compression = 2;
pub const ZEBKIT_COMPRESS_DEFLATE_RAW: zebkit_compression = 3;
pub const ZEBKIT_COMPRESS_BROTLI: zebkit_compression = 4;
pub const ZEBKIT_COMPRESS_ZSTD: zebkit_compression = 5;

/// `zebkit_digest`.
pub type zebkit_digest = c_int;
pub const ZEBKIT_DIGEST_SHA1: zebkit_digest = 1;
pub const ZEBKIT_DIGEST_SHA256: zebkit_digest = 2;
pub const ZEBKIT_DIGEST_SHA384: zebkit_digest = 3;
pub const ZEBKIT_DIGEST_SHA512: zebkit_digest = 4;

/// `zebkit_url_part`.
pub type zebkit_url_part = c_int;
pub const ZEBKIT_URL_SPEC: zebkit_url_part = 0;
pub const ZEBKIT_URL_SCHEME: zebkit_url_part = 1;
pub const ZEBKIT_URL_USERNAME: zebkit_url_part = 2;
pub const ZEBKIT_URL_PASSWORD: zebkit_url_part = 3;
pub const ZEBKIT_URL_HOST: zebkit_url_part = 4;
pub const ZEBKIT_URL_PORT: zebkit_url_part = 5;
pub const ZEBKIT_URL_PATH: zebkit_url_part = 6;
pub const ZEBKIT_URL_QUERY: zebkit_url_part = 7;
pub const ZEBKIT_URL_FRAGMENT: zebkit_url_part = 8;
pub const ZEBKIT_URL_ORIGIN: zebkit_url_part = 9;

/// `zebkit_image_format`.
pub type zebkit_image_format = c_int;
pub const ZEBKIT_IMAGE_PNG: zebkit_image_format = 1;
pub const ZEBKIT_IMAGE_JPEG: zebkit_image_format = 2;
pub const ZEBKIT_IMAGE_WEBP: zebkit_image_format = 3;

/// `zebkit_response`: valid during the callback only.
#[repr(C)]
pub struct zebkit_response {
    pub status: c_int,
    pub url: *const c_char,
    pub headers: *const c_char,
    pub body: *const u8,
    pub body_size: usize,
    pub error: *const c_char,
}

pub type zebkit_response_func = unsafe extern "C" fn(response: *const zebkit_response, data: *mut c_void);

extern "C" {
    /// In `zebkit_sandbox.lib`: `<zebkit/win_sandbox.h>`.
    #[cfg(all(windows, target_feature = "crt-static"))]
    pub fn zebkit_win_startup(exit_code: *mut c_int) -> bool;
    pub fn zebkit_version() -> *const c_char;
    pub fn zebkit_engine_version() -> *const c_char;
    pub fn zebkit_free(pointer: *mut c_void);

    pub fn zebkit_set_cookie_storage(storage: zebkit_cookie_storage, key_directory: *const c_char) -> bool;
    pub fn zebkit_set_cookie_encryption_key(key: *const u8, size: usize) -> bool;
    pub fn zebkit_view_get_target_id(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_main(argc: c_int, argv: *mut *mut c_char, ready: Option<zebkit_task_func>, data: *mut c_void)
        -> c_int;
    pub fn zebkit_init(argc: c_int, argv: *mut *mut c_char) -> c_int;
    pub fn zebkit_init_with_settings(argc: c_int, argv: *mut *mut c_char,
        settings_json: *const c_char, error: *mut *mut c_char) -> c_int;
    pub fn zebkit_set_wakeup(func: Option<zebkit_wakeup_func>, data: *mut c_void) -> bool;
    pub fn zebkit_pump() -> bool;
    pub fn zebkit_shutdown() -> bool;
    pub fn zebkit_quit();
    pub fn zebkit_post_task(func: zebkit_task_func, data: *mut c_void) -> bool;
    pub fn zebkit_post_delayed_task(func: zebkit_task_func, data: *mut c_void, delay_ms: u32) -> bool;
    pub fn zebkit_is_headless() -> bool;
    pub fn zebkit_view_set_settings(view: *mut zebkit_view, json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_get_settings(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_set_engine_settings(json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_get_engine_settings() -> *mut c_char;
    pub fn zebkit_view_set_runtime_settings(view: *mut zebkit_view, json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_get_runtime_settings(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_view_on_navigation(view: *mut zebkit_view, func: Option<zebkit_navigation_func>, data: *mut c_void);
    pub fn zebkit_view_on_certificate(view: *mut zebkit_view, func: Option<zebkit_certificate_func>, data: *mut c_void);
    pub fn zebkit_view_on_certificate_result(view: *mut zebkit_view, func: Option<zebkit_certificate_result_func>, data: *mut c_void);
    pub fn zebkit_view_set_certificate_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_certificate_request_info(request: *mut zebkit_certificate_request) -> *mut c_char;
    pub fn zebkit_certificate_respond(request: *mut zebkit_certificate_request, allow: bool) -> bool;
    pub fn zebkit_view_set_network_filter(view: *mut zebkit_view, filter_json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_on_network(view: *mut zebkit_view, func: Option<zebkit_network_func>, data: *mut c_void);
    pub fn zebkit_view_on_network_event(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_network_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_network_request_info(request: *mut zebkit_network_request) -> *mut c_char;
    pub fn zebkit_network_respond(request: *mut zebkit_network_request, reply_json: *const c_char, body: *const c_void, body_size: usize, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_set_network_filter_for_workers(view: *mut zebkit_view, on: bool) -> bool;
    pub fn zebkit_view_on_network_body(view: *mut zebkit_view, func: Option<zebkit_network_body_func>, data: *mut c_void);
    pub fn zebkit_network_respond_file(request: *mut zebkit_network_request, reply_json: *const c_char, path: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_network_respond_stream(request: *mut zebkit_network_request, reply_json: *const c_char, error: *mut *mut c_char) -> *mut zebkit_network_stream;
    pub fn zebkit_network_stream_write(stream: *mut zebkit_network_stream, bytes: *const c_void, size: usize) -> bool;
    pub fn zebkit_network_stream_on_writable(stream: *mut zebkit_network_stream, func: Option<zebkit_network_stream_writable_func>, data: *mut c_void);
    pub fn zebkit_network_stream_is_open(stream: *mut zebkit_network_stream) -> bool;
    pub fn zebkit_network_stream_end(stream: *mut zebkit_network_stream, failed: bool);
    pub fn zebkit_view_on_external_protocol(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_on_download(view: *mut zebkit_view, func: Option<zebkit_download_func>, data: *mut c_void);
    pub fn zebkit_view_on_download_progress(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_download_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_download_request_info(request: *mut zebkit_download_request) -> *mut c_char;
    pub fn zebkit_download_respond(request: *mut zebkit_download_request, action: c_int, path: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_cancel_download(view: *mut zebkit_view, download_id: *const c_char) -> bool;
    pub fn zebkit_view_on_client_certificate(view: *mut zebkit_view, func: Option<zebkit_client_certificate_func>, data: *mut c_void);
    pub fn zebkit_view_on_client_certificate_result(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_client_certificate_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_client_certificate_request_info(request: *mut zebkit_client_certificate_request) -> *mut c_char;
    pub fn zebkit_client_certificate_respond(request: *mut zebkit_client_certificate_request, action: c_int, certificate_pem: *const c_char, key_pem: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_client_certificate_choose(request: *mut zebkit_client_certificate_request, index: usize, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_add_world_script(view: *mut zebkit_view, world: *const c_char, source: *const c_char, main_frame_only: bool, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_on_file_chooser(view: *mut zebkit_view, func: Option<zebkit_file_chooser_func>, data: *mut c_void);
    pub fn zebkit_view_on_file_chooser_result(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_file_chooser_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_file_chooser_request_info(request: *mut zebkit_file_chooser_request) -> *mut c_char;
    pub fn zebkit_file_chooser_respond(request: *mut zebkit_file_chooser_request, paths: *const *const c_char, count: usize, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_on_media_access(view: *mut zebkit_view, func: Option<zebkit_media_access_func>, data: *mut c_void);
    pub fn zebkit_view_on_media_access_result(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_media_access_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_media_access_request_info(request: *mut zebkit_media_access_request) -> *mut c_char;
    pub fn zebkit_media_access_respond(request: *mut zebkit_media_access_request, allow: bool, audio_device_id: *const c_char, video_device_id: *const c_char) -> bool;
    pub fn zebkit_view_set_reserved_keys(view: *mut zebkit_view, keys_json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_on_reserved_key(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_on_context_menu(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_context_menu_run(view: *mut zebkit_view, menu_id: u64, command: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_context_menu_show(view: *mut zebkit_view, menu_id: u64, items_json: *const c_char, func: Option<zebkit_context_menu_item_func>, data: *mut c_void, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_on_eye_dropper(view: *mut zebkit_view, func: Option<zebkit_eye_dropper_func>, data: *mut c_void);
    pub fn zebkit_view_on_eye_dropper_result(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_eye_dropper_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_eye_dropper_request_info(request: *mut zebkit_eye_dropper_request) -> *mut c_char;
    pub fn zebkit_eye_dropper_respond(request: *mut zebkit_eye_dropper_request, color: *const c_char) -> bool;
    pub fn zebkit_view_on_device_chooser(view: *mut zebkit_view, func: Option<zebkit_device_chooser_func>, data: *mut c_void);
    pub fn zebkit_view_on_device_chooser_result(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_device_chooser_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_device_chooser_request_info(request: *mut zebkit_device_chooser_request) -> *mut c_char;
    pub fn zebkit_device_chooser_respond(request: *mut zebkit_device_chooser_request, device_id: *const c_char) -> bool;
    pub fn zebkit_view_on_share(view: *mut zebkit_view, func: Option<zebkit_share_func>, data: *mut c_void);
    pub fn zebkit_view_on_share_result(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_set_share_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_share_request_info(request: *mut zebkit_share_request) -> *mut c_char;
    pub fn zebkit_share_respond(request: *mut zebkit_share_request, shared: bool) -> bool;
    pub fn zebkit_view_set_fullscreen_policy(view: *mut zebkit_view, policy: c_int) -> bool;
    pub fn zebkit_view_on_fullscreen(view: *mut zebkit_view, func: Option<zebkit_told_func>, data: *mut c_void);
    pub fn zebkit_view_on_http_auth(view: *mut zebkit_view, func: Option<zebkit_http_auth_func>, data: *mut c_void);
    pub fn zebkit_view_on_http_auth_result(view: *mut zebkit_view, func: Option<zebkit_http_auth_result_func>, data: *mut c_void);
    pub fn zebkit_view_set_http_auth_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_http_auth_request_info(request: *mut zebkit_http_auth_request) -> *mut c_char;
    pub fn zebkit_http_auth_respond(request: *mut zebkit_http_auth_request, username: *const c_char, password: *const c_char) -> bool;
    pub fn zebkit_view_on_navigation_result(view: *mut zebkit_view, func: Option<zebkit_navigation_result_func>, data: *mut c_void);
    pub fn zebkit_view_on_window_open_event(view: *mut zebkit_view, func: Option<zebkit_window_open_event_func>, data: *mut c_void);
    pub fn zebkit_view_get_window_open_info(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_view_on_console(view: *mut zebkit_view, func: Option<zebkit_console_func>, data: *mut c_void);

    // <zebkit/window.h>
    pub fn zebkit_view_new_with_options(context: *mut zebkit_context, url: *const c_char, options_json: *const c_char, error: *mut *mut c_char) -> *mut zebkit_view;
    pub fn zebkit_view_get_window(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_view_set_window(view: *mut zebkit_view, options_json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_window_action(view: *mut zebkit_view, action: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_on_window_state(view: *mut zebkit_view, func: Option<zebkit_window_state_func>, data: *mut c_void);
    pub fn zebkit_view_set_drag_regions(view: *mut zebkit_view, regions_json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_set_popup_window_options(view: *mut zebkit_view, options_json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_get_capabilities() -> *mut c_char;
    pub fn zebkit_get_environment() -> *mut c_char;
    pub fn zebkit_on_environment_changed(func: Option<unsafe extern "C" fn(*const c_char, *mut c_void)>, data: *mut c_void);
    pub fn zebkit_view_set_navigation_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_view_set_navigation_fail_closed(view: *mut zebkit_view, fail_closed: bool) -> bool;
    pub fn zebkit_navigation_request_info(request: *mut zebkit_navigation_request) -> *mut c_char;
    pub fn zebkit_navigation_respond(request: *mut zebkit_navigation_request, action: zebkit_navigation_action, replacement_url: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_set_permission(view: *mut zebkit_view, origin: *const c_char, permission: *const c_char, state: zebkit_permission_state, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_get_permission(view: *mut zebkit_view, origin: *const c_char, permission: *const c_char) -> zebkit_permission_state;
    pub fn zebkit_view_reset_permissions(view: *mut zebkit_view, origin: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_set_permission_timeout(view: *mut zebkit_view, milliseconds: u32) -> bool;
    pub fn zebkit_view_on_permission(view: *mut zebkit_view, func: Option<zebkit_permission_func>, data: *mut c_void);
    pub fn zebkit_view_on_permission_result(view: *mut zebkit_view, func: Option<zebkit_permission_result_func>, data: *mut c_void);
    pub fn zebkit_permission_request_info(request: *mut zebkit_permission_request) -> *mut c_char;
    pub fn zebkit_permission_respond(request: *mut zebkit_permission_request, state: zebkit_permission_state, remember: bool) -> bool;
    pub fn zebkit_view_set_popup_policy(view: *mut zebkit_view, policy: zebkit_popup_policy) -> bool;
    pub fn zebkit_view_get_popup_policy(view: *mut zebkit_view) -> zebkit_popup_policy;
    pub fn zebkit_view_on_popup_blocked(view: *mut zebkit_view, func: Option<zebkit_popup_blocked_func>, data: *mut c_void);

    pub fn zebkit_view_new(url: *const c_char, width: c_int, height: c_int) -> *mut zebkit_view;
    pub fn zebkit_context_new() -> *mut zebkit_context;
    pub fn zebkit_context_close(context: *mut zebkit_context);
    pub fn zebkit_view_new_in(context: *mut zebkit_context, url: *const c_char, width: c_int, height: c_int) -> *mut zebkit_view;
    pub fn zebkit_view_load_url(view: *mut zebkit_view, url: *const c_char);
    pub fn zebkit_view_reload(view: *mut zebkit_view);
    pub fn zebkit_view_go_back(view: *mut zebkit_view);
    pub fn zebkit_view_go_forward(view: *mut zebkit_view);
    pub fn zebkit_view_resize(view: *mut zebkit_view, width: c_int, height: c_int);
    pub fn zebkit_view_set_size_limits(view: *mut zebkit_view, min_width: c_int, min_height: c_int, resizable: bool);
    pub fn zebkit_view_close(view: *mut zebkit_view);
    pub fn zebkit_view_get_url(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_view_get_title(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_view_on_load(view: *mut zebkit_view, func: Option<zebkit_load_func>, data: *mut c_void);
    pub fn zebkit_view_on_title(view: *mut zebkit_view, func: Option<zebkit_title_func>, data: *mut c_void);
    pub fn zebkit_view_on_close(view: *mut zebkit_view, func: Option<zebkit_view_func>, data: *mut c_void);
    pub fn zebkit_view_evaluate(
        view: *mut zebkit_view,
        script: *const c_char,
        func: Option<zebkit_result_func>,
        data: *mut c_void,
    );
    pub fn zebkit_view_grant(view: *mut zebkit_view, origin: *const c_char, allow: c_uint, files_root: *const c_char);
    pub fn zebkit_view_bind(view: *mut zebkit_view, name: *const c_char, func: Option<zebkit_bind_func>, data: *mut c_void);
    pub fn zebkit_call_resolve(call: *mut zebkit_call, json: *const c_char);
    pub fn zebkit_call_resolve_json(call: *mut zebkit_call, json: *const c_char, length: usize);
    pub fn zebkit_call_reject(call: *mut zebkit_call, message: *const c_char);

    pub fn zebkit_script_new(argc: c_int, argv: *const *const c_char) -> *mut zebkit_script;
    pub fn zebkit_script_run_module(
        script: *mut zebkit_script,
        path: *const c_char,
        func: Option<zebkit_result_func>,
        data: *mut c_void,
    );
    pub fn zebkit_script_evaluate(
        script: *mut zebkit_script,
        source: *const c_char,
        func: Option<zebkit_result_func>,
        data: *mut c_void,
    );
    pub fn zebkit_script_on_idle(script: *mut zebkit_script, func: Option<zebkit_task_func>, data: *mut c_void);
    pub fn zebkit_script_destroy(script: *mut zebkit_script);

    pub fn zebkit_cdp_listen(address: *const c_char, port: c_uint, error: *mut *mut c_char) -> c_uint;
    pub fn zebkit_cdp_get_port() -> c_uint;
    pub fn zebkit_cdp_get_url() -> *mut c_char;
    pub fn zebkit_cdp_stop();

    pub fn zebkit_compress(
        format: zebkit_compression,
        data: *const c_void,
        size: usize,
        level: c_int,
        out_size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_decompress(
        format: zebkit_compression,
        data: *const c_void,
        size: usize,
        max_size: usize,
        out_size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_digest_size(digest: zebkit_digest) -> usize;
    pub fn zebkit_digest_compute(digest: zebkit_digest, data: *const c_void, size: usize, out: *mut u8) -> bool;
    pub fn zebkit_hmac(
        digest: zebkit_digest,
        key: *const c_void,
        key_size: usize,
        data: *const c_void,
        size: usize,
        out: *mut u8,
    ) -> bool;
    pub fn zebkit_random_bytes(out: *mut c_void, size: usize) -> bool;
    pub fn zebkit_aes_gcm_seal(
        key: *const c_void,
        key_size: usize,
        nonce: *const c_void,
        nonce_size: usize,
        ad: *const c_void,
        ad_size: usize,
        plaintext: *const c_void,
        size: usize,
        out_size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_aes_gcm_open(
        key: *const c_void,
        key_size: usize,
        nonce: *const c_void,
        nonce_size: usize,
        ad: *const c_void,
        ad_size: usize,
        sealed: *const c_void,
        size: usize,
        out_size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_url_get(url: *const c_char, base: *const c_char, part: zebkit_url_part) -> *mut c_char;
    pub fn zebkit_url_registrable_domain(host_or_url: *const c_char) -> *mut c_char;
    pub fn zebkit_image_decode(
        data: *const c_void,
        size: usize,
        width: *mut c_int,
        height: *mut c_int,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_image_encode(
        format: zebkit_image_format,
        rgba: *const u8,
        width: c_int,
        height: c_int,
        quality: c_int,
        out_size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_icu_init() -> bool;
    pub fn zebkit_fetch(
        method: *const c_char,
        url: *const c_char,
        headers: *const c_char,
        body: *const c_void,
        body_size: usize,
        func: zebkit_response_func,
        data: *mut c_void,
    );
}

pub type zebkit_capture_func =
    unsafe extern "C" fn(view: *mut zebkit_view, png: *const u8, size: usize, error: *const c_char, data: *mut c_void);
pub type zebkit_url_func = unsafe extern "C" fn(view: *mut zebkit_view, url: *const c_char, data: *mut c_void);
pub type zebkit_window_open_func =
    unsafe extern "C" fn(opener: *mut zebkit_view, opened: *mut zebkit_view, url: *const c_char, data: *mut c_void);
pub type zebkit_drop_func = unsafe extern "C" fn(view: *mut zebkit_view, paths_json: *const c_char, data: *mut c_void);

extern "C" {
    pub fn zebkit_view_set_title(view: *mut zebkit_view, title: *const c_char);
    pub fn zebkit_view_focus(view: *mut zebkit_view);
    pub fn zebkit_view_attach(
        view: *mut zebkit_view,
        parent: *mut zebkit_view,
        x: c_int,
        y: c_int,
        width: c_int,
        height: c_int,
    ) -> bool;
    pub fn zebkit_view_attach_native(view: *mut zebkit_view, native_parent: usize, x: c_int, y: c_int, width: c_int, height: c_int, error: *mut *mut c_char) -> bool;
    pub fn zebkit_view_detach_native(view: *mut zebkit_view) -> bool;
    pub fn zebkit_view_get_native_window(view: *mut zebkit_view, window: *mut zebkit_native_window) -> bool;
    pub fn zebkit_view_export_native_window(view: *mut zebkit_view, callback: Option<zebkit_native_window_export_func>, data: *mut c_void) -> bool;
    pub fn zebkit_view_set_bounds(view: *mut zebkit_view, x: c_int, y: c_int, width: c_int, height: c_int);
    pub fn zebkit_view_set_visible(view: *mut zebkit_view, visible: bool);
    pub fn zebkit_view_add_script(view: *mut zebkit_view, source: *const c_char);
    pub fn zebkit_view_stop(view: *mut zebkit_view);
    pub fn zebkit_view_is_loading(view: *mut zebkit_view) -> bool;
    pub fn zebkit_view_set_user_agent(view: *mut zebkit_view, user_agent: *const c_char);
    pub fn zebkit_view_set_user_agent_override(view: *mut zebkit_view, json: *const c_char,
        func: Option<zebkit_result_func>, data: *mut c_void) -> bool;
    pub fn zebkit_view_capture(view: *mut zebkit_view, func: zebkit_capture_func, data: *mut c_void);
    pub fn zebkit_view_on_new_window(view: *mut zebkit_view, func: Option<zebkit_url_func>, data: *mut c_void);
    pub fn zebkit_view_on_window_open(view: *mut zebkit_view, func: Option<zebkit_window_open_func>, data: *mut c_void);
    pub fn zebkit_view_on_drop(view: *mut zebkit_view, func: Option<zebkit_drop_func>, data: *mut c_void);
    pub fn zebkit_pdf_redact(
        pdf: *mut zebkit_pdf,
        page: c_int,
        areas_json: *const c_char,
        fill: u32,
        error: *mut *mut c_char,
    ) -> c_int;
    pub fn zebkit_pdf_form_fields(pdf: *mut zebkit_pdf, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_form_set(
        pdf: *mut zebkit_pdf,
        name: *const c_char,
        value: *const c_char,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_form_reset(pdf: *mut zebkit_pdf, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_page_rotate(pdf: *mut zebkit_pdf, page: c_int, rotation: c_int, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_pages_delete(pdf: *mut zebkit_pdf, pages: *const c_int, count: c_int, error: *mut *mut c_char)
        -> bool;
    pub fn zebkit_pdf_pages_move(
        pdf: *mut zebkit_pdf,
        pages: *const c_int,
        count: c_int,
        to: c_int,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_page_insert(pdf: *mut zebkit_pdf, at: c_int, width: f64, height: f64, error: *mut *mut c_char)
        -> bool;
    pub fn zebkit_pdf_pages_import(
        pdf: *mut zebkit_pdf,
        from: *mut zebkit_pdf,
        pages: *const c_int,
        count: c_int,
        at: c_int,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_new(error: *mut *mut c_char) -> *mut zebkit_pdf;
}

opaque!(zebkit_pdf);
opaque!(zebkit_canvas);
opaque!(zebkit_canvas_font);
opaque!(zebkit_request);

/// `zebkit_pdf_render_options`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct zebkit_pdf_render_options {
    pub scale: f32,
    pub rotation: c_int,
    pub flags: c_uint,
    pub background: u32,
    pub x: c_int,
    pub y: c_int,
    pub width: c_int,
    pub height: c_int,
}

pub const ZEBKIT_PDF_SAVE_INCREMENTAL: c_uint = 1 << 0;
pub const ZEBKIT_PDF_SAVE_NO_SECURITY: c_uint = 1 << 1;
pub const ZEBKIT_PDF_RENDER_ANNOTATIONS: c_uint = 1 << 0;
pub const ZEBKIT_PDF_RENDER_FORMS: c_uint = 1 << 1;
pub const ZEBKIT_PDF_RENDER_LCD_TEXT: c_uint = 1 << 2;
pub const ZEBKIT_PDF_RENDER_GRAYSCALE: c_uint = 1 << 3;
pub const ZEBKIT_PDF_RENDER_PRINTING: c_uint = 1 << 4;
pub const ZEBKIT_PDF_RENDER_NO_SMOOTHING: c_uint = 1 << 5;
pub const ZEBKIT_PDF_SEARCH_CASE: c_uint = 1 << 0;
pub const ZEBKIT_PDF_SEARCH_WHOLE_WORD: c_uint = 1 << 1;

pub const ZEBKIT_CANVAS_UNPREMULTIPLIED: c_uint = 1 << 0;
pub const ZEBKIT_PDF_XFDF: c_int = 0;
pub const ZEBKIT_PDF_FDF: c_int = 1;
pub const ZEBKIT_PDF_CONTINUE: c_int = 0;
pub const ZEBKIT_PDF_REFUSE: c_int = 1;
pub const ZEBKIT_PDF_LAYER_BELOW: c_int = 0;
pub const ZEBKIT_PDF_LAYER_ABOVE: c_int = 1;
pub const ZEBKIT_PDF_UNTRUSTED: c_int = -1;
pub const ZEBKIT_PDF_TRUST_UNKNOWN: c_int = 0;
pub const ZEBKIT_PDF_TRUSTED: c_int = 1;

pub type zebkit_pdf_before_func = unsafe extern "C" fn(
    pdf: *mut zebkit_pdf,
    operation: *const c_char,
    change: *mut *mut c_char,
    reason: *mut *mut c_char,
    data: *mut c_void,
) -> c_int;
pub type zebkit_pdf_after_func = unsafe extern "C" fn(pdf: *mut zebkit_pdf, operation: *const c_char, data: *mut c_void);
pub type zebkit_pdf_appearance_func = unsafe extern "C" fn(
    canvas: *mut zebkit_canvas,
    pdf: *mut zebkit_pdf,
    page: c_int,
    index: c_int,
    annotation: *const c_char,
    width: f64,
    height: f64,
    data: *mut c_void,
) -> bool;
pub type zebkit_pdf_destroy_func = unsafe extern "C" fn(data: *mut c_void);

/// `zebkit_pdf_kind`.
#[repr(C)]
pub struct zebkit_pdf_kind {
    pub name: *const c_char,
    pub prefix: *const c_char,
    pub subtype: *const c_char,
    pub schema: *const c_char,
    pub check: Option<unsafe extern "C" fn(data_json: *const c_char, data: *mut c_void) -> *mut c_char>,
    pub draw: Option<zebkit_pdf_appearance_func>,
    pub hit_test: Option<
        unsafe extern "C" fn(
            pdf: *mut zebkit_pdf,
            page: c_int,
            index: c_int,
            annotation: *const c_char,
            x: f64,
            y: f64,
            data: *mut c_void,
        ) -> bool,
    >,
    pub data: *mut c_void,
    pub destroy: Option<zebkit_pdf_destroy_func>,
}

/// `zebkit_pdf_layer_info`.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct zebkit_pdf_layer_info {
    pub width: c_int,
    pub height: c_int,
    pub scale: f64,
    pub rotation: c_int,
    pub matrix: [f64; 6],
}

pub type zebkit_pdf_layer_func = unsafe extern "C" fn(
    canvas: *mut zebkit_canvas,
    pdf: *mut zebkit_pdf,
    page: c_int,
    info: *const zebkit_pdf_layer_info,
    data: *mut c_void,
);
pub type zebkit_pdf_ocr_func = unsafe extern "C" fn(
    pdf: *mut zebkit_pdf,
    page: c_int,
    rgba: *const u8,
    width: c_int,
    height: c_int,
    scale: f64,
    data: *mut c_void,
) -> *mut c_char;
pub type zebkit_pdf_font_func =
    unsafe extern "C" fn(pdf: *mut zebkit_pdf, family: *const c_char, size: *mut usize, data: *mut c_void) -> *mut u8;
pub type zebkit_pdf_trust_func = unsafe extern "C" fn(
    pdf: *mut zebkit_pdf,
    signature: *const c_char,
    contents: *const u8,
    size: usize,
    data: *mut c_void,
) -> c_int;
pub type zebkit_pdf_action_func =
    unsafe extern "C" fn(pdf: *mut zebkit_pdf, action: *const c_char, data: *mut c_void) -> bool;
pub type zebkit_pdf_input_func = unsafe extern "C" fn(
    pdf: *mut zebkit_pdf,
    tool: *const c_char,
    points: *const c_char,
    data: *mut c_void,
) -> *mut c_char;

extern "C" {
    pub fn zebkit_text_shape(
        font: *const c_void,
        font_size: usize,
        size: f64,
        text: *const c_char,
        language: *const c_char,
        direction: *const c_char,
        error: *mut *mut c_char,
    ) -> *mut c_char;

    pub fn zebkit_canvas_new(width: c_int, height: c_int, error: *mut *mut c_char) -> *mut zebkit_canvas;
    pub fn zebkit_canvas_wrap(
        rgba: *mut u8,
        width: c_int,
        height: c_int,
        stride: usize,
        flags: c_uint,
        error: *mut *mut c_char,
    ) -> *mut zebkit_canvas;
    pub fn zebkit_canvas_close(canvas: *mut zebkit_canvas);
    pub fn zebkit_canvas_width(canvas: *mut zebkit_canvas) -> c_int;
    pub fn zebkit_canvas_height(canvas: *mut zebkit_canvas) -> c_int;
    pub fn zebkit_canvas_pixels(canvas: *mut zebkit_canvas, error: *mut *mut c_char) -> *mut u8;
    pub fn zebkit_canvas_png(canvas: *mut zebkit_canvas, size: *mut usize, error: *mut *mut c_char) -> *mut u8;
    pub fn zebkit_canvas_save(canvas: *mut zebkit_canvas);
    pub fn zebkit_canvas_restore(canvas: *mut zebkit_canvas);
    pub fn zebkit_canvas_translate(canvas: *mut zebkit_canvas, x: f64, y: f64);
    pub fn zebkit_canvas_scale(canvas: *mut zebkit_canvas, x: f64, y: f64);
    pub fn zebkit_canvas_rotate(canvas: *mut zebkit_canvas, degrees: f64);
    pub fn zebkit_canvas_transform(canvas: *mut zebkit_canvas, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64);
    pub fn zebkit_canvas_set_matrix(canvas: *mut zebkit_canvas, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64);
    pub fn zebkit_canvas_set_fill_color(canvas: *mut zebkit_canvas, rgba: u32);
    pub fn zebkit_canvas_set_stroke_color(canvas: *mut zebkit_canvas, rgba: u32);
    pub fn zebkit_canvas_set_alpha(canvas: *mut zebkit_canvas, alpha: f64);
    pub fn zebkit_canvas_set_blend(canvas: *mut zebkit_canvas, mode: *const c_char) -> bool;
    pub fn zebkit_canvas_set_line_width(canvas: *mut zebkit_canvas, width: f64);
    pub fn zebkit_canvas_set_line_cap(canvas: *mut zebkit_canvas, cap: c_int);
    pub fn zebkit_canvas_set_line_join(canvas: *mut zebkit_canvas, join: c_int);
    pub fn zebkit_canvas_set_miter_limit(canvas: *mut zebkit_canvas, limit: f64);
    pub fn zebkit_canvas_set_dash(canvas: *mut zebkit_canvas, lengths: *const f64, count: c_int, phase: f64);
    pub fn zebkit_canvas_set_linear_gradient(
        canvas: *mut zebkit_canvas,
        x0: f64,
        y0: f64,
        x1: f64,
        y1: f64,
        offsets: *const f64,
        colors: *const u32,
        count: c_int,
    ) -> bool;
    pub fn zebkit_canvas_set_radial_gradient(
        canvas: *mut zebkit_canvas,
        x: f64,
        y: f64,
        radius: f64,
        offsets: *const f64,
        colors: *const u32,
        count: c_int,
    ) -> bool;
    pub fn zebkit_canvas_begin_path(canvas: *mut zebkit_canvas);
    pub fn zebkit_canvas_move_to(canvas: *mut zebkit_canvas, x: f64, y: f64);
    pub fn zebkit_canvas_line_to(canvas: *mut zebkit_canvas, x: f64, y: f64);
    pub fn zebkit_canvas_quad_to(canvas: *mut zebkit_canvas, cx: f64, cy: f64, x: f64, y: f64);
    pub fn zebkit_canvas_curve_to(canvas: *mut zebkit_canvas, c1x: f64, c1y: f64, c2x: f64, c2y: f64, x: f64, y: f64);
    pub fn zebkit_canvas_arc(
        canvas: *mut zebkit_canvas,
        x: f64,
        y: f64,
        radius: f64,
        start: f64,
        end: f64,
        counterclockwise: bool,
    );
    pub fn zebkit_canvas_rect(canvas: *mut zebkit_canvas, x: f64, y: f64, width: f64, height: f64);
    pub fn zebkit_canvas_close_path(canvas: *mut zebkit_canvas);
    pub fn zebkit_canvas_fill(canvas: *mut zebkit_canvas, even_odd: bool);
    pub fn zebkit_canvas_stroke(canvas: *mut zebkit_canvas);
    pub fn zebkit_canvas_clip(canvas: *mut zebkit_canvas, even_odd: bool);
    pub fn zebkit_canvas_clear(canvas: *mut zebkit_canvas, rgba: u32);
    pub fn zebkit_canvas_draw_image(
        canvas: *mut zebkit_canvas,
        rgba: *const u8,
        width: c_int,
        height: c_int,
        x: f64,
        y: f64,
        dest_width: f64,
        dest_height: f64,
    ) -> bool;
    pub fn zebkit_canvas_font_new(data: *const c_void, size: usize, error: *mut *mut c_char) -> *mut zebkit_canvas_font;
    pub fn zebkit_canvas_font_close(font: *mut zebkit_canvas_font);
    pub fn zebkit_canvas_set_font(canvas: *mut zebkit_canvas, font: *mut zebkit_canvas_font, size: f64);
    pub fn zebkit_canvas_fill_text(
        canvas: *mut zebkit_canvas,
        text: *const c_char,
        x: f64,
        y: f64,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_canvas_text_path(
        canvas: *mut zebkit_canvas,
        text: *const c_char,
        x: f64,
        y: f64,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_canvas_measure_text(canvas: *mut zebkit_canvas, text: *const c_char) -> f64;
    pub fn zebkit_canvas_run(canvas: *mut zebkit_canvas, ops: *const c_char, error: *mut *mut c_char) -> bool;

    pub fn zebkit_pdf_annotation_at(
        pdf: *mut zebkit_pdf,
        page: c_int,
        x: f64,
        y: f64,
        tolerance: f64,
        error: *mut *mut c_char,
    ) -> c_int;
    pub fn zebkit_pdf_annotation_move(pdf: *mut zebkit_pdf, page: c_int, index: c_int, to: c_int, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_annotation_copy(
        pdf: *mut zebkit_pdf,
        page: c_int,
        index: c_int,
        target: *mut zebkit_pdf,
        target_page: c_int,
        error: *mut *mut c_char,
    ) -> c_int;
    pub fn zebkit_pdf_annotation_flatten(pdf: *mut zebkit_pdf, page: c_int, index: c_int, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_annotation_stamp_page(
        pdf: *mut zebkit_pdf,
        page: c_int,
        index: c_int,
        source: *mut zebkit_pdf,
        source_page: c_int,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_annotation_attach_file(
        pdf: *mut zebkit_pdf,
        page: c_int,
        index: c_int,
        name: *const c_char,
        data: *const c_void,
        size: usize,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_annotation_file(
        pdf: *mut zebkit_pdf,
        page: c_int,
        index: c_int,
        name: *mut *mut c_char,
        size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_pdf_annotation_draw(
        pdf: *mut zebkit_pdf,
        page: c_int,
        index: c_int,
        ops: *const c_char,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_annotation_action(pdf: *mut zebkit_pdf, page: c_int, index: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_annotations_export(
        pdf: *mut zebkit_pdf,
        format: c_int,
        page: c_int,
        index: c_int,
        size: *mut usize,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_pdf_annotations_import(
        pdf: *mut zebkit_pdf,
        format: c_int,
        data: *const c_void,
        size: usize,
        error: *mut *mut c_char,
    ) -> c_int;
    pub fn zebkit_pdf_string(text: *const c_char) -> *mut c_char;
    pub fn zebkit_pdf_bytes(data: *const c_void, size: usize) -> *mut u8;
    pub fn zebkit_pdf_hook_remove(hook: c_int) -> bool;
    pub fn zebkit_pdf_hook_before(
        pdf: *mut zebkit_pdf,
        event: *const c_char,
        func: zebkit_pdf_before_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_hook_after(
        pdf: *mut zebkit_pdf,
        event: *const c_char,
        func: zebkit_pdf_after_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_apply(pdf: *mut zebkit_pdf, operation: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_hook_appearance(
        pdf: *mut zebkit_pdf,
        kind: *const c_char,
        func: zebkit_pdf_appearance_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_hook_kind(pdf: *mut zebkit_pdf, kind: *const zebkit_pdf_kind, error: *mut *mut c_char) -> c_int;
    pub fn zebkit_pdf_page_data(pdf: *mut zebkit_pdf, page: c_int, prefix: *const c_char, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_hook_layer(
        pdf: *mut zebkit_pdf,
        position: c_int,
        func: zebkit_pdf_layer_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_hook_ocr(
        pdf: *mut zebkit_pdf,
        func: zebkit_pdf_ocr_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_hook_font(
        pdf: *mut zebkit_pdf,
        func: zebkit_pdf_font_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_font_add(
        pdf: *mut zebkit_pdf,
        family: *const c_char,
        font: *const c_void,
        size: usize,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_hook_trust(
        pdf: *mut zebkit_pdf,
        func: zebkit_pdf_trust_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_hook_action(
        pdf: *mut zebkit_pdf,
        func: zebkit_pdf_action_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_hook_input(
        pdf: *mut zebkit_pdf,
        tool: *const c_char,
        func: zebkit_pdf_input_func,
        data: *mut c_void,
        destroy: Option<zebkit_pdf_destroy_func>,
    ) -> c_int;
    pub fn zebkit_pdf_filter_input(
        pdf: *mut zebkit_pdf,
        tool: *const c_char,
        points: *const c_char,
        options: *const c_char,
        error: *mut *mut c_char,
    ) -> *mut c_char;
}

pub const ZEBKIT_SCHEME_STANDARD: c_uint = 1 << 0;
pub const ZEBKIT_SCHEME_SECURE: c_uint = 1 << 1;
pub const ZEBKIT_SCHEME_FETCH: c_uint = 1 << 2;
pub const ZEBKIT_SCHEME_BYPASS_CSP: c_uint = 1 << 3;

pub type zebkit_scheme_func = unsafe extern "C" fn(request: *mut zebkit_request, data: *mut c_void);
pub type zebkit_menu_func = unsafe extern "C" fn(id: *const c_char, data: *mut c_void);
pub type zebkit_paths_func = unsafe extern "C" fn(paths_json: *const c_char, data: *mut c_void);
pub type zebkit_app_func = unsafe extern "C" fn(data: *mut c_void);
pub type zebkit_system_event_func = unsafe extern "C" fn(json: *const c_char, data: *mut c_void);
pub type zebkit_notification_func = unsafe extern "C" fn(json: *const c_char, data: *mut c_void);
opaque!(zebkit_tray);

extern "C" {
    pub fn zebkit_pdf_open(data: *const c_void, size: usize, password: *const c_char, error: *mut *mut c_char)
        -> *mut zebkit_pdf;
    pub fn zebkit_pdf_open_file(path: *const c_char, password: *const c_char, error: *mut *mut c_char)
        -> *mut zebkit_pdf;
    pub fn zebkit_pdf_close(pdf: *mut zebkit_pdf);
    pub fn zebkit_pdf_save(pdf: *mut zebkit_pdf, flags: c_uint, size: *mut usize, error: *mut *mut c_char) -> *mut u8;
    pub fn zebkit_pdf_info(pdf: *mut zebkit_pdf) -> *mut c_char;
    pub fn zebkit_pdf_set_metadata(pdf: *mut zebkit_pdf, json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_page_count(pdf: *mut zebkit_pdf) -> c_int;
    pub fn zebkit_pdf_page(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_render(
        pdf: *mut zebkit_pdf,
        page: c_int,
        options: *const zebkit_pdf_render_options,
        width: *mut c_int,
        height: *mut c_int,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_pdf_render_annotation(
        pdf: *mut zebkit_pdf,
        page: c_int,
        annotation: c_int,
        mode: c_int,
        options: *const zebkit_pdf_render_options,
        width: *mut c_int,
        height: *mut c_int,
        error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_pdf_text(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_text_lines(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_images(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_signatures(pdf: *mut zebkit_pdf, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_signature_contents(
        pdf: *mut zebkit_pdf, index: c_int, size: *mut usize, error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_pdf_image_data(
        pdf: *mut zebkit_pdf, page: c_int, image: c_int, size: *mut usize, jpeg: *mut bool, error: *mut *mut c_char,
    ) -> *mut u8;
    pub fn zebkit_pdf_text_layout(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_text_rects(
        pdf: *mut zebkit_pdf,
        page: c_int,
        start: c_int,
        count: c_int,
        error: *mut *mut c_char,
    ) -> *mut c_char;
    pub fn zebkit_pdf_char_at(pdf: *mut zebkit_pdf, page: c_int, x: f64, y: f64, tolerance: f64) -> c_int;
    pub fn zebkit_pdf_search(
        pdf: *mut zebkit_pdf,
        query: *const c_char,
        flags: c_uint,
        first: c_int,
        limit: c_int,
        error: *mut *mut c_char,
    ) -> *mut c_char;
    pub fn zebkit_pdf_set_outline(pdf: *mut zebkit_pdf, json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_outline(pdf: *mut zebkit_pdf, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_links(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_annotations(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_annotation_add(
        pdf: *mut zebkit_pdf,
        page: c_int,
        json: *const c_char,
        error: *mut *mut c_char,
    ) -> c_int;
    pub fn zebkit_pdf_annotation_update(
        pdf: *mut zebkit_pdf,
        page: c_int,
        index: c_int,
        json: *const c_char,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_annotation_remove(pdf: *mut zebkit_pdf, page: c_int, index: c_int, error: *mut *mut c_char)
        -> bool;
    pub fn zebkit_pdf_annotation_set_image(
        pdf: *mut zebkit_pdf, page: c_int, index: c_int, rgba: *const u8, width: c_int, height: c_int,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_flatten(pdf: *mut zebkit_pdf, page: c_int, for_print: bool, error: *mut *mut c_char) -> bool;
    pub fn zebkit_pdf_attachments(pdf: *mut zebkit_pdf, error: *mut *mut c_char) -> *mut c_char;
    pub fn zebkit_pdf_attachment(pdf: *mut zebkit_pdf, index: c_int, size: *mut usize, error: *mut *mut c_char)
        -> *mut u8;
    pub fn zebkit_pdf_attach(
        pdf: *mut zebkit_pdf,
        name: *const c_char,
        data: *const c_void,
        size: usize,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_pdf_detach(pdf: *mut zebkit_pdf, index: c_int, error: *mut *mut c_char) -> bool;

    pub fn zebkit_scheme_register(scheme: *const c_char, flags: c_uint) -> bool;
    pub fn zebkit_scheme_handle(scheme: *const c_char, func: Option<zebkit_scheme_func>, data: *mut c_void);

    pub fn zebkit_app_set_menu(
        json: *const c_char,
        func: Option<zebkit_menu_func>,
        data: *mut c_void,
        error: *mut *mut c_char,
    ) -> bool;
    pub fn zebkit_app_on_open_files(func: Option<zebkit_paths_func>, data: *mut c_void);
    pub fn zebkit_app_on_reopen(func: Option<zebkit_app_func>, data: *mut c_void);
    pub fn zebkit_app_on_quit_request(func: Option<zebkit_app_func>, data: *mut c_void);
    pub fn zebkit_app_update_menu_item(id: *const c_char, json: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_app_get_menu() -> *mut c_char;
    pub fn zebkit_app_set_badge(text: *const c_char) -> bool;
    pub fn zebkit_app_set_progress(progress: f64) -> bool;
    pub fn zebkit_app_request_attention(critical: bool);
    pub fn zebkit_app_set_dock_menu(json: *const c_char, func: Option<zebkit_menu_func>, data: *mut c_void, error: *mut *mut c_char) -> bool;

    pub fn zebkit_tray_new(png: *const u8, size: usize, tooltip: *const c_char, error: *mut *mut c_char) -> *mut zebkit_tray;
    pub fn zebkit_tray_set_icon(tray: *mut zebkit_tray, png: *const u8, size: usize, error: *mut *mut c_char) -> bool;
    pub fn zebkit_tray_set_tooltip(tray: *mut zebkit_tray, tooltip: *const c_char);
    pub fn zebkit_tray_set_menu(tray: *mut zebkit_tray, json: *const c_char, func: Option<zebkit_menu_func>, data: *mut c_void, error: *mut *mut c_char) -> bool;
    pub fn zebkit_tray_on_click(tray: *mut zebkit_tray, func: Option<zebkit_app_func>, data: *mut c_void);
    pub fn zebkit_tray_close(tray: *mut zebkit_tray);

    pub fn zebkit_open_external(url: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_show_in_folder(path: *const c_char, error: *mut *mut c_char) -> bool;
    pub fn zebkit_on_power_event(func: Option<zebkit_system_event_func>, data: *mut c_void);
    pub fn zebkit_get_displays() -> *mut c_char;
    pub fn zebkit_on_displays_changed(func: Option<zebkit_system_event_func>, data: *mut c_void);
    pub fn zebkit_prevent_sleep(keep_display_on: bool, reason: *const c_char) -> u64;
    pub fn zebkit_allow_sleep(token: u64) -> bool;
    pub fn zebkit_on_notification(func: Option<zebkit_notification_func>, data: *mut c_void);
    pub fn zebkit_notification_clicked(id: *const c_char, action: c_int) -> bool;
    pub fn zebkit_notification_closed(id: *const c_char, by_user: bool) -> bool;
    pub fn zebkit_request_url(request: *mut zebkit_request) -> *const c_char;
    pub fn zebkit_request_method(request: *mut zebkit_request) -> *const c_char;
    pub fn zebkit_request_headers(request: *mut zebkit_request) -> *const c_char;
    pub fn zebkit_request_body(request: *mut zebkit_request, size: *mut usize) -> *const u8;
    pub fn zebkit_request_respond(
        request: *mut zebkit_request,
        status: c_int,
        headers: *const c_char,
        body: *const c_void,
        size: usize,
    );
    pub fn zebkit_request_respond_file(request: *mut zebkit_request, path: *const c_char, headers: *const c_char);
}

// ---- Off-screen views: <zebkit/offscreen.h> --------------------------------

#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct zebkit_rect {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// `zebkit_frame_kind`.
pub type zebkit_frame_kind = c_int;
pub const ZEBKIT_FRAME_SHARED_MEMORY: zebkit_frame_kind = 1;
pub const ZEBKIT_FRAME_IOSURFACE: zebkit_frame_kind = 2;
pub const ZEBKIT_FRAME_DMABUF: zebkit_frame_kind = 3;
pub const ZEBKIT_FRAME_D3D11: zebkit_frame_kind = 4;

/// `zebkit_frame_layer`.
pub type zebkit_frame_layer = c_int;
pub const ZEBKIT_LAYER_VIEW: zebkit_frame_layer = 0;
pub const ZEBKIT_LAYER_POPUP: zebkit_frame_layer = 1;

/// `zebkit_pixel_format`.
pub type zebkit_pixel_format = c_int;
pub const ZEBKIT_FORMAT_BGRA8: zebkit_pixel_format = 1;
pub const ZEBKIT_FORMAT_RGBA8: zebkit_pixel_format = 2;

#[repr(C)]
pub struct zebkit_frame {
    pub sequence: u64,
    pub layer: zebkit_frame_layer,
    pub bounds: zebkit_rect,
    pub width: i32,
    pub height: i32,
    pub scale: f32,
    pub damage: *const zebkit_rect,
    pub damage_count: usize,
    pub drawn_us: i64,
    pub ready_us: i64,
    pub kind: zebkit_frame_kind,
    pub format: zebkit_pixel_format,
    pub buffer_id: u64,
    pub pixels: *const u8,
    pub stride: usize,
    pub fd: c_int,
    pub size: usize,
    pub offset: usize,
    pub iosurface: *mut c_void,
    pub plane_count: c_int,
    pub fds: [c_int; 4],
    pub strides: [u32; 4],
    pub offsets: [u32; 4],
    pub modifier: u64,
    pub fourcc: u32,
    pub handle: *mut c_void,
}

/// `zebkit_frame_buffers`.
pub type zebkit_frame_buffers = c_int;
pub const ZEBKIT_BUFFERS_AUTO: zebkit_frame_buffers = 0;
pub const ZEBKIT_BUFFERS_SHARED_MEMORY: zebkit_frame_buffers = 1;
pub const ZEBKIT_BUFFERS_GPU: zebkit_frame_buffers = 2;

/// `zebkit_modifiers` bits.
pub const ZEBKIT_MODIFIER_SHIFT: c_uint = 1 << 0;
pub const ZEBKIT_MODIFIER_CONTROL: c_uint = 1 << 1;
pub const ZEBKIT_MODIFIER_ALT: c_uint = 1 << 2;
pub const ZEBKIT_MODIFIER_META: c_uint = 1 << 3;
pub const ZEBKIT_MODIFIER_CAPS_LOCK: c_uint = 1 << 4;
pub const ZEBKIT_MODIFIER_NUM_LOCK: c_uint = 1 << 5;
pub const ZEBKIT_MODIFIER_LEFT_BUTTON: c_uint = 1 << 6;
pub const ZEBKIT_MODIFIER_MIDDLE_BUTTON: c_uint = 1 << 7;
pub const ZEBKIT_MODIFIER_RIGHT_BUTTON: c_uint = 1 << 8;
pub const ZEBKIT_MODIFIER_KEYPAD: c_uint = 1 << 9;
pub const ZEBKIT_MODIFIER_AUTO_REPEAT: c_uint = 1 << 10;

/// `zebkit_mouse_type`.
pub type zebkit_mouse_type = c_int;
pub const ZEBKIT_MOUSE_MOVE: zebkit_mouse_type = 0;
pub const ZEBKIT_MOUSE_DOWN: zebkit_mouse_type = 1;
pub const ZEBKIT_MOUSE_UP: zebkit_mouse_type = 2;
pub const ZEBKIT_MOUSE_LEAVE: zebkit_mouse_type = 3;

/// `zebkit_mouse_button`.
pub type zebkit_mouse_button = c_int;
pub const ZEBKIT_BUTTON_NONE: zebkit_mouse_button = 0;
pub const ZEBKIT_BUTTON_LEFT: zebkit_mouse_button = 1;
pub const ZEBKIT_BUTTON_MIDDLE: zebkit_mouse_button = 2;
pub const ZEBKIT_BUTTON_RIGHT: zebkit_mouse_button = 3;
pub const ZEBKIT_BUTTON_BACK: zebkit_mouse_button = 4;
pub const ZEBKIT_BUTTON_FORWARD: zebkit_mouse_button = 5;

/// `zebkit_wheel_phase`.
pub type zebkit_wheel_phase = c_int;
pub const ZEBKIT_WHEEL_PHASE_NONE: zebkit_wheel_phase = 0;
pub const ZEBKIT_WHEEL_PHASE_BEGAN: zebkit_wheel_phase = 1;
pub const ZEBKIT_WHEEL_PHASE_CHANGED: zebkit_wheel_phase = 2;
pub const ZEBKIT_WHEEL_PHASE_ENDED: zebkit_wheel_phase = 3;
pub const ZEBKIT_WHEEL_PHASE_MOMENTUM: zebkit_wheel_phase = 4;

/// `zebkit_key_type`.
pub type zebkit_key_type = c_int;
pub const ZEBKIT_KEY_DOWN: zebkit_key_type = 0;
pub const ZEBKIT_KEY_UP: zebkit_key_type = 1;
pub const ZEBKIT_KEY_CHAR: zebkit_key_type = 2;

#[repr(C)]
pub struct zebkit_key_event {
    pub type_: zebkit_key_type,
    pub modifiers: c_uint,
    pub key_code: c_int,
    pub native_key_code: c_int,
    pub code: *const c_char,
    pub key: *const c_char,
    pub text: *const c_char,
    pub unmodified_text: *const c_char,
}

#[repr(C)]
pub struct zebkit_edit_command {
    pub name: *const c_char,
    pub value: *const c_char,
}

#[repr(C)]
pub struct zebkit_cursor_image {
    pub pixels: *const u8,
    pub width: i32,
    pub height: i32,
    pub hot_x: i32,
    pub hot_y: i32,
    pub scale: f32,
}

pub type zebkit_frame_func = unsafe extern "C" fn(view: *mut zebkit_view, frame: *const zebkit_frame, data: *mut c_void);
pub type zebkit_caret_func = unsafe extern "C" fn(view: *mut zebkit_view, caret: *const zebkit_rect, data: *mut c_void);
pub type zebkit_cursor_func = unsafe extern "C" fn(
    view: *mut zebkit_view,
    cursor: *const c_char,
    image: *const zebkit_cursor_image,
    data: *mut c_void,
);
pub type zebkit_popup_func =
    unsafe extern "C" fn(view: *mut zebkit_view, shown: bool, bounds: *const zebkit_rect, data: *mut c_void);

extern "C" {
    pub fn zebkit_view_new_offscreen(url: *const c_char, width: c_int, height: c_int, scale: f32) -> *mut zebkit_view;
    pub fn zebkit_view_new_offscreen_in(context: *mut zebkit_context, url: *const c_char, width: c_int, height: c_int, scale: f32) -> *mut zebkit_view;
    pub fn zebkit_view_is_offscreen(view: *mut zebkit_view) -> bool;
    pub fn zebkit_view_set_size(view: *mut zebkit_view, width: c_int, height: c_int, scale: f32);
    pub fn zebkit_view_on_frame(view: *mut zebkit_view, func: Option<zebkit_frame_func>, data: *mut c_void);
    pub fn zebkit_frame_release(frame: *const zebkit_frame);
    pub fn zebkit_view_set_frame_rate(view: *mut zebkit_view, fps: f64);
    pub fn zebkit_view_set_max_frames_in_flight(view: *mut zebkit_view, count: c_int);
    pub fn zebkit_view_set_frame_buffers(view: *mut zebkit_view, buffers: zebkit_frame_buffers);
    pub fn zebkit_view_request_frame(view: *mut zebkit_view);
    pub fn zebkit_frame_iosurface_mach_port(frame: *const zebkit_frame) -> u32;
    pub fn zebkit_view_send_mouse(
        view: *mut zebkit_view,
        type_: zebkit_mouse_type,
        x: f32,
        y: f32,
        button: zebkit_mouse_button,
        click_count: c_int,
        modifiers: c_uint,
    );
    pub fn zebkit_view_send_wheel(
        view: *mut zebkit_view,
        x: f32,
        y: f32,
        delta_x: f32,
        delta_y: f32,
        precise: bool,
        phase: zebkit_wheel_phase,
        modifiers: c_uint,
    );
    pub fn zebkit_view_send_key(view: *mut zebkit_view, event: *const zebkit_key_event);
    pub fn zebkit_view_send_key_with_commands(view: *mut zebkit_view, event: *const zebkit_key_event,
        commands: *const zebkit_edit_command, count: usize) -> bool;
    pub fn zebkit_view_ime_set_composition(
        view: *mut zebkit_view,
        text: *const c_char,
        selection_start: c_int,
        selection_end: c_int,
    );
    pub fn zebkit_view_ime_commit(view: *mut zebkit_view, text: *const c_char);
    pub fn zebkit_view_ime_cancel(view: *mut zebkit_view);
    pub fn zebkit_view_on_caret(view: *mut zebkit_view, func: Option<zebkit_caret_func>, data: *mut c_void);
    pub fn zebkit_view_set_focus(view: *mut zebkit_view, focused: bool);
    pub fn zebkit_view_on_authentication(
        view: *mut zebkit_view, func: Option<zebkit_authentication_func>, data: *mut c_void,
    ) -> bool;
    pub fn zebkit_view_authentication_action(view: *mut zebkit_view, action_json: *const c_char) -> bool;
    pub fn zebkit_view_set_authentication_ui(view: *mut zebkit_view, enabled: bool) -> bool;
    pub fn zebkit_view_on_cursor(view: *mut zebkit_view, func: Option<zebkit_cursor_func>, data: *mut c_void);
    pub fn zebkit_view_on_popup(view: *mut zebkit_view, func: Option<zebkit_popup_func>, data: *mut c_void);
}

// <zebkit/page.h>: dialogs, printing to PDF, drag and drop.

/// `zebkit_dialog_type`.
pub type zebkit_dialog_type = c_int;
pub const ZEBKIT_DIALOG_ALERT: zebkit_dialog_type = 0;
pub const ZEBKIT_DIALOG_CONFIRM: zebkit_dialog_type = 1;
pub const ZEBKIT_DIALOG_PROMPT: zebkit_dialog_type = 2;
pub const ZEBKIT_DIALOG_BEFOREUNLOAD: zebkit_dialog_type = 3;
pub const ZEBKIT_DIALOG_PRINT: zebkit_dialog_type = 4;

/// `zebkit_dialog_policy`.
pub type zebkit_dialog_policy = c_int;
pub const ZEBKIT_DIALOG_POLICY_DEFAULT: zebkit_dialog_policy = 0;
pub const ZEBKIT_DIALOG_POLICY_HOLD: zebkit_dialog_policy = 1;
pub const ZEBKIT_DIALOG_POLICY_ACCEPT: zebkit_dialog_policy = 2;
pub const ZEBKIT_DIALOG_POLICY_DISMISS: zebkit_dialog_policy = 3;

#[repr(C)]
pub struct zebkit_dialog {
    pub id: u64,
    pub type_: zebkit_dialog_type,
    pub message: *const c_char,
    pub default_prompt: *const c_char,
    pub url: *const c_char,
    pub is_reload: bool,
    pub held: bool,
}

pub type zebkit_dialog_func =
    unsafe extern "C" fn(view: *mut zebkit_view, dialog: *const zebkit_dialog, data: *mut c_void);
pub type zebkit_dialog_closed_func =
    unsafe extern "C" fn(view: *mut zebkit_view, id: u64, accepted: bool, user_input: *const c_char, data: *mut c_void);
pub type zebkit_pdf_func =
    unsafe extern "C" fn(view: *mut zebkit_view, pdf: *const u8, size: usize, error: *const c_char, data: *mut c_void);

pub const ZEBKIT_FIND_MATCH_CASE: c_uint = 1 << 0;
pub const ZEBKIT_FIND_BACKWARD: c_uint = 1 << 1;
pub const ZEBKIT_FIND_HIGHLIGHT_ONLY: c_uint = 1 << 2;

pub type zebkit_find_stop_action = c_int;
pub const ZEBKIT_FIND_STOP_CLEAR: zebkit_find_stop_action = 0;
pub const ZEBKIT_FIND_STOP_KEEP: zebkit_find_stop_action = 1;
pub const ZEBKIT_FIND_STOP_ACTIVATE: zebkit_find_stop_action = 2;

pub type zebkit_find_func = unsafe extern "C" fn(view: *mut zebkit_view, json: *const c_char, data: *mut c_void);

pub const ZEBKIT_DRAG_NONE: c_uint = 0;
pub const ZEBKIT_DRAG_COPY: c_uint = 1;
pub const ZEBKIT_DRAG_LINK: c_uint = 2;
pub const ZEBKIT_DRAG_MOVE: c_uint = 16;
pub const ZEBKIT_DRAG_ALL: c_uint = ZEBKIT_DRAG_COPY | ZEBKIT_DRAG_LINK | ZEBKIT_DRAG_MOVE;

#[repr(C)]
pub struct zebkit_drag {
    pub data_json: *const c_char,
    pub operations: c_uint,
    pub x: f32,
    pub y: f32,
    pub image: *const u8,
    pub image_width: c_int,
    pub image_height: c_int,
    pub image_stride: c_int,
    pub image_scale: f32,
    pub image_offset_x: f32,
    pub image_offset_y: f32,
}

pub type zebkit_drag_func =
    unsafe extern "C" fn(view: *mut zebkit_view, drag: *const zebkit_drag, data: *mut c_void) -> bool;
pub type zebkit_drag_end_func = unsafe extern "C" fn(view: *mut zebkit_view, operation: c_uint, data: *mut c_void);
pub type zebkit_drag_operation_func =
    unsafe extern "C" fn(view: *mut zebkit_view, operation: c_uint, data: *mut c_void);

extern "C" {
    pub fn zebkit_view_on_dialog(view: *mut zebkit_view, func: Option<zebkit_dialog_func>, data: *mut c_void);
    pub fn zebkit_view_on_dialog_closed(
        view: *mut zebkit_view,
        func: Option<zebkit_dialog_closed_func>,
        data: *mut c_void,
    );
    pub fn zebkit_view_set_dialog_policy(view: *mut zebkit_view, policy: zebkit_dialog_policy);
    pub fn zebkit_view_get_dialog_policy(view: *mut zebkit_view) -> zebkit_dialog_policy;
    pub fn zebkit_view_dialog_respond(view: *mut zebkit_view, id: u64, accept: bool, prompt_text: *const c_char)
        -> bool;
    pub fn zebkit_view_dialogs(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_view_request_close(view: *mut zebkit_view);
    pub fn zebkit_view_find(view: *mut zebkit_view, text: *const c_char, flags: c_uint) -> c_int;
    pub fn zebkit_view_stop_finding(view: *mut zebkit_view, action: zebkit_find_stop_action);
    pub fn zebkit_view_on_find(view: *mut zebkit_view, func: Option<zebkit_find_func>, data: *mut c_void);
    pub fn zebkit_view_find_result(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_automation_call(
        view: *mut zebkit_view,
        method: *const c_char,
        params: *const c_char,
        func: Option<zebkit_automation_func>,
        data: *mut c_void,
    );
    pub fn zebkit_automation_screenshot_pixels(
        view: *mut zebkit_view,
        method: *const c_char,
        params: *const c_char,
        func: Option<zebkit_automation_pixels_func>,
        data: *mut c_void,
    );
    pub fn zebkit_automation_on_event(view: *mut zebkit_view, func: Option<zebkit_automation_event_func>, data: *mut c_void);
    pub fn zebkit_automation_commands() -> *mut c_char;
    pub fn zebkit_automation_page(page: *const c_char) -> *mut zebkit_view;
    pub fn zebkit_automation_page_id(view: *mut zebkit_view) -> *mut c_char;
    pub fn zebkit_automation_listen(port: c_int, func: Option<zebkit_automation_listen_func>, data: *mut c_void);
    pub fn zebkit_view_print_to_pdf(
        view: *mut zebkit_view,
        options_json: *const c_char,
        func: zebkit_pdf_func,
        data: *mut c_void,
    );
    pub fn zebkit_view_on_drag_start(view: *mut zebkit_view, func: Option<zebkit_drag_func>, data: *mut c_void);
    pub fn zebkit_view_on_drag_end(view: *mut zebkit_view, func: Option<zebkit_drag_end_func>, data: *mut c_void);
    pub fn zebkit_view_is_dragging(view: *mut zebkit_view) -> bool;
    pub fn zebkit_view_drag_enter(
        view: *mut zebkit_view,
        data_json: *const c_char,
        x: f32,
        y: f32,
        operations: c_uint,
        modifiers: c_uint,
    );
    pub fn zebkit_view_drag_over(
        view: *mut zebkit_view,
        x: f32,
        y: f32,
        operations: c_uint,
        modifiers: c_uint,
        func: Option<zebkit_drag_operation_func>,
        data: *mut c_void,
    );
    pub fn zebkit_view_drag_leave(view: *mut zebkit_view);
    pub fn zebkit_view_drop(
        view: *mut zebkit_view,
        x: f32,
        y: f32,
        modifiers: c_uint,
        func: Option<zebkit_drag_operation_func>,
        data: *mut c_void,
    );
    pub fn zebkit_view_drag_source_end(view: *mut zebkit_view, x: f32, y: f32, operation: c_uint);
}

// <zebkit/media_tap.h>: native real-time media data.
#[repr(C)]
pub struct zebkit_media_tap { _private: [u8; 0] }
pub type zebkit_media_tap_func = unsafe extern "C" fn(
    tap: *mut zebkit_media_tap, json: *const c_char,
    bytes: *const u8, size: usize, data: *mut c_void);
extern "C" {
    pub fn zebkit_media_tap_start(view: *mut zebkit_view, media_id: *const c_char,
        options_json: *const c_char, func: Option<zebkit_media_tap_func>,
        data: *mut c_void, error: *mut *mut c_char) -> *mut zebkit_media_tap;
    pub fn zebkit_media_tap_stop(tap: *mut zebkit_media_tap);
    pub fn zebkit_media_tap_free(tap: *mut zebkit_media_tap);
}

// <zebkit/media.h>: the media a page plays, saved as it plays.

#[repr(C)]
pub struct zebkit_media_capture {
    _private: [u8; 0],
}

pub type zebkit_media_capture_func =
    unsafe extern "C" fn(capture: *mut zebkit_media_capture, json: *const c_char, data: *mut c_void);

extern "C" {
    pub fn zebkit_media_keep_history(keep: bool);
    pub fn zebkit_view_media_list(view: *mut zebkit_view, func: Option<zebkit_result_func>, data: *mut c_void);
    pub fn zebkit_view_media_at(
        view: *mut zebkit_view,
        x: f32,
        y: f32,
        func: Option<zebkit_result_func>,
        data: *mut c_void,
    );
    pub fn zebkit_media_capture_start(
        view: *mut zebkit_view,
        media_id: *const c_char,
        path: *const c_char,
        options_json: *const c_char,
        error: *mut *mut c_char,
    ) -> *mut zebkit_media_capture;
    pub fn zebkit_media_capture_on_progress(
        capture: *mut zebkit_media_capture,
        func: Option<zebkit_media_capture_func>,
        data: *mut c_void,
    );
    pub fn zebkit_media_capture_on_done(
        capture: *mut zebkit_media_capture,
        func: Option<zebkit_media_capture_func>,
        data: *mut c_void,
    );
    pub fn zebkit_media_capture_stop(capture: *mut zebkit_media_capture);
    pub fn zebkit_media_capture_status(capture: *mut zebkit_media_capture) -> *mut c_char;
    pub fn zebkit_media_capture_free(capture: *mut zebkit_media_capture);
}

/// The FFmpeg inside the engine. The engine exports each of its functions
/// as `zebkit_vendor_ffmpeg_<name>`; here they have FFmpeg's names.
pub mod ffmpeg {
    use super::*;

    /// The start of `AVCodec`, whose first fields are part of FFmpeg's
    /// public ABI. Only ever used behind a pointer the library returned.
    #[repr(C)]
    pub struct AVCodec {
        pub name: *const c_char,
        pub long_name: *const c_char,
        pub media_type: c_int,
        pub id: c_int,
        _rest: [u8; 0],
    }

    /// `enum AVMediaType`.
    pub const AVMEDIA_TYPE_VIDEO: c_int = 0;
    pub const AVMEDIA_TYPE_AUDIO: c_int = 1;

    extern "C" {
        #[link_name = "zebkit_vendor_ffmpeg_av_version_info"]
        pub fn av_version_info() -> *const c_char;
        #[link_name = "zebkit_vendor_ffmpeg_avcodec_version"]
        pub fn avcodec_version() -> c_uint;
        /// Walks the codecs that are built in; `*opaque` starts as null.
        #[link_name = "zebkit_vendor_ffmpeg_av_codec_iterate"]
        pub fn av_codec_iterate(opaque: *mut *mut c_void) -> *const AVCodec;
        #[link_name = "zebkit_vendor_ffmpeg_av_codec_is_decoder"]
        pub fn av_codec_is_decoder(codec: *const AVCodec) -> c_int;
        #[link_name = "zebkit_vendor_ffmpeg_av_codec_is_encoder"]
        pub fn av_codec_is_encoder(codec: *const AVCodec) -> c_int;
        #[link_name = "zebkit_vendor_ffmpeg_avcodec_find_decoder_by_name"]
        pub fn avcodec_find_decoder_by_name(name: *const c_char) -> *const AVCodec;
    }
}

// <zebkit/media_tools.h>: synchronous, thread-independent local media tools.
pub type zebkit_media_progress_func = unsafe extern "C" fn(i64, i64, *mut c_void) -> c_int;
extern "C" {
    pub fn zebkit_ffprobe(argc: c_int, argv: *const *const c_char,
        json: *mut *mut c_char, error: *mut *mut c_char) -> c_int;
    pub fn zebkit_media_probe(path: *const c_char, json: *mut *mut c_char,
        error: *mut *mut c_char) -> c_int;
    pub fn zebkit_ffmpeg(argc: c_int, argv: *const *const c_char,
        progress: Option<zebkit_media_progress_func>, data: *mut c_void,
        error: *mut *mut c_char) -> c_int;
}


/// Borrowed system window; see zebkit_view_get_native_window.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct zebkit_native_window {
    pub kind: c_int,
    pub handle: usize,
}
pub const ZEBKIT_NATIVE_WINDOW_NONE: c_int = 0;
pub const ZEBKIT_NATIVE_WINDOW_X11: c_int = 1;
pub const ZEBKIT_NATIVE_WINDOW_WIN32: c_int = 2;
pub const ZEBKIT_NATIVE_WINDOW_APPKIT: c_int = 3;
