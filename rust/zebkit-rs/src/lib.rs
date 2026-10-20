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
