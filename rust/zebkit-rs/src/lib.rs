//! Raw bindings to the zebkit engine: `libzebkit.so.1` on Linux,
//! `Zebkit.framework` on macOS. Declarations only, and everything here is
//! `unsafe`; the `zebkit` crate is the safe interface over them.
//!
//! - The engine, views, the page's calls to the application, script
//!   outside any page and CDP: `<zebkit/zebkit.h>`.
//! - What the engine has for applications besides: compression, digests and
//!   encryption, URLs, images and HTTP (`<zebkit/services.h>`).
//! - [`ffmpeg`]: the FFmpeg inside the engine (`<zebkit/vendor/ffmpeg.h>`),
//!   the part of it the `zebkit` crate uses.
//!
//! Names, types and rules are the C headers'; `docs/blink/API.md` in the
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
    pub fn zebkit_pdf_text(pdf: *mut zebkit_pdf, page: c_int, error: *mut *mut c_char) -> *mut c_char;
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
