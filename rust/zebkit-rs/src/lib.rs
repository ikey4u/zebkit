//! Raw bindings to zebkit, the engine library (`libzebkit.so.1` on Linux,
//! `Zebkit.framework` on macOS).
//!
//! Declarations and nothing else: every function here is `unsafe` and has
//! the meaning and the rules it has in C. The `zebkit` crate is the safe
//! interface built on these; use that unless something is missing from it.
//!
//! What is declared is what `zebkit` needs so far:
//!
//! - On Linux, [`glib`]: the main loop, objects and signals of GLib and
//!   GObject, which the engine's API is made of; and [`webkit`] and
//!   [`wpe`]: web views, and the windows they are shown in.
//! - On macOS, [`cocoa`]: the same things, where the engine's API is
//!   Objective-C (`WKWebView`, `NSWindow`, the application's loop), through
//!   C functions this crate has for them.
//! - [`cdp`]: the Chrome DevTools Protocol endpoint (`zebkit-cdp.h`).
//! - [`ffmpeg`]: the FFmpeg inside the engine (`zebkit/vendor/ffmpeg.h`).
//!
//! The engine's API is much larger than this (it is WPE WebKit's on Linux
//! and WebKit's on macOS). On Linux a function that is missing can be
//! declared in the same way; on macOS [`cocoa::zebkit_shim_view_get_web_view`]
//! gives the `WKWebView` for whatever speaks Objective-C.

#![allow(non_camel_case_types, non_snake_case)]

pub use std::os::raw::{c_char, c_int, c_uint, c_ulong, c_void};

#[cfg(not(target_os = "macos"))]
pub type gboolean = c_int;
#[cfg(not(target_os = "macos"))]
pub type gpointer = *mut c_void;
#[cfg(not(target_os = "macos"))]
pub type GType = usize;

/// GLib and GObject: main loop, objects, signals.
#[cfg(not(target_os = "macos"))]
pub mod glib {
    use super::*;

    #[repr(C)]
    pub struct GMainLoop {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct GMainContext {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct GObject {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct GClosure {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct GParamSpec {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct GError {
        pub domain: u32,
        pub code: c_int,
        pub message: *mut c_char,
    }

    /// A signal handler, cast from its real type as `G_CALLBACK` does in C.
    pub type GCallback = unsafe extern "C" fn();
    pub type GClosureNotify = unsafe extern "C" fn(data: gpointer, closure: *mut GClosure);
    pub type GSourceFunc = unsafe extern "C" fn(data: gpointer) -> gboolean;
    pub type GDestroyNotify = unsafe extern "C" fn(data: gpointer);

    pub const G_PRIORITY_DEFAULT: c_int = 0;

    extern "C" {
        pub fn g_main_loop_new(context: *mut GMainContext, is_running: gboolean) -> *mut GMainLoop;
        pub fn g_main_loop_ref(main_loop: *mut GMainLoop) -> *mut GMainLoop;
        pub fn g_main_loop_unref(main_loop: *mut GMainLoop);
        pub fn g_main_loop_run(main_loop: *mut GMainLoop);
        pub fn g_main_loop_quit(main_loop: *mut GMainLoop);
        pub fn g_timeout_add_full(
            priority: c_int,
            interval: c_uint,
            function: GSourceFunc,
            data: gpointer,
            notify: Option<GDestroyNotify>,
        ) -> c_uint;

        pub fn g_object_new(object_type: GType, first_property_name: *const c_char, ...) -> *mut GObject;
        pub fn g_object_ref(object: gpointer) -> gpointer;
        pub fn g_object_unref(object: gpointer);
        pub fn g_signal_connect_data(
            instance: gpointer,
            detailed_signal: *const c_char,
            handler: GCallback,
            data: gpointer,
            destroy_data: Option<GClosureNotify>,
            connect_flags: c_int,
        ) -> c_ulong;

        pub fn g_error_free(error: *mut GError);
        pub fn g_free(memory: gpointer);
    }
}

/// The windowing side of the engine: displays, the surface a view draws
/// on, and the window that holds it.
#[cfg(not(target_os = "macos"))]
pub mod wpe {
    use super::*;

    #[repr(C)]
    pub struct WPEDisplay {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct WPEView {
        _private: [u8; 0],
    }
    #[repr(C)]
    pub struct WPEToplevel {
        _private: [u8; 0],
    }

