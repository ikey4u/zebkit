//! Raw bindings to the zebkit engine: `libzebkit.so.1` on Linux,
//! `Zebkit.framework` on macOS. Declarations only, and everything here is
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
opaque!(zebkit_call);
opaque!(zebkit_script);

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
pub type zebkit_title_func = unsafe extern "C" fn(view: *mut zebkit_view, title: *const c_char, data: *mut c_void);
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
    pub fn zebkit_version() -> *const c_char;
    pub fn zebkit_engine_version() -> *const c_char;
    pub fn zebkit_free(pointer: *mut c_void);

    pub fn zebkit_main(argc: c_int, argv: *mut *mut c_char, ready: Option<zebkit_task_func>, data: *mut c_void)
        -> c_int;
    pub fn zebkit_quit();
    pub fn zebkit_post_task(func: zebkit_task_func, data: *mut c_void);
    pub fn zebkit_post_delayed_task(func: zebkit_task_func, data: *mut c_void, delay_ms: u32);
    pub fn zebkit_is_headless() -> bool;

    pub fn zebkit_view_new(url: *const c_char, width: c_int, height: c_int) -> *mut zebkit_view;
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
    );
    pub fn zebkit_view_set_bounds(view: *mut zebkit_view, x: c_int, y: c_int, width: c_int, height: c_int);
    pub fn zebkit_view_set_visible(view: *mut zebkit_view, visible: bool);
    pub fn zebkit_view_add_script(view: *mut zebkit_view, source: *const c_char);
    pub fn zebkit_view_stop(view: *mut zebkit_view);
    pub fn zebkit_view_is_loading(view: *mut zebkit_view) -> bool;
    pub fn zebkit_view_set_user_agent(view: *mut zebkit_view, user_agent: *const c_char);
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

pub const ZEBKIT_SCHEME_STANDARD: c_uint = 1 << 0;
pub const ZEBKIT_SCHEME_SECURE: c_uint = 1 << 1;
pub const ZEBKIT_SCHEME_FETCH: c_uint = 1 << 2;
pub const ZEBKIT_SCHEME_BYPASS_CSP: c_uint = 1 << 3;

pub type zebkit_scheme_func = unsafe extern "C" fn(request: *mut zebkit_request, data: *mut c_void);
pub type zebkit_menu_func = unsafe extern "C" fn(id: *const c_char, data: *mut c_void);
pub type zebkit_paths_func = unsafe extern "C" fn(paths_json: *const c_char, data: *mut c_void);
pub type zebkit_app_func = unsafe extern "C" fn(data: *mut c_void);

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