    extern "C" {
        /// A display with no window system behind it. Returns a new reference.
        pub fn wpe_display_headless_new() -> *mut WPEDisplay;
        pub fn wpe_view_get_toplevel(view: *mut WPEView) -> *mut WPEToplevel;
        pub fn wpe_toplevel_resize(toplevel: *mut WPEToplevel, width: c_int, height: c_int) -> gboolean;
        pub fn wpe_toplevel_set_title(toplevel: *mut WPEToplevel, title: *const c_char);
    }
}

/// Web views.
#[cfg(not(target_os = "macos"))]
pub mod webkit {
    use super::*;

    #[repr(C)]
    pub struct WebKitWebView {
        _private: [u8; 0],
    }

    /// `WebKitLoadEvent`.
    pub const WEBKIT_LOAD_STARTED: c_int = 0;
    pub const WEBKIT_LOAD_REDIRECTED: c_int = 1;
    pub const WEBKIT_LOAD_COMMITTED: c_int = 2;
    pub const WEBKIT_LOAD_FINISHED: c_int = 3;

    extern "C" {
        pub fn webkit_web_view_get_type() -> GType;
        pub fn webkit_web_view_load_uri(web_view: *mut WebKitWebView, uri: *const c_char);
        pub fn webkit_web_view_load_html(web_view: *mut WebKitWebView, content: *const c_char, base_uri: *const c_char);
        pub fn webkit_web_view_get_uri(web_view: *mut WebKitWebView) -> *const c_char;
        pub fn webkit_web_view_get_title(web_view: *mut WebKitWebView) -> *const c_char;
        pub fn webkit_web_view_reload(web_view: *mut WebKitWebView);
        pub fn webkit_web_view_stop_loading(web_view: *mut WebKitWebView);
        pub fn webkit_web_view_is_loading(web_view: *mut WebKitWebView) -> gboolean;
        pub fn webkit_web_view_can_go_back(web_view: *mut WebKitWebView) -> gboolean;
        pub fn webkit_web_view_go_back(web_view: *mut WebKitWebView);
        pub fn webkit_web_view_can_go_forward(web_view: *mut WebKitWebView) -> gboolean;
        pub fn webkit_web_view_go_forward(web_view: *mut WebKitWebView);
        pub fn webkit_web_view_get_wpe_view(web_view: *mut WebKitWebView) -> *mut wpe::WPEView;
    }
}

/// Web views, their windows and the application's loop on macOS, as the C
/// functions of `src/cocoa/shim.m`, which this crate compiles. All of it is
/// for the main thread.
#[cfg(target_os = "macos")]
pub mod cocoa {
    use super::*;

    /// A view with its window. Reference counted: [`zebkit_shim_view_new`]
    /// and [`zebkit_shim_view_retain`] each return a reference that
    /// [`zebkit_shim_view_release`] gives back. The page and the window go
    /// with the last one.
    #[repr(C)]
    pub struct ZebkitShimView {
        _private: [u8; 0],
    }

    /// What [`zebkit_shim_view_add_handler`] can be told about.
    pub const ZEBKIT_SHIM_TITLE: c_int = 0;
    pub const ZEBKIT_SHIM_URI: c_int = 1;
    pub const ZEBKIT_SHIM_LOAD: c_int = 2;
    pub const ZEBKIT_SHIM_CLOSE: c_int = 3;

    /// The `event` of a [`ZEBKIT_SHIM_LOAD`] handler.
    pub const ZEBKIT_SHIM_LOAD_STARTED: c_int = 0;
    pub const ZEBKIT_SHIM_LOAD_REDIRECTED: c_int = 1;
    pub const ZEBKIT_SHIM_LOAD_COMMITTED: c_int = 2;
    pub const ZEBKIT_SHIM_LOAD_FINISHED: c_int = 3;

    /// `text` is the new title or address, valid during the call, and null
    /// for the other kinds.
    pub type ZebkitShimHandler = unsafe extern "C" fn(event: c_int, text: *const c_char, data: *mut c_void);
    pub type ZebkitShimFunction = unsafe extern "C" fn(data: *mut c_void);

    extern "C" {
        /// A page in a window of 1024 by 768, which the engine makes: on
        /// the screen when the machine has a display, and never on any
        /// when it has none or `headless` is set.
        pub fn zebkit_shim_view_new(headless: c_int) -> *mut ZebkitShimView;
        pub fn zebkit_shim_view_retain(view: *mut ZebkitShimView) -> *mut ZebkitShimView;
        pub fn zebkit_shim_view_release(view: *mut ZebkitShimView);
        /// The `WKWebView`, which the view owns.
        pub fn zebkit_shim_view_get_web_view(view: *mut ZebkitShimView) -> *mut c_void;

        pub fn zebkit_shim_view_load_uri(view: *mut ZebkitShimView, uri: *const c_char);
        pub fn zebkit_shim_view_load_html(view: *mut ZebkitShimView, html: *const c_char, base_uri: *const c_char);
        /// Null when there is none. Free with [`free`].
        pub fn zebkit_shim_view_copy_uri(view: *mut ZebkitShimView) -> *mut c_char;
        /// Null when there is none. Free with [`free`].
        pub fn zebkit_shim_view_copy_title(view: *mut ZebkitShimView) -> *mut c_char;
        pub fn zebkit_shim_view_is_loading(view: *mut ZebkitShimView) -> c_int;
        pub fn zebkit_shim_view_reload(view: *mut ZebkitShimView);
        pub fn zebkit_shim_view_stop_loading(view: *mut ZebkitShimView);
        pub fn zebkit_shim_view_can_go_back(view: *mut ZebkitShimView) -> c_int;
        pub fn zebkit_shim_view_go_back(view: *mut ZebkitShimView);
        pub fn zebkit_shim_view_can_go_forward(view: *mut ZebkitShimView) -> c_int;
        pub fn zebkit_shim_view_go_forward(view: *mut ZebkitShimView);

        /// Calls `handler` with `data` for every change of the kind, for
        /// as long as the view lives, and `destroy` with `data` after that.
        pub fn zebkit_shim_view_add_handler(
            view: *mut ZebkitShimView,
            kind: c_int,
            handler: ZebkitShimHandler,
            data: *mut c_void,
            destroy: Option<ZebkitShimFunction>,
        );

        pub fn zebkit_shim_window_resize(view: *mut ZebkitShimView, width: c_int, height: c_int);
        pub fn zebkit_shim_window_set_title(view: *mut ZebkitShimView, title: *const c_char);

        /// Runs the application's loop until [`zebkit_shim_quit`].
        pub fn zebkit_shim_run();
        /// Does nothing when the loop is not running.
        pub fn zebkit_shim_quit();
        /// Calls `function` once from the loop, then `destroy`.
        pub fn zebkit_shim_timeout(
            milliseconds: c_uint,
            function: ZebkitShimFunction,
            data: *mut c_void,
            destroy: Option<ZebkitShimFunction>,
        );

        /// The C library's, for the strings handed over here and by [`cdp`].
        pub fn free(memory: *mut c_void);
    }
}

/// The Chrome DevTools Protocol endpoint (`zebkit-cdp.h`, `docs/CDP.md`).
pub mod cdp {
    use super::*;

    #[cfg(not(target_os = "macos"))]
    extern "C" {
        /// Returns the port, or 0 with `error` set.
        pub fn zebkit_cdp_listen(address: *const c_char, port: c_uint, error: *mut *mut glib::GError) -> c_uint;
        pub fn zebkit_cdp_set_discovery_enabled(enabled: gboolean);
        /// Null when the endpoint is not running. Free with `g_free`.
        pub fn zebkit_cdp_get_url() -> *mut c_char;
    }

    #[cfg(target_os = "macos")]
    extern "C" {
        /// Returns the port, or 0 with `error` set to a message to free
        /// with [`cocoa::free`].
        pub fn zebkit_cdp_listen(address: *const c_char, port: c_uint, error: *mut *mut c_char) -> c_uint;
        pub fn zebkit_cdp_set_discovery_enabled(enabled: bool);
        /// Null when the endpoint is not running. Free with [`cocoa::free`].
        pub fn zebkit_cdp_get_url() -> *mut c_char;
    }

    extern "C" {
        pub fn zebkit_cdp_get_port() -> c_uint;
        pub fn zebkit_cdp_stop();
    }
}

/// The FFmpeg inside the engine (`docs/VENDOR.md`). The engine exports each
/// function as `zebkit_vendor_ffmpeg_<name>`; here they have FFmpeg's names.
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
