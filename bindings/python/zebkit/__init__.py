"""zebkit for Python: the engine, and what is inside it, through ctypes.

    import zebkit

    def ready():
        view = zebkit.View("https://example.com/")
        view.on_load(lambda event, url: event == zebkit.LOAD_FINISHED and
                     view.evaluate("document.title", lambda title, error: print(title)))

    zebkit.main(ready)

The services work without the engine running:

    packed = zebkit.compress(b"..." * 100, "zstd")
    zebkit.digest(b"abc", "sha256").hex()
    zebkit.url_part("../b", "https://example.com/a/", "spec")

The library is libzebkit.so.1 (Zebkit.framework on macOS, zebkit.dll on Windows) in $ZEBKIT_DIR,
or inside this package's directory, or in the zebkit repository's package
of the slim profile (slim-macos on macOS).

Threads: everything is for the thread that called main() (or init()), and
callbacks run on it; post() runs a function there from any thread. Views,
contexts, scripts and the application's functions raise Error on another
thread once the engine has started.
"""

import ctypes
import json
import os
import pathlib
import sys
import threading

__all__ = [
    "View", "PermissionRequest", "NavigationRequest", "CertificateRequest", "HttpAuthRequest", "DownloadRequest", "ClientCertificateRequest", "FileChooserRequest", "MediaAccessRequest", "NetworkRequest", "NetworkStream", "main", "init", "pump", "shutdown", "set_wakeup", "quit", "post", "version", "engine_version", "is_headless", "set_engine_settings", "engine_settings",
    "set_cookie_storage", "set_cookie_encryption_key", "cdp_listen", "cdp_port", "cdp_url", "cdp_stop", "fetch",
    "compress", "decompress", "digest", "hmac", "random_bytes", "aes_gcm_seal", "aes_gcm_open",
    "url_part", "registrable_domain", "image_decode", "image_encode", "icu_init", "Error",
    "LOAD_STARTED", "LOAD_COMMITTED", "LOAD_FINISHED", "LOAD_FAILED",
    "ALLOW_APP", "ALLOW_FETCH", "ALLOW_FILES", "ALLOW_COMPRESS", "ALLOW_CRYPTO", "ALLOW_IMAGE",
    "ALLOW_URL", "ALLOW_PDF", "ALLOW_ALL",
    "Canvas", "Font", "shape_text", "PdfHooks", "pdf_hooks", "remove_pdf_hook", "pdf_filter_input",
    "Pdf", "RENDER_ANNOTATIONS", "RENDER_FORMS", "RENDER_LCD_TEXT", "RENDER_GRAYSCALE", "RENDER_PRINTING",
    "RENDER_NO_SMOOTHING", "Request", "scheme_register", "scheme_handle",
    "SCHEME_STANDARD", "SCHEME_SECURE", "SCHEME_FETCH", "SCHEME_BYPASS_CSP",
    "app_set_menu", "app_update_menu_item", "app_menu", "app_set_badge", "app_set_progress",
    "app_request_attention", "app_set_dock_menu", "app_on_open_files", "app_on_reopen", "app_on_quit_request",
    "Tray", "DeviceChooserRequest", "EyeDropperRequest", "ShareRequest", "UNDEFINED",
    "environment", "on_environment_changed", "capabilities", "displays", "on_displays_changed",
    "on_notification", "notification_clicked", "notification_closed", "on_power_event",
    "prevent_sleep", "allow_sleep", "open_external", "show_in_folder",
]

LOAD_STARTED, LOAD_COMMITTED, LOAD_FINISHED, LOAD_FAILED = 0, 1, 2, 3
ALLOW_APP, ALLOW_FETCH, ALLOW_FILES, ALLOW_COMPRESS = 1, 2, 4, 8
ALLOW_CRYPTO, ALLOW_IMAGE, ALLOW_URL, ALLOW_PDF, ALLOW_ALL = 16, 32, 64, 128, 0xFF

_COMPRESSION = {"gzip": 1, "zlib": 2, "deflate": 2, "deflate-raw": 3, "brotli": 4, "br": 4, "zstd": 5}
_DIGEST = {"sha1": 1, "sha256": 2, "sha384": 3, "sha512": 4}
_IMAGE = {"png": 1, "jpeg": 2, "webp": 3}
_URL_PART = {"spec": 0, "scheme": 1, "username": 2, "password": 3, "host": 4, "port": 5, "path": 6,
             "query": 7, "fragment": 8, "origin": 9}


class Error(Exception):
    """What the engine reports when something it was asked to do fails."""


class _Undefined:
    def __repr__(self):
        return "zebkit.UNDEFINED"


# What a function bound with View.bind() returns to resolve the page's
# promise with undefined rather than a JSON value.
UNDEFINED = _Undefined()


def _find_library():
    if sys.platform == "darwin":
        library, profile = "Zebkit.framework/Zebkit", "slim-macos"
    elif sys.platform == "win32":
        # python.exe cannot be the sandbox's child (docs/WINDOWS.md): --no-sandbox.
        library, profile = "zebkit.dll", "slim-windows"
    else:
        library, profile = "libzebkit.so.1", "slim"
    names = []
    if os.environ.get("ZEBKIT_DIR"):
        names.append(pathlib.Path(os.environ["ZEBKIT_DIR"]) / library)
    here = pathlib.Path(__file__).resolve().parent
    names.append(here / library)
    if sys.platform == "darwin":
        names.append(here.parents[2] / "dist" / "release" / library)
        names.append(here.parents[2] / "out" / "blink" / "slim-macos-native" / "package" / library)
    names.append(here.parents[2] / "out" / "blink" / profile / "package" / library)
    for name in names:
        if name.exists():
            return str(name)
    raise ImportError(f"{library} not found; set ZEBKIT_DIR to the engine's directory")


_lib = ctypes.CDLL(_find_library())

_TASK = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_LOAD = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)
_TITLE = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)
_VIEW = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)
_RESULT = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p)
_BIND = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)


class _Response(ctypes.Structure):
    _fields_ = [("status", ctypes.c_int), ("url", ctypes.c_char_p), ("headers", ctypes.c_char_p),
                ("body", ctypes.POINTER(ctypes.c_uint8)), ("body_size", ctypes.c_size_t),
                ("error", ctypes.c_char_p)]


_RESPONSE = ctypes.CFUNCTYPE(None, ctypes.POINTER(_Response), ctypes.c_void_p)


def _declare(name, result, *args):
    function = getattr(_lib, name)
    function.restype = result
    function.argtypes = list(args)
    return function


_V, _P, _S, _I, _U, _Z, _B = ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint, \
    ctypes.c_size_t, ctypes.c_bool
_ZP = ctypes.POINTER(ctypes.c_size_t)
_IP = ctypes.POINTER(ctypes.c_int)
_EP = ctypes.POINTER(ctypes.c_void_p)

_version = _declare("zebkit_version", _S)
_engine_version = _declare("zebkit_engine_version", _S)
_free = _declare("zebkit_free", None, _P)
_main = _declare("zebkit_main", _I, _I, ctypes.POINTER(ctypes.c_char_p), _TASK, _P)
_init = _declare("zebkit_init", _I, _I, ctypes.POINTER(ctypes.c_char_p))
_init_settings = _declare("zebkit_init_with_settings", _I, _I, ctypes.POINTER(ctypes.c_char_p), _S, _EP)
_WAKEUP = ctypes.CFUNCTYPE(None, ctypes.c_int64, _P)
_set_wakeup = _declare("zebkit_set_wakeup", _B, _WAKEUP, _P)
_wakeup_callback = None
_pump = _declare("zebkit_pump", _B)
_shutdown = _declare("zebkit_shutdown", _B)
_quit = _declare("zebkit_quit", None)
_post = _declare("zebkit_post_task", _B, _TASK, _P)
_post_delayed = _declare("zebkit_post_delayed_task", _B, _TASK, _P, ctypes.c_uint32)
_is_headless = _declare("zebkit_is_headless", _B)
_view_new = _declare("zebkit_view_new", _V, _S, _I, _I)
_view_load_url = _declare("zebkit_view_load_url", None, _V, _S)
_view_user_agent_override = _declare("zebkit_view_set_user_agent_override", _B, _V, _S, _RESULT, _P)
_cookie_storage = _declare("zebkit_set_cookie_storage", _B, _I, _S)
_cookie_key = _declare("zebkit_set_cookie_encryption_key", _B, _P, ctypes.c_size_t)
_view_target = _declare("zebkit_view_get_target_id", _P, _V)
_view_reload = _declare("zebkit_view_reload", None, _V)
_view_go_back = _declare("zebkit_view_go_back", None, _V)
_view_go_forward = _declare("zebkit_view_go_forward", None, _V)
_view_resize = _declare("zebkit_view_resize", None, _V, _I, _I)
_view_focus = _declare("zebkit_view_focus", None, _V)
_AUTHENTICATION = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_authentication = _declare("zebkit_view_on_authentication", _B, _V, _AUTHENTICATION, _P)
_view_authentication_action = _declare("zebkit_view_authentication_action", _B, _V, _S)
_view_authentication_ui = _declare("zebkit_view_set_authentication_ui", _B, _V, _B)
_view_close = _declare("zebkit_view_close", None, _V)
_view_get_url = _declare("zebkit_view_get_url", _P, _V)
_view_get_title = _declare("zebkit_view_get_title", _P, _V)
_view_on_load = _declare("zebkit_view_on_load", None, _V, _LOAD, _P)
_view_on_title = _declare("zebkit_view_on_title", None, _V, _TITLE, _P)
_view_on_close = _declare("zebkit_view_on_close", None, _V, _VIEW, _P)
_POPUP_BLOCKED = ctypes.CFUNCTYPE(None, _V, _S, _P)
_WINDOW_OPEN = ctypes.CFUNCTYPE(None, _V, _V, _S, _P)
_WINDOW_OPEN_EVENT = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_window_open = _declare("zebkit_view_on_window_open", None, _V, _WINDOW_OPEN, _P)
_view_on_window_open_event = _declare("zebkit_view_on_window_open_event", None, _V, _WINDOW_OPEN_EVENT, _P)
_view_window_open_info = _declare("zebkit_view_get_window_open_info", _P, _V)
_CONSOLE = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_console = _declare("zebkit_view_on_console", None, _V, _CONSOLE, _P)

# <zebkit/window.h>
_WINDOW_STATE = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_new_with_options = _declare("zebkit_view_new_with_options", _V, _P, _S, _S, ctypes.POINTER(_P))
_view_get_window = _declare("zebkit_view_get_window", _P, _V)
_view_set_window = _declare("zebkit_view_set_window", _B, _V, _S, ctypes.POINTER(_P))
_view_window_action = _declare("zebkit_view_window_action", _B, _V, _S, ctypes.POINTER(_P))
_view_on_window_state = _declare("zebkit_view_on_window_state", None, _V, _WINDOW_STATE, _P)
_view_set_drag_regions = _declare("zebkit_view_set_drag_regions", _B, _V, _S, ctypes.POINTER(_P))
_view_set_popup_window_options = _declare("zebkit_view_set_popup_window_options", _B, _V, _S, ctypes.POINTER(_P))
_get_capabilities = _declare("zebkit_get_capabilities", _P)
_ENVIRONMENT = ctypes.CFUNCTYPE(None, _S, _P)
_JSON_EVENT = ctypes.CFUNCTYPE(None, _S, _P)
_on_notification = _declare("zebkit_on_notification", None, _JSON_EVENT, _P)
_notification_clicked = _declare("zebkit_notification_clicked", _B, _S, ctypes.c_int)
_notification_closed = _declare("zebkit_notification_closed", _B, _S, _B)
_on_power_event = _declare("zebkit_on_power_event", None, _JSON_EVENT, _P)
_get_displays = _declare("zebkit_get_displays", _P)
_on_displays_changed = _declare("zebkit_on_displays_changed", None, _JSON_EVENT, _P)
_prevent_sleep = _declare("zebkit_prevent_sleep", ctypes.c_uint64, _B, _S)
_allow_sleep = _declare("zebkit_allow_sleep", _B, ctypes.c_uint64)
_open_external = _declare("zebkit_open_external", _B, _S, ctypes.POINTER(_P))
_show_in_folder = _declare("zebkit_show_in_folder", _B, _S, ctypes.POINTER(_P))
_TRAY_CLICK = ctypes.CFUNCTYPE(None, _P)
_TRAY_MENU = ctypes.CFUNCTYPE(None, _S, _P)
_tray_new = _declare("zebkit_tray_new", _P, ctypes.c_char_p, ctypes.c_size_t, _S, ctypes.POINTER(_P))
_tray_set_icon = _declare("zebkit_tray_set_icon", _B, _P, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(_P))
_tray_set_tooltip = _declare("zebkit_tray_set_tooltip", None, _P, _S)
_tray_set_menu = _declare("zebkit_tray_set_menu", _B, _P, _S, _TRAY_MENU, _P, ctypes.POINTER(_P))
_tray_on_click = _declare("zebkit_tray_on_click", None, _P, _TRAY_CLICK, _P)
_tray_close = _declare("zebkit_tray_close", None, _P)
_system_callbacks = {}
_get_environment = _declare("zebkit_get_environment", _P)
_on_environment_changed = _declare("zebkit_on_environment_changed", None, _ENVIRONMENT, _P)
_environment_callback = None

_view_set_settings = _declare("zebkit_view_set_settings", _B, _V, _S, ctypes.POINTER(_P))
_view_get_settings = _declare("zebkit_view_get_settings", _P, _V)
_set_engine_settings = _declare("zebkit_set_engine_settings", _B, _S, ctypes.POINTER(_P))
_get_engine_settings = _declare("zebkit_get_engine_settings", _P)
_view_set_runtime_settings = _declare("zebkit_view_set_runtime_settings", _B, _V, _S, ctypes.POINTER(_P))
_view_get_runtime_settings = _declare("zebkit_view_get_runtime_settings", _P, _V)
_PERMISSION = ctypes.CFUNCTYPE(None, _V, _P, _S, _P)
_PERMISSION_RESULT = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_set_permission = _declare("zebkit_view_set_permission", _B, _V, _S, _S, _I, _EP)
_view_get_permission = _declare("zebkit_view_get_permission", _I, _V, _S, _S)
_view_reset_permissions = _declare("zebkit_view_reset_permissions", _B, _V, _S, _EP)
_view_permission_timeout = _declare("zebkit_view_set_permission_timeout", _B, _V, ctypes.c_uint32)
_view_on_permission = _declare("zebkit_view_on_permission", None, _V, _PERMISSION, _P)
_view_on_permission_result = _declare("zebkit_view_on_permission_result", None, _V, _PERMISSION_RESULT, _P)
_permission_info = _declare("zebkit_permission_request_info", _P, _P)
_permission_respond = _declare("zebkit_permission_respond", _B, _P, _I, _B)
_PERMISSION_STATES = {"prompt": 0, "granted": 1, "denied": 2}
_NAVIGATION = ctypes.CFUNCTYPE(None, _V, _P, _S, _P)
_NAVIGATION_RESULT = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_navigation = _declare("zebkit_view_on_navigation", None, _V, _NAVIGATION, _P)
_view_on_navigation_result = _declare("zebkit_view_on_navigation_result", None, _V, _NAVIGATION_RESULT, _P)
_view_navigation_timeout = _declare("zebkit_view_set_navigation_timeout", _B, _V, ctypes.c_uint32)
_view_navigation_fail_closed = _declare("zebkit_view_set_navigation_fail_closed", _B, _V, ctypes.c_bool)
_navigation_info = _declare("zebkit_navigation_request_info", _P, _P)
_navigation_respond = _declare("zebkit_navigation_respond", _B, _P, _I, _S, _EP)


_CERTIFICATE = ctypes.CFUNCTYPE(None, _V, _P, _S, _P)
_CERTIFICATE_RESULT = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_certificate = _declare("zebkit_view_on_certificate", None, _V, _CERTIFICATE, _P)
_view_on_certificate_result = _declare("zebkit_view_on_certificate_result", None, _V, _CERTIFICATE_RESULT, _P)
_view_certificate_timeout = _declare("zebkit_view_set_certificate_timeout", _B, _V, ctypes.c_uint32)
_certificate_info = _declare("zebkit_certificate_request_info", _P, _P)
_certificate_respond = _declare("zebkit_certificate_respond", _B, _P, _B)

_HTTP_AUTH = ctypes.CFUNCTYPE(None, _V, _P, _S, _P)
_HTTP_AUTH_RESULT = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_http_auth = _declare("zebkit_view_on_http_auth", None, _V, _HTTP_AUTH, _P)
_view_on_http_auth_result = _declare("zebkit_view_on_http_auth_result", None, _V, _HTTP_AUTH_RESULT, _P)
_view_http_auth_timeout = _declare("zebkit_view_set_http_auth_timeout", _B, _V, ctypes.c_uint32)
_http_auth_info = _declare("zebkit_http_auth_request_info", _P, _P)
_http_auth_respond = _declare("zebkit_http_auth_respond", _B, _P, _S, _S)


_ASKED = ctypes.CFUNCTYPE(None, _V, _P, _S, _P)
_TOLD = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_on_external_protocol = _declare("zebkit_view_on_external_protocol", None, _V, _TOLD, _P)
_view_on_download = _declare("zebkit_view_on_download", None, _V, _ASKED, _P)
_view_on_download_progress = _declare("zebkit_view_on_download_progress", None, _V, _TOLD, _P)
_view_download_timeout = _declare("zebkit_view_set_download_timeout", _B, _V, ctypes.c_uint32)
_download_info = _declare("zebkit_download_request_info", _P, _P)
_download_respond = _declare("zebkit_download_respond", _B, _P, _I, _S, _EP)
_view_cancel_download = _declare("zebkit_view_cancel_download", _B, _V, _S)
_view_on_client_certificate = _declare("zebkit_view_on_client_certificate", None, _V, _ASKED, _P)
_view_on_client_certificate_result = _declare("zebkit_view_on_client_certificate_result", None, _V, _TOLD, _P)
_view_client_certificate_timeout = _declare("zebkit_view_set_client_certificate_timeout", _B, _V, ctypes.c_uint32)
_client_certificate_info = _declare("zebkit_client_certificate_request_info", _P, _P)
_client_certificate_respond = _declare("zebkit_client_certificate_respond", _B, _P, _I, _S, _S, _EP)
_client_certificate_choose = _declare("zebkit_client_certificate_choose", _B, _P, ctypes.c_size_t, _EP)
_view_attach_native = _declare("zebkit_view_attach_native", _B, _V, ctypes.c_size_t, _I, _I, _I, _I, _EP)
_view_detach_native = _declare("zebkit_view_detach_native", _B, _V)


class _NativeWindowInfo(ctypes.Structure):
    _fields_ = [("kind", _I), ("handle", ctypes.c_size_t)]


_view_get_native_window = _declare("zebkit_view_get_native_window", _B, _V, ctypes.POINTER(_NativeWindowInfo))
_NATIVE_WINDOW_EXPORT = ctypes.CFUNCTYPE(None, _P, _S)
_view_export_native_window = _declare("zebkit_view_export_native_window", _B, _V, _NATIVE_WINDOW_EXPORT, _P)
_view_set_bounds = _declare("zebkit_view_set_bounds", None, _V, _I, _I, _I, _I)
_view_add_world_script = _declare("zebkit_view_add_world_script", _B, _V, _S, _S, _B, _EP)
_view_on_file_chooser = _declare("zebkit_view_on_file_chooser", None, _V, _ASKED, _P)
_view_on_file_chooser_result = _declare("zebkit_view_on_file_chooser_result", None, _V, _TOLD, _P)
_view_file_chooser_timeout = _declare("zebkit_view_set_file_chooser_timeout", _B, _V, ctypes.c_uint32)
_file_chooser_info = _declare("zebkit_file_chooser_request_info", _P, _P)
_view_on_eye_dropper = _declare("zebkit_view_on_eye_dropper", None, _V, _ASKED, _P)
_view_on_eye_dropper_result = _declare("zebkit_view_on_eye_dropper_result", None, _V, _TOLD, _P)
_view_eye_dropper_timeout = _declare("zebkit_view_set_eye_dropper_timeout", _B, _V, ctypes.c_uint32)
_eye_dropper_info = _declare("zebkit_eye_dropper_request_info", _P, _P)
_eye_dropper_respond = _declare("zebkit_eye_dropper_respond", _B, _P, _S)
_view_on_device_chooser = _declare("zebkit_view_on_device_chooser", None, _V, _ASKED, _P)
_view_on_device_chooser_result = _declare("zebkit_view_on_device_chooser_result", None, _V, _TOLD, _P)
_view_device_chooser_timeout = _declare("zebkit_view_set_device_chooser_timeout", _B, _V, ctypes.c_uint32)
_device_chooser_info = _declare("zebkit_device_chooser_request_info", _P, _P)
_device_chooser_respond = _declare("zebkit_device_chooser_respond", _B, _P, _S)
_view_on_share = _declare("zebkit_view_on_share", None, _V, _ASKED, _P)
_view_on_share_result = _declare("zebkit_view_on_share_result", None, _V, _TOLD, _P)
_view_share_timeout = _declare("zebkit_view_set_share_timeout", _B, _V, ctypes.c_uint32)
_share_info = _declare("zebkit_share_request_info", _P, _P)
_share_respond = _declare("zebkit_share_respond", _B, _P, _B)
_file_chooser_respond = _declare("zebkit_file_chooser_respond", _B, _P, ctypes.POINTER(ctypes.c_char_p), _Z, _EP)
_view_on_media_access = _declare("zebkit_view_on_media_access", None, _V, _ASKED, _P)
_view_on_media_access_result = _declare("zebkit_view_on_media_access_result", None, _V, _TOLD, _P)
_view_media_access_timeout = _declare("zebkit_view_set_media_access_timeout", _B, _V, ctypes.c_uint32)
_media_access_info = _declare("zebkit_media_access_request_info", _P, _P)
_media_access_respond = _declare("zebkit_media_access_respond", _B, _P, _B, _S, _S)
_view_set_reserved_keys = _declare("zebkit_view_set_reserved_keys", _B, _V, _S, _EP)
_view_on_reserved_key = _declare("zebkit_view_on_reserved_key", None, _V, _TOLD, _P)
_view_on_context_menu = _declare("zebkit_view_on_context_menu", None, _V, _TOLD, _P)
_CONTEXT_MENU_ITEM = ctypes.CFUNCTYPE(None, _V, _S, _P)
_view_context_menu_run = _declare("zebkit_view_context_menu_run", _B, _V, ctypes.c_uint64, _S, _EP)
_view_context_menu_show = _declare("zebkit_view_context_menu_show", _B, _V, ctypes.c_uint64, _S,
                                   _CONTEXT_MENU_ITEM, _P, _EP)
_view_set_fullscreen_policy = _declare("zebkit_view_set_fullscreen_policy", _B, _V, _I)
_view_on_fullscreen = _declare("zebkit_view_on_fullscreen", None, _V, _TOLD, _P)
_view_set_network_filter = _declare("zebkit_view_set_network_filter", _B, _V, _S, _EP)
_view_on_network = _declare("zebkit_view_on_network", None, _V, _ASKED, _P)
_view_on_network_event = _declare("zebkit_view_on_network_event", None, _V, _TOLD, _P)
_view_network_timeout = _declare("zebkit_view_set_network_timeout", _B, _V, ctypes.c_uint32)
_network_info = _declare("zebkit_network_request_info", _P, _P)
_network_respond = _declare("zebkit_network_respond", _B, _P, _S, ctypes.c_char_p, _Z, _EP)
_view_network_for_workers = _declare("zebkit_view_set_network_filter_for_workers", _B, _V, _B)
_NETWORK_BODY = ctypes.CFUNCTYPE(None, _V, _S, _P, _Z, _B, _B, _P)
_view_on_network_body = _declare("zebkit_view_on_network_body", None, _V, _NETWORK_BODY, _P)
_network_respond_file = _declare("zebkit_network_respond_file", _B, _P, _S, _S, _EP)
_network_respond_stream = _declare("zebkit_network_respond_stream", _P, _P, _S, _EP)
_network_stream_write = _declare("zebkit_network_stream_write", _B, _P, ctypes.c_char_p, _Z)
_STREAM_WRITABLE = ctypes.CFUNCTYPE(None, _P, _P)
_network_stream_on_writable = _declare("zebkit_network_stream_on_writable", None, _P, _STREAM_WRITABLE, _P)
_network_stream_is_open = _declare("zebkit_network_stream_is_open", _B, _P)
_network_stream_end = _declare("zebkit_network_stream_end", None, _P, _B)


class _Request:
    """An engine-thread request with a deadline: metadata stays, info ends with it.
    When an answer returns False, error is what the engine said of it."""
    _info = None
    error = None

    def __init__(self, handle, metadata):
        self._handle = handle
        self.metadata = metadata

    def _answer(self, function, *args):
        """function(handle, *args, char **error): True, or False with self.error."""
        _require_thread("a request")
        error = _P()
        if function(self._handle, *args, ctypes.byref(error)):
            self.error = None
            return True
        self.error = _refused(error, "refused")
        return False

    @property
    def info(self):
        value = _string(type(self)._info(self._handle))
        return json.loads(value) if value else None


class DownloadRequest(_Request):
    _info = _download_info

    def respond(self, action, path=None):
        """respond('save', absolute_path) or respond('cancel'); False when stale or invalid."""
        if action not in ("save", "cancel"):
            raise ValueError("a download is saved or cancelled")
        encoded = None if path is None else os.fsencode(path)
        return self._answer(_download_respond, 0 if action == "save" else 1, encoded)


class EyeDropperRequest(_Request):
    _info = _eye_dropper_info

    def respond(self, color=None):
        """respond("#rrggbb") gives the page that colour; respond() cancels."""
        return bool(_eye_dropper_respond(self._handle, color.encode() if color else None))


class DeviceChooserRequest(_Request):
    """navigator.usb.requestDevice(), navigator.hid.requestDevice(),
    navigator.serial.requestPort(): metadata["kind"], ["devices"] (info has the
    list as it is now)."""
    _info = _device_chooser_info

    def respond(self, device_id=None):
        """respond(id) gives the page that device; respond() gives none."""
        return bool(_device_chooser_respond(self._handle, device_id.encode() if device_id else None))


class ShareRequest(_Request):
    _info = _share_info

    def respond(self, shared=True):
        """respond(True): shared; respond(False): cancelled (AbortError)."""
        return bool(_share_respond(self._handle, bool(shared)))


class ClientCertificateRequest(_Request):
    _info = _client_certificate_info

    def respond(self, action, certificate=None, key=None):
        """respond('none'), respond('cancel') or respond('provide', pem_chain, pem_key)."""
        actions = {"none": 0, "provide": 1, "cancel": 2}
        if action not in actions:
            raise ValueError("a client certificate is none, provide or cancel")
        return self._answer(_client_certificate_respond, actions[action],
                            None if certificate is None else certificate.encode(),
                            None if key is None else key.encode())

    def choose(self, index):
        """One of metadata['identities'], the system store's (clientCertificateStores)."""
        return self._answer(_client_certificate_choose, int(index))


class FileChooserRequest(_Request):
    _info = _file_chooser_info

    def respond(self, paths=()):
        """Absolute paths; none cancels. False when stale or invalid."""
        if isinstance(paths, (str, bytes)):
            raise TypeError("paths is a list")
        encoded = [os.fsencode(path) for path in paths]
        array = (ctypes.c_char_p * len(encoded))(*encoded)
        return self._answer(_file_chooser_respond, array, len(encoded))


class MediaAccessRequest(_Request):
    _info = _media_access_info

    def respond(self, allow, audio_device=None, video_device=None):
        return bool(_media_access_respond(self._handle, bool(allow),
                                          None if audio_device is None else audio_device.encode(),
                                          None if video_device is None else video_device.encode()))


class NetworkRequest(_Request):
    _info = _network_info

    def respond(self, action, body=b"", **fields):
        """respond('continue', headers=[...]), respond('block', error='failed'),
        respond('redirect', url=...), respond('fulfill', body=bytes, status=200, headers=[...])."""
        reply = dict(fields, action=action)
        body = bytes(body)
        return self._answer(_network_respond, json.dumps(reply).encode(), body, len(body))

    def respond_file(self, path, **fields):
        """Fulfill from an absolute path: respond_file(path, status=200, headers=[...])."""
        return self._answer(_network_respond_file, json.dumps(fields).encode(), os.fsencode(path))

    def respond_stream(self, **fields):
        """Fulfill with what is written: a NetworkStream, or None when invalid or
        stale (error says which)."""
        _require_thread("a request")
        error = _P()
        handle = _network_respond_stream(self._handle, json.dumps(fields).encode(), ctypes.byref(error))
        if not handle:
            self.error = _refused(error, "refused")
            return None
        self.error = None
        return NetworkStream(handle)


class NetworkStream:
    """An answer written as it goes, on the engine thread; end() frees it.
    Open until then; one that is collected open ends as failed, a body cut
    short, as Rust's does when dropped (on the engine's thread: posted there
    when it is collected on another)."""
    def __init__(self, handle):
        self._handle = handle
        # The engine holds on_writable's callback until end(): that is kept
        # (_alive), not the stream, which may be collected.
        self._writable = None

    def __del__(self):
        handle = getattr(self, "_handle", None)
        if handle is None:
            return
        self._handle = None
        writable, self._writable = self._writable, None

        def end():
            _network_stream_end(handle, True)
            _alive.discard(writable)
            _retire(writable)
        if _engine_thread is not None and threading.get_ident() == _engine_thread:
            end()
        elif not post(end):
            # The engine has stopped: the stream went with it.
            _alive.discard(writable)

    def write(self, data):
        """True when the stream took all of data. False is not an error: it
        does not fit now (at most 1 MiB waits; on_writable says when it does)
        or nobody reads any more (is_open says which)."""
        if self._handle is None:
            return False
        _require_thread("NetworkStream.write()")
        data = bytes(data)
        return bool(_network_stream_write(self._handle, data, len(data)))

    def on_writable(self, function):
        """function() once after a write that returned false: it fits, or is_open is false."""
        if self._handle is None:
            return
        _require_thread("NetworkStream.on_writable()")
        previous = self._writable
        self._writable = _STREAM_WRITABLE(_guard(lambda stream, data: function())) if function else _STREAM_WRITABLE()
        _alive.add(self._writable)
        _network_stream_on_writable(self._handle, self._writable, None)
        if previous is not None:
            _alive.discard(previous)
            _retire(previous)

    @property
    def is_open(self):
        if self._handle is None:
            return False
        _require_thread("NetworkStream.is_open")
        return bool(_network_stream_is_open(self._handle))

    def end(self, failed=False):
        if getattr(self, "_handle", None) is not None:
            _require_thread("NetworkStream.end()")
            handle, self._handle = self._handle, None
            _network_stream_end(handle, bool(failed))
            if self._writable is not None:
                _alive.discard(self._writable)
                _retire(self._writable)
            self._writable = None


class HttpAuthRequest:
    """An expiring HTTP/proxy challenge; credentials are not kept on this object."""
    def __init__(self, handle, metadata):
        self._handle = handle
        self.metadata = metadata

    @property
    def info(self):
        value = _string(_http_auth_info(self._handle))
        return json.loads(value) if value else None

    def respond(self, username=None, password=None):
        """Reply once with two strings, or cancel with two None values."""
        if username is None and password is None:
            return bool(_http_auth_respond(self._handle, None, None))
        if not isinstance(username, str) or not isinstance(password, str):
            raise TypeError("HTTP authentication needs two strings or two None values")
        if '\0' in username or '\0' in password:
            raise ValueError("HTTP authentication credentials cannot contain NUL")
        user, secret = username.encode(), password.encode()
        if len(user) > 65536 or len(secret) > 65536:
            raise ValueError("each HTTP authentication credential is limited to 65536 UTF-8 bytes")
        return bool(_http_auth_respond(self._handle, user, secret))


class CertificateRequest:
    """A document TLS error; keeping this object does not extend its deadline."""
    def __init__(self, handle, metadata):
        self._handle = handle
        self.metadata = metadata

    @property
    def info(self):
        value = _string(_certificate_info(self._handle))
        return json.loads(value) if value else None

    def respond(self, allow=False):
        """Reply once; False for stale requests or attempts to ignore strict errors."""
        if not isinstance(allow, bool):
            raise TypeError("certificate reply must be a boolean")
        return bool(_certificate_respond(self._handle, allow))


class NavigationRequest:
    """An engine-thread navigation decision; retaining it does not extend its deadline.

    metadata is a snapshot; info becomes None after settlement. A reply affects
    one stage of one navigation. Unanswered stages continue after their deadline.
    """
    def __init__(self, handle, metadata):
        self._handle = handle
        self.metadata = metadata

    @property
    def info(self):
        value = _string(_navigation_info(self._handle))
        return json.loads(value) if value else None

    def respond(self, action='continue', replacement_url=None):
        """Continue, cancel, or redirect as GET in the same frame; False if stale/invalid."""
        actions = {'continue': 0, 'cancel': 1, 'redirect': 2}
        if action not in actions:
            raise ValueError("unknown navigation action")
        if replacement_url is not None and (not isinstance(replacement_url, str) or '\0' in replacement_url):
            raise ValueError("replacement URL must be a NUL-free string")
        return bool(_navigation_respond(self._handle, actions[action],
                    replacement_url.encode() if replacement_url is not None else None, None))



def _permission_state(state):
    if state not in _PERMISSION_STATES:
        raise ValueError("permission state must be prompt, granted or denied")
    return _PERMISSION_STATES[state]


def _permission_text(value):
    if value is None:
        return None
    if not isinstance(value, str) or '\0' in value:
        raise ValueError("permission name and origin must be NUL-free strings")
    return value.encode()


class PermissionRequest:
    """A permission prompt. Use only on the engine thread; post worker replies.

    metadata is a snapshot. info is None after cancellation, timeout or reply;
    respond returns False for stale requests, without affecting later prompts.
    """

    def __init__(self, handle, metadata):
        self._handle = handle
        self.metadata = metadata

    @property
    def info(self):
        value = _string(_permission_info(self._handle))
        return json.loads(value) if value else None

    def respond(self, state, remember=False):
        """Reply once; remember stores a rule for the requesting origin."""
        return bool(_permission_respond(self._handle, _permission_state(state), bool(remember)))

_view_popup_policy = _declare("zebkit_view_set_popup_policy", _B, _V, _I)
_view_get_popup_policy = _declare("zebkit_view_get_popup_policy", _I, _V)
_view_on_popup_blocked = _declare("zebkit_view_on_popup_blocked", None, _V, _POPUP_BLOCKED, _P)
_view_evaluate = _declare("zebkit_view_evaluate", None, _V, _S, _RESULT, _P)
_view_grant = _declare("zebkit_view_grant", None, _V, _S, _U, _S)
_view_bind = _declare("zebkit_view_bind", None, _V, _S, _BIND, _P)
_call_resolve = _declare("zebkit_call_resolve", None, _P, _S)
_call_resolve_json = _declare("zebkit_call_resolve_json", None, _P, _S, ctypes.c_size_t)
_call_reject = _declare("zebkit_call_reject", None, _P, _S)
_cdp_listen = _declare("zebkit_cdp_listen", _U, _S, _U, _EP)
_cdp_get_port = _declare("zebkit_cdp_get_port", _U)
_cdp_get_url = _declare("zebkit_cdp_get_url", _P)
_cdp_stop = _declare("zebkit_cdp_stop", None)
_compress = _declare("zebkit_compress", _P, _I, _P, _Z, _I, _ZP, _EP)
_decompress = _declare("zebkit_decompress", _P, _I, _P, _Z, _Z, _ZP, _EP)
_digest_size = _declare("zebkit_digest_size", _Z, _I)
_digest_compute = _declare("zebkit_digest_compute", _B, _I, _P, _Z, _P)
_hmac = _declare("zebkit_hmac", _B, _I, _P, _Z, _P, _Z, _P)
_random_bytes = _declare("zebkit_random_bytes", _B, _P, _Z)
_aes_gcm_seal = _declare("zebkit_aes_gcm_seal", _P, _P, _Z, _P, _Z, _P, _Z, _P, _Z, _ZP, _EP)
_aes_gcm_open = _declare("zebkit_aes_gcm_open", _P, _P, _Z, _P, _Z, _P, _Z, _P, _Z, _ZP, _EP)
_url_get = _declare("zebkit_url_get", _P, _S, _S, _I)
_url_registrable_domain = _declare("zebkit_url_registrable_domain", _P, _S)
_image_decode = _declare("zebkit_image_decode", _P, _P, _Z, _IP, _IP, _EP)
_image_encode = _declare("zebkit_image_encode", _P, _I, _P, _I, _I, _I, _ZP, _EP)
_icu_init = _declare("zebkit_icu_init", _B)
_fetch = _declare("zebkit_fetch", None, _S, _S, _S, _P, _Z, _RESPONSE, _P)

# Callbacks and views that C still holds, by id: kept here so that Python
# does not free them. ctypes' function objects cannot be hashed.
class _Alive(dict):
    def add(self, item):
        self[id(item)] = item

    def discard(self, item):
        self.pop(id(item), None)


_alive = _Alive()


def _bytes(data):
    if isinstance(data, str):
        data = data.encode()
    return bytes(data)


def _string(pointer):
    """A string the engine returned, freed."""
    if not pointer:
        return None
    try:
        return ctypes.string_at(pointer).decode(errors="replace")
    finally:
        _free(pointer)


def _text(value, default=""):
    """A string the engine lent to a callback (bytes, or None for NULL)."""
    return default if value is None else value.decode(errors="replace")


def _retire(callback):
    """Keeps a callback the engine was given until a task after this one:
    it may be the one running now, which replacing it must not free."""
    if callback is not None:
        post(lambda: callback)


# The thread that started the engine (main() or init()); None before.
_engine_thread = None


def _require_thread(what="this"):
    if _engine_thread is not None and threading.get_ident() != _engine_thread:
        raise Error(f"{what} is for the engine's thread, the one that started it; post() runs a function there")


def _buffer(pointer, size):
    """A buffer the engine returned, freed."""
    try:
        return ctypes.string_at(pointer, size)
    finally:
        _free(pointer)


def _raise(error, fallback):
    message = _string(error.value) if error.value else fallback
    raise Error(message)


def _refused(error, fallback):
    """What the engine said when it refused (freed), or fallback."""
    return (_string(error.value) if error.value else None) or fallback


def _guard(function):
    """Exceptions do not cross into C: report them and carry on."""
    def call(*args):
        try:
            return function(*args)
        except Exception:  # noqa: BLE001
            sys.excepthook(*sys.exc_info())
    return call


# ---- the engine ----

def version():
    return _version().decode()


def engine_version():
    return _engine_version().decode()


def environment():
    """What the system says of how things should look: colorScheme, contrast,
    forcedColors, locale, accessibility, following."""
    _require_thread("environment()")
    value = _string(_get_environment())
    return json.loads(value) if value else None


def on_environment_changed(function):
    """function(dict) on each change of environment(); None stops."""
    global _environment_callback
    _require_thread("on_environment_changed()")
    previous = _environment_callback
    if function is None:
        _environment_callback = None
        _on_environment_changed(_ENVIRONMENT(), None)
    else:
        _environment_callback = _ENVIRONMENT(_guard(lambda payload, d: function(json.loads(_text(payload, "{}")))))
        _on_environment_changed(_environment_callback, None)
    _retire(previous)


def _json_event(kind, setter, function):
    _require_thread()
    previous = _system_callbacks.pop(kind, None)
    if function is None:
        setter(_JSON_EVENT(), None)
    else:
        callback = _JSON_EVENT(_guard(lambda payload, d: function(json.loads(_text(payload, "{}")))))
        _system_callbacks[kind] = callback
        setter(callback, None)
    _retire(previous)


def on_notification(function):
    """function(dict) shows web notifications: {"event": "show" | "close", ...};
    None: the system shows them."""
    _json_event("notification", _on_notification, function)


def notification_clicked(notification_id, action=-1):
    _require_thread("notification_clicked()")
    return bool(_notification_clicked(notification_id.encode(), int(action)))


def notification_closed(notification_id, by_user=True):
    _require_thread("notification_closed()")
    return bool(_notification_closed(notification_id.encode(), bool(by_user)))


def on_power_event(function):
    """function(dict): suspend, resume, power-source, lock, unlock."""
    _json_event("power", _on_power_event, function)


def displays():
    """The screens: [{id, bounds, workArea, scale, rotation, primary}]."""
    _require_thread("displays()")
    value = _string(_get_displays())
    return json.loads(value) if value else []


def on_displays_changed(function):
    _json_event("displays", _on_displays_changed, function)


def prevent_sleep(reason, keep_display_on=False):
    """Keeps the system awake until allow_sleep(token)."""
    _require_thread("prevent_sleep()")
    return int(_prevent_sleep(bool(keep_display_on), reason.encode()))


def allow_sleep(token):
    _require_thread("allow_sleep()")
    return bool(_allow_sleep(int(token)))


def open_external(url):
    """The system's handler for url."""
    _require_thread("open_external()")
    error = _P()
    if not _open_external(url.encode(), ctypes.byref(error)):
        _raise(error, "the URL was not opened")


def show_in_folder(path):
    """The file manager, with path selected."""
    _require_thread("show_in_folder()")
    error = _P()
    if not _show_in_folder(os.fsencode(path), ctypes.byref(error)):
        _raise(error, "the file was not shown")


class Tray:
    """The application's icon in the system's tray (png: bytes of a PNG)."""

    def __init__(self, png, tooltip=""):
        _require_thread("Tray()")
        error = _P()
        self._handle = _tray_new(png, len(png), tooltip.encode(), ctypes.byref(error))
        if not self._handle:
            _raise(error, "the tray was not made")
        self._callbacks = {}
        # Up until close(), with the functions the engine calls: kept here.
        _alive.add(self)

    def _check(self):
        _require_thread("a tray icon")
        if not self._handle:
            raise Error("the tray icon is closed")
        return self._handle

    def set_icon(self, png):
        error = _P()
        if not _tray_set_icon(self._check(), png, len(png), ctypes.byref(error)):
            _raise(error, "the icon was refused")

    def set_tooltip(self, tooltip):
        _tray_set_tooltip(self._check(), tooltip.encode())

    def set_menu(self, menu, function):
        """Items as app_set_menu's; function(id) for one chosen."""
        handle = self._check()
        callback = _TRAY_MENU(_guard(lambda item, d: function(_text(item))))
        error = _P()
        if not _tray_set_menu(handle, json.dumps(menu).encode(), callback, None, ctypes.byref(error)):
            _raise(error, "the menu was refused")
        _retire(self._callbacks.get("menu"))
        self._callbacks["menu"] = callback

    def on_click(self, function):
        handle = self._check()
        callback = _TRAY_CLICK(_guard(lambda d: function())) if function else _TRAY_CLICK()
        _tray_on_click(handle, callback, None)
        _retire(self._callbacks.get("click"))
        self._callbacks["click"] = callback

    def close(self):
        if self._handle:
            _tray_close(self._handle)
            self._handle = None
            for callback in self._callbacks.values():
                _retire(callback)
            self._callbacks = {}
        _alive.discard(self)


def capabilities():
    """What this build can do on this system and display (<zebkit/window.h>)."""
    _require_thread("capabilities()")
    value = _string(_get_capabilities())
    return json.loads(value) if value else None


def is_headless():
    _require_thread("is_headless()")
    return bool(_is_headless())


def main(ready, argv=None):
    """Start the engine, call ready() once views can be made, run until
    quit(); returns the exit code. Chromium's switches in argv (by default
    sys.argv) are honoured, such as --remote-debugging-port=9222."""
    global _engine_thread
    argv = list(sys.argv if argv is None else argv) or ["python"]
    array = (ctypes.c_char_p * (len(argv) + 1))(*[a.encode() for a in argv], None)
    callback = _TASK(_guard(lambda data: ready()))
    _alive.add(callback)
    _engine_thread = threading.get_ident()
    try:
        return _main(len(argv), array, callback, None)
    finally:
        _alive.discard(callback)


def init(argv=None, *, settings=None):
    """Initialize on the main thread for a host-owned loop; once per process.
    Returns 0 on success or Chromium's startup code. With settings (a dict or
    JSON string), invalid settings/startup errors raise Error with the native
    explanation. Use set_wakeup for event-driven pumping, or a toolkit timer;
    call shutdown outside callbacks when the engine is no longer needed.
    """
    global _engine_thread
    argv = list(sys.argv if argv is None else argv) or ["python"]
    array = (ctypes.c_char_p * (len(argv) + 1))(*[a.encode() for a in argv], None)
    if settings is None:
        code = _init(len(argv), array)
    else:
        source = settings if isinstance(settings, str) else json.dumps(settings)
        error = ctypes.c_void_p()
        code = _init_settings(len(argv), array, source.encode(), ctypes.byref(error))
        message = _string(error.value)
        if code:
            raise Error(message or f"engine startup failed: {code}")
    if code == 0:
        _engine_thread = threading.get_ident()
    return code


def set_engine_settings(settings):
    """What zebkit decides for the whole engine (<zebkit/hooks.h>): thirdPartyCookies,
    spareRenderer, dialogs, downloads, newWindows. All of it applies or none."""
    _require_thread("set_engine_settings()")
    source = settings if isinstance(settings, str) else json.dumps(settings)
    if '\0' in source:
        raise Error("runtime settings contain NUL")
    error = ctypes.c_void_p()
    if not _set_engine_settings(source.encode(), ctypes.byref(error)):
        _raise(error, "runtime settings rejected")


def engine_settings():
    """{"explicit": ..., "effective": ...}."""
    _require_thread("engine_settings()")
    return json.loads(_string(_get_engine_settings()))


def pump():
    """Process ready engine/native events without waiting; False before
    init, after shutdown and inside a callback (a nested pump)."""
    _require_thread("pump()")
    return bool(_pump())


def set_wakeup(function):
    """Before init, set function(delay_ms) to queue a host event/timer.
    It may run on any engine thread: never call zebkit or block in it.
    Keep the earliest deadline and consume it before pump(). None removes it.
    """
    global _wakeup_callback
    _require_thread("set_wakeup()")
    callback = _WAKEUP(_guard(lambda delay, data: function(delay))) if function else _WAKEUP()
    if not _set_wakeup(callback, None):
        return False
    _wakeup_callback = callback if function else None
    return True


def shutdown():
    """Close views and stop the engine on its initializing thread, outside callbacks.
    Returns False if invalid. The engine cannot be restarted after shutdown.
    """
    global _wakeup_callback, _engine_thread
    _require_thread("shutdown()")
    if not _shutdown():
        return False
    _wakeup_callback = None
    _engine_thread = None
    return True


def quit():  # noqa: A001
    _require_thread("quit()")
    _quit()


def post(function, delay_ms=0):
    """Run function() on the engine's thread; any thread may call this.
    Before the engine has started and once it has stopped, function never
    runs (False)."""
    def run(data):
        _alive.discard(callback)
        function()
    callback = _TASK(_guard(run))
    _alive.add(callback)
    if delay_ms:
        posted = _post_delayed(callback, None, int(delay_ms))
    else:
        posted = _post(callback, None)
    if not posted:
        _alive.discard(callback)
    return bool(posted)


class View:
    """A page, in a window when the machine has a display."""

    def __init__(self, url=None, width=0, height=0, options=None, context=None):
        """A view loading url. options (<zebkit/window.h>): the window's, before
        it is made, such as {"frame": "overlay", "show": False}; width and
        height go into them. context: a Context of its own cookies and
        storage, else the default one."""
        _require_thread("View()")
        context_handle = context._check() if context is not None else None
        if options is None and context_handle:
            self._handle = _view_new_in(context_handle, url.encode() if url else None, width, height)
        elif options is None:
            self._handle = _view_new(url.encode() if url else None, width, height)
        else:
            given = dict(options)
            if width:
                given.setdefault("width", width)
            if height:
                given.setdefault("height", height)
            error = _P()
            self._handle = _view_new_with_options(context_handle, url.encode() if url else None,
                                                  json.dumps(given).encode(), ctypes.byref(error))
            if not self._handle:
                _raise(error, "cannot create a view with these window options")
        if not self._handle:
            raise Error("cannot create a view")
        self._initialize_callbacks()

    @property
    def window(self):
        """The window's state (<zebkit/window.h>): state, bounds, frame,
        titlebarArea, controls, capabilities."""
        value = _string(_view_get_window(self._check()))
        return json.loads(value) if value else None

    def set_window(self, options=None, **fields):
        """Changes the window's runtime options, merged by key."""
        given = dict(options or {}, **fields)
        error = _P()
        if not _view_set_window(self._check(), json.dumps(given).encode(), ctypes.byref(error)):
            _raise(error, "window options rejected")

    def window_action(self, action):
        """show, hide, minimize, maximize, restore, fullscreen,
        exit-fullscreen, close: asked of the system; the state tells."""
        error = _P()
        if not _view_window_action(self._check(), action.encode(), ctypes.byref(error)):
            _raise(error, "window action refused")

    def on_window_state(self, function):
        """function(dict) for every change of the window's state; None removes it."""
        callback = _WINDOW_STATE(_guard(lambda h, payload, d: function(json.loads(_text(payload, "{}"))))) \
            if function is not None else _WINDOW_STATE()
        self._set("window-state", callback, _view_on_window_state)

    def set_drag_regions(self, regions):
        """With dragRegions.source "host": [{"x", "y", "width", "height", "drag"}]."""
        error = _P()
        if not _view_set_drag_regions(self._check(), json.dumps(list(regions)).encode(), ctypes.byref(error)):
            _raise(error, "drag regions rejected")

    def set_popup_window_options(self, options):
        """Options for the windows this view's pages open; None: the system's frame."""
        error = _P()
        text = json.dumps(options).encode() if options is not None else None
        if not _view_set_popup_window_options(self._check(), text, ctypes.byref(error)):
            _raise(error, "popup window options rejected")

    @classmethod
    def _adopt(cls, handle):
        """Wrap a newly created native popup exactly once, from its creation callback."""
        view = cls.__new__(cls)
        view._handle = handle
        view._initialize_callbacks()
        return view

    def _initialize_callbacks(self):
        self._callbacks = {}
        self._on_close_user = None
        self._close_hooks = []
        self._closed = False
        callback = _VIEW(_guard(self._closed_by_engine))
        self._callbacks["close"] = callback
        _view_on_close(self._handle, callback, None)
        _alive.add(self)

    def _set(self, kind, callback, setter):
        """Gives the engine callback (a NULL one removes it) for kind. The
        one it replaces lives until a task after this one: it may be the
        one running."""
        handle = self._check()
        previous = self._callbacks.get(kind)
        setter(handle, callback, None)
        self._callbacks[kind] = callback
        _retire(previous)

    def _closed_by_engine(self, handle, data):
        self._closed = True
        _alive.discard(self)
        try:
            if self._on_close_user:
                self._on_close_user()
        finally:
            for hook in self._close_hooks:
                hook()
            self._close_hooks = []
            # What the callbacks hold may hold the view; this one is running.
            callbacks, self._callbacks = self._callbacks, {}
            _retire(callbacks)

    def _check(self):
        _require_thread("a view")
        if self._closed:
            raise Error("the view is closed")
        return self._handle

    def load(self, url):
        _view_load_url(self._check(), url.encode())

    def reload(self):
        _view_reload(self._check())

    def back(self):
        _view_go_back(self._check())

    def forward(self):
        _view_go_forward(self._check())

    def target_id(self):
        return _string(_view_target(self._check()))

    def resize(self, width, height):
        _view_resize(self._check(), width, height)

    def attach_native(self, parent, x, y, width, height):
        """In a window of the application's: an X11 window id, or an NSView* as an int."""
        error = ctypes.c_void_p()
        if not _view_attach_native(self._check(), parent, x, y, width, height, ctypes.byref(error)):
            _raise(error, "the window cannot be attached")

    def native_window(self):
        """Borrowed containing system window as {kind, handle}, or None.

        kind is x11, win32 or appkit; appkit means NSWindow*, not NSView*.
        Requery after attach/detach. Never destroy or release this handle.
        """
        info = _NativeWindowInfo()
        if not _view_get_native_window(self._check(), ctypes.byref(info)):
            return None
        return {"kind": {1: "x11", 2: "win32", 3: "appkit"}[info.kind], "handle": info.handle}

    def export_native_window(self, function):
        """function(handle) once, later: "x11:<XID>" or "wayland:<handle>" for
        the system's portals to parent a dialog to, or None when there is none
        (always on Windows and macOS, where native_window() is the handle)."""
        def done(data, handle):
            _alive.discard(callback)
            function(handle.decode() if handle else None)
        callback = _NATIVE_WINDOW_EXPORT(_guard(done))
        _alive.add(callback)
        if not _view_export_native_window(self._check(), callback, None):
            _alive.discard(callback)
            function(None)

    def detach_native(self):
        """Back to its own window; False when it was in none of the application's."""
        return _view_detach_native(self._check())

    def focus(self):
        """Focus to the view, as when a person clicks it; a page goes
        fullscreen only from a view with focus."""
        _view_focus(self._check())

    def set_bounds(self, x, y, width, height):
        """Where an attached view is in its parent."""
        _view_set_bounds(self._check(), x, y, width, height)

    def set_settings(self, settings):
        """Merge native preferences; None values reset individual keys."""
        source = settings if isinstance(settings, str) else json.dumps(settings)
        if '\0' in source:
            raise Error("view settings contain NUL")
        error = ctypes.c_void_p()
        if not _view_set_settings(self._check(), source.encode(), ctypes.byref(error)):
            _raise(error, "view settings rejected")

    @property
    def settings(self):
        """Explicit native preferences; temporary CDP emulation is separate."""
        return json.loads(_string(_view_get_settings(self._check())))

    def set_runtime_settings(self, settings):
        """What zebkit decides for this view (<zebkit/hooks.h>): thirdPartyCookies,
        dialogs, downloads, newWindows. All of it applies or none."""
        source = settings if isinstance(settings, str) else json.dumps(settings)
        if '\0' in source:
            raise Error("runtime settings contain NUL")
        error = ctypes.c_void_p()
        if not _view_set_runtime_settings(self._check(), source.encode(), ctypes.byref(error)):
            _raise(error, "runtime settings rejected")

    @property
    def runtime_settings(self):
        """{"explicit": ..., "effective": ...}."""
        return json.loads(_string(_view_get_runtime_settings(self._check())))

    def set_permission(self, permission, state, origin=None):
        """Set a view rule; None uses the current origin, '*' a default rule."""
        error = _P()
        if not _view_set_permission(self._check(), _permission_text(origin),
                                    _permission_text(permission), _permission_state(state), ctypes.byref(error)):
            _raise(error, "permission rule rejected")

    def get_permission(self, permission, origin=None):
        """Effective policy for a concrete origin; document security still applies."""
        state = _view_get_permission(self._check(), _permission_text(origin), _permission_text(permission))
        if state < 0:
            raise Error("invalid permission or origin")
        return ("prompt", "granted", "denied")[state]

    def reset_permissions(self, origin=None):
        """Remove all view rules, or only rules at the supplied origin/default."""
        error = _P()
        if not _view_reset_permissions(self._check(), _permission_text(origin), ctypes.byref(error)):
            _raise(error, "permission reset rejected")

    def set_permission_timeout(self, milliseconds):
        """Deadline for future prompts, from 1 to 300000 milliseconds."""
        self._deadline(_view_permission_timeout, milliseconds, "permission timeout")

    def on_permission(self, function):
        """function(PermissionRequest); None restores the default permission policy."""
        self._asked("permission", _view_on_permission, PermissionRequest, function, _PERMISSION)

    def on_permission_result(self, function):
        """function(dict) receives asynchronous outcomes; None removes it."""
        self._told("permission-result", _view_on_permission_result, function, _PERMISSION_RESULT)

    def set_certificate_timeout(self, milliseconds):
        """Deadline for future certificate decisions, from 1 to 300000 milliseconds."""
        self._deadline(_view_certificate_timeout, milliseconds, "certificate timeout")

    def on_certificate(self, function):
        """function(CertificateRequest); None denies pending requests and removes it."""
        self._asked("certificate", _view_on_certificate, CertificateRequest, function, _CERTIFICATE)

    def on_certificate_result(self, function):
        """function(dict) observes asynchronous outcomes; None removes it."""
        self._told("certificate-result", _view_on_certificate_result, function, _CERTIFICATE_RESULT)

    def set_http_auth_timeout(self, milliseconds):
        """Deadline for future HTTP challenges, from 1 to 300000 milliseconds."""
        self._deadline(_view_http_auth_timeout, milliseconds, "HTTP authentication timeout")

    def on_http_auth(self, function):
        """function(HttpAuthRequest); None cancels pending challenges and removes it."""
        self._asked("http-auth", _view_on_http_auth, HttpAuthRequest, function, _HTTP_AUTH)

    def on_http_auth_result(self, function):
        """function(dict) observes credential provision/cancellation, not HTTP success."""
        self._told("http-auth-result", _view_on_http_auth_result, function, _HTTP_AUTH_RESULT)

    def _asked(self, kind, setter, request_type, function, kind_type=None):
        """function(request_type) for what the page asks; None removes it."""
        kind_type = kind_type or _ASKED
        self._check()
        if function is not None and not callable(function):
            raise TypeError(kind + " handler must be callable or None")
        callback = kind_type(_guard(lambda h, request, payload, d:
                    function(request_type(request, json.loads(_text(payload, "{}")))))) \
            if function is not None else kind_type()
        self._set(kind, callback, setter)

    def _told(self, kind, setter, function, kind_type=None):
        """function(dict) for what the engine tells; None removes it."""
        kind_type = kind_type or _TOLD
        self._check()
        if function is not None and not callable(function):
            raise TypeError(kind + " observer must be callable or None")
        callback = kind_type(_guard(lambda h, payload, d: function(json.loads(_text(payload, "{}"))))) \
            if function is not None else kind_type()
        self._set(kind, callback, setter)

    def _deadline(self, setter, milliseconds, what=None):
        if isinstance(milliseconds, bool) or not isinstance(milliseconds, int) or not 1 <= milliseconds <= 300000:
            raise ValueError(f"{what} must be an integer from 1 to 300000" if what
                             else "a deadline is an integer from 1 to 300000")
        if not setter(self._check(), milliseconds):
            raise Error(f"{what or 'deadline'} rejected")

    def on_external_protocol(self, function):
        """function(dict): a link to another program. The engine opens nothing."""
        self._told('external-protocol', _view_on_external_protocol, function)

    def on_download(self, function):
        """function(DownloadRequest); None cancels pending ones and removes it."""
        self._asked('download', _view_on_download, DownloadRequest, function)

    def on_download_progress(self, function):
        self._told('download-progress', _view_on_download_progress, function)

    def set_download_timeout(self, milliseconds):
        self._deadline(_view_download_timeout, milliseconds)

    def cancel_download(self, download_id):
        return bool(_view_cancel_download(self._check(), download_id.encode()))

    def on_client_certificate(self, function):
        """function(ClientCertificateRequest); without one no certificate is sent."""
        self._asked('client-certificate', _view_on_client_certificate, ClientCertificateRequest, function)

    def on_client_certificate_result(self, function):
        self._told('client-certificate-result', _view_on_client_certificate_result, function)

    def set_client_certificate_timeout(self, milliseconds):
        self._deadline(_view_client_certificate_timeout, milliseconds)

    def add_world_script(self, source, world=None, main_frame_only=False):
        """A script before the page's, in the named isolated world or the page's."""
        error = _P()
        if not _view_add_world_script(self._check(), None if world is None else world.encode(),
                                      source.encode(), bool(main_frame_only), ctypes.byref(error)):
            _raise(error, "script or world name rejected")

    def on_file_chooser(self, function):
        """function(FileChooserRequest); None cancels pending ones and removes it."""
        self._asked('file-chooser', _view_on_file_chooser, FileChooserRequest, function)

    def on_eye_dropper(self, function):
        """function(EyeDropperRequest) for the page's EyeDropper.open(); without
        one it is cancelled."""
        self._asked('eye-dropper', _view_on_eye_dropper, EyeDropperRequest, function)

    def on_device_chooser(self, function):
        """function(DeviceChooserRequest) for the page's USB, HID and serial
        choosers; without one the page gets no device."""
        self._asked('device-chooser', _view_on_device_chooser, DeviceChooserRequest, function)

    def on_device_chooser_result(self, function):
        self._told('device-chooser-result', _view_on_device_chooser_result, function)

    def set_device_chooser_timeout(self, milliseconds):
        self._deadline(_view_device_chooser_timeout, milliseconds)

    def on_share(self, function):
        """function(ShareRequest) for the page's navigator.share()."""
        self._asked('share', _view_on_share, ShareRequest, function)

    def set_share_timeout(self, milliseconds):
        self._deadline(_view_share_timeout, milliseconds)

    def on_file_chooser_result(self, function):
        self._told('file-chooser-result', _view_on_file_chooser_result, function)

    def set_file_chooser_timeout(self, milliseconds):
        self._deadline(_view_file_chooser_timeout, milliseconds)

    def on_media_access(self, function):
        """function(MediaAccessRequest) for camera and microphone."""
        self._asked('media-access', _view_on_media_access, MediaAccessRequest, function)

    def on_media_access_result(self, function):
        self._told('media-access-result', _view_on_media_access_result, function)

    def set_media_access_timeout(self, milliseconds):
        self._deadline(_view_media_access_timeout, milliseconds)

    def set_reserved_keys(self, keys):
        """Keys the page does not get, as ['Ctrl+Shift+KeyI']."""
        error = _P()
        if not _view_set_reserved_keys(self._check(), json.dumps(list(keys)).encode(), ctypes.byref(error)):
            _raise(error, "reserved keys rejected")

    def on_reserved_key(self, function):
        self._told('reserved-key', _view_on_reserved_key, function)

    def on_context_menu(self, function):
        """function(dict) with what was right-clicked, its menuId and the
        default items; while set, the engine shows no menu itself."""
        self._told('context-menu', _view_on_context_menu, function)

    def context_menu_run(self, menu_id, command):
        """Runs a default item's command against what was right-clicked."""
        error = _P()
        if not _view_context_menu_run(self._check(), int(menu_id), command.encode(), ctypes.byref(error)):
            _raise(error, "the context menu's command was refused")

    def context_menu_show(self, menu_id, items=None, function=None):
        """Shows a menu where it was asked for: the default items (None) or a
        list mixing default ids and the application's own; function(id) for
        an item of the application's ("" when none is chosen)."""
        handle = self._check()
        error = _P()
        callback = _CONTEXT_MENU_ITEM(_guard(lambda h, item, d: function(_text(item)))) if function else \
            _CONTEXT_MENU_ITEM()
        text = json.dumps(items).encode() if items is not None else None
        if not _view_context_menu_show(handle, int(menu_id), text, callback, None, ctypes.byref(error)):
            _raise(error, "the context menu was not shown")
        _retire(self._callbacks.get("context-menu-item"))
        self._callbacks["context-menu-item"] = callback

    def set_fullscreen_policy(self, policy):
        if policy not in ("allow", "deny"):
            raise ValueError("fullscreen is allow or deny")
        if not _view_set_fullscreen_policy(self._check(), 0 if policy == "allow" else 1):
            raise Error("fullscreen policy rejected")

    def on_fullscreen(self, function):
        self._told('fullscreen', _view_on_fullscreen, function)

    def set_network_filter(self, rules):
        """A list of rules, as <zebkit/hooks.h> says; [] holds and tells nothing."""
        error = _P()
        if not _view_set_network_filter(self._check(), json.dumps(rules).encode(), ctypes.byref(error)):
            _raise(error, "network filter rejected")

    def on_network(self, function):
        """function(NetworkRequest) for what a hold rule holds."""
        self._asked('network', _view_on_network, NetworkRequest, function)

    def on_network_event(self, function):
        self._told('network-event', _view_on_network_event, function)

    def set_network_filter_for_workers(self, on):
        """This view's filter also decides for its profile's shared and service workers."""
        return bool(_view_network_for_workers(self._check(), bool(on)))

    def on_network_body(self, function):
        """function(request_id, bytes, end, truncated) for what a rule with body: true matches."""
        self._check()
        if function is not None and not callable(function):
            raise TypeError("network-body observer must be callable or None")
        callback = _NETWORK_BODY(_guard(lambda h, request_id, data, size, end, truncated, d:
                    function(request_id.decode(), ctypes.string_at(data, size) if size else b"",
                             bool(end), bool(truncated)))) if function is not None else _NETWORK_BODY()
        self._set('network-body', callback, _view_on_network_body)

    def set_network_timeout(self, milliseconds):
        self._deadline(_view_network_timeout, milliseconds)

    def set_navigation_fail_closed(self, fail_closed):
        """With True, an unanswered navigation is cancelled instead of continued."""
        if not _view_navigation_fail_closed(self._check(), bool(fail_closed)):
            raise Error("cannot set the navigation policy of this view")

    def set_navigation_timeout(self, milliseconds):
        """Deadline for future navigation decisions, from 1 to 300000 milliseconds."""
        self._deadline(_view_navigation_timeout, milliseconds, "navigation timeout")

    def on_navigation(self, function):
        """function(NavigationRequest); None continues pending requests and removes policy."""
        self._asked("navigation", _view_on_navigation, NavigationRequest, function, _NAVIGATION)

    def on_navigation_result(self, function):
        """function(dict) observes asynchronous decisions; None removes it."""
        self._told("navigation-result", _view_on_navigation_result, function, _NAVIGATION_RESULT)

    def set_popup_policy(self, policy):
        """Set block-without-gesture, allow or block for future new windows."""
        policies = {"block-without-gesture": 0, "allow": 1, "block": 2}
        if policy not in policies:
            raise ValueError("unknown popup policy")
        return bool(_view_popup_policy(self._check(), policies[policy]))

    @property
    def popup_policy(self):
        return ("block-without-gesture", "allow", "block")[_view_get_popup_policy(self._check())]

    @property
    def window_open_info(self):
        """Creation snapshot for a popup; an ordinary view returns an empty dict."""
        value = _view_window_open_info(self._check())
        if not value:
            raise Error("window metadata unavailable on this thread")
        return json.loads(_string(value))

    def on_window_open(self, function):
        """function(child_view, url) receives the original popup with its opener."""
        self._check()
        if function is not None and not callable(function):
            raise TypeError("window handler must be callable or None")
        callback = _WINDOW_OPEN(_guard(lambda h, child, url, d:
                    function(View._adopt(child), _text(url)))) if function is not None else _WINDOW_OPEN()
        self._set('window-open', callback, _view_on_window_open)

    def on_window_open_event(self, function):
        """function(dict) observes decision/created metadata; None removes it."""
        self._check()
        if function is not None and not callable(function):
            raise TypeError("window observer must be callable or None")
        callback = _WINDOW_OPEN_EVENT(_guard(lambda h, payload, d:
                    function(json.loads(payload)))) if function is not None else _WINDOW_OPEN_EVENT()
        self._set('window-open-event', callback, _view_on_window_open_event)

    def on_console(self, function):
        """function(dict) observes native document-console messages; None removes it."""
        self._check()
        if function is not None and not callable(function):
            raise TypeError("console observer must be callable or None")
        callback = _CONSOLE(_guard(lambda h, payload, d:
                    function(json.loads(payload)))) if function is not None else _CONSOLE()
        self._set('console', callback, _view_on_console)

    def on_popup_blocked(self, function):
        """Report a blocked popup as a dict, after the decision; None removes it."""
        self._check()
        callback = _POPUP_BLOCKED(_guard(lambda view, event, data: function(json.loads(event)))) if function else _POPUP_BLOCKED()
        self._set("popup-blocked", callback, _view_on_popup_blocked)

    def on_authentication(self, function):
        """Receive host-owned Passkey events as dictionaries; None restores native UI.

        Never log/persist these events or PIN actions. Off-screen hosts must
        opt into authentication UI. See docs/PASSKEYS.md for the schema.
        """
        handle = self._check()
        # Cleared first: that cancels the outstanding requests.
        self._set("authentication", _AUTHENTICATION(), _view_on_authentication)
        if function is None:
            return True
        callback = _AUTHENTICATION(_guard(lambda h, event, data: function(json.loads(_text(event, "{}")))))
        self._callbacks["authentication"] = callback
        return bool(_view_on_authentication(handle, callback, None))

    def authentication_action(self, action):
        """Queue a current request/revision action; True is not authentication success."""
        return bool(_view_authentication_action(self._check(), json.dumps(action).encode()))

    def set_authentication_ui(self, enabled):
        """Allow Passkey UI for this view."""
        return bool(_view_authentication_ui(self._check(), bool(enabled)))

    def close(self):
        if not self._closed:
            _view_close(self._handle)

    @property
    def url(self):
        return _string(_view_get_url(self._check()))

    @property
    def title(self):
        return _string(_view_get_title(self._check()))

    def on_load(self, function):
        """function(event, url), event one of LOAD_STARTED, LOAD_COMMITTED,
        LOAD_FINISHED and LOAD_FAILED; None removes it."""
        callback = _LOAD(_guard(lambda h, e, u, d: function(e, _text(u)))) if function is not None else _LOAD()
        self._set("load", callback, _view_on_load)

    def on_title(self, function):
        """function(title) whenever the page's title changes; None removes it."""
        callback = _TITLE(_guard(lambda h, t, d: function(_text(t)))) if function is not None else _TITLE()
        self._set("title", callback, _view_on_title)

    def on_close(self, function):
        self._on_close_user = function

    def evaluate(self, script, function=None):
        """Run script in the page; function(value, error), value as from JSON."""
        handle = self._check()
        def done(view, result, error, data):
            _alive.discard(callback)
            if function:
                if error is not None:
                    function(None, _text(error))
                else:
                    function(json.loads(_text(result)) if result else None, None)
        callback = _RESULT(_guard(done))
        _alive.add(callback)
        _view_evaluate(handle, script.encode(), callback, None)

    def set_user_agent_override(self, options, function=None):
        """Set UA/client hints together; function(value, error) reports validation."""
        handle = self._check()
        if function is not None and not callable(function):
            raise TypeError("completion must be callable or None")
        source = options if isinstance(options, str) else json.dumps(options, ensure_ascii=False)
        encoded = source.encode('utf-8')
        if b'\0' in encoded:
            raise ValueError("user agent options must not contain a literal NUL")
        def done(view, result, error, data):
            _alive.discard(callback)
            if function is not None:
                function(json.loads(result) if result else None, _text(error, None))
        callback = _RESULT(_guard(done))
        _alive.add(callback)
        if not _view_user_agent_override(handle, encoded, callback, None):
            # Refused at once: it calls nothing back.
            _alive.discard(callback)
            return False
        return True

    def grant(self, origin, allow, files_root=None):
        """Give pages of origin the zebkit.* capabilities in allow (ALLOW_*)."""
        _view_grant(self._check(), origin.encode(), allow,
                    str(files_root).encode() if files_root else None)

    def bind(self, name, function):
        """Make zebkit.app.<name>(...args) in the page call
        function(*args, origin=...), whose return value (JSON-able) the
        page's promise resolves with; an exception rejects it."""
        def called(handle, origin, args, call, data):
            try:
                result = function(*json.loads(_text(args, "[]")), origin=_text(origin))
                if result is UNDEFINED:
                    _call_resolve(call, None)
                    return
                answer = json.dumps(result).encode()
                _call_resolve_json(call, answer, len(answer))
            except Exception as e:  # noqa: BLE001
                _call_reject(call, str(e).encode())
        handle = self._check()
        callback = _BIND(called) if function is not None else _BIND()
        _view_bind(handle, name.encode(), callback, None)
        _retire(self._callbacks.pop("bind:" + name, None))
        if function is not None:
            self._callbacks["bind:" + name] = callback

    # The three of <zebkit/hooks.h> the binding had declared and not given.
    def on_eye_dropper_result(self, function):
        """function(dict): how an EyeDropper.open() ended."""
        self._told('eye-dropper-result', _view_on_eye_dropper_result, function)

    def set_eye_dropper_timeout(self, milliseconds):
        self._deadline(_view_eye_dropper_timeout, milliseconds)

    def on_share_result(self, function):
        """function(dict): how a navigator.share() ended."""
        self._told('share-result', _view_on_share_result, function)


def cdp_listen(address=None, port=0):
    _require_thread("cdp_listen()")
    error = ctypes.c_void_p()
    bound = _cdp_listen(address.encode() if address else None, port, ctypes.byref(error))
    if not bound:
        _raise(error, "cannot listen")
    return bound


def cdp_port():
    _require_thread("cdp_port()")
    return _cdp_get_port()


def cdp_url():
    _require_thread("cdp_url()")
    return _string(_cdp_get_url())


def cdp_stop():
    _require_thread("cdp_stop()")
    _cdp_stop()


def fetch(url, function, method="GET", headers=None, body=None):
    """Request url through the engine's network stack; function(response)
    gets a dict: status, url, headers (a dict: of a repeated header, such as
    Set-Cookie, the last), header_list (every header, as (name, value)
    pairs in order), body (bytes), error."""
    _require_thread("fetch()")
    def done(response, data):
        _alive.discard(callback)
        r = response.contents
        header_list = []
        for line in _text(r.headers).split("\r\n"):
            name, colon, value = line.partition(":")
            if colon and name.strip():
                header_list.append((name.strip(), value.strip()))
        function({
            "status": r.status,
            "url": _text(r.url),
            "headers": dict(header_list),
            "header_list": header_list,
            "body": ctypes.string_at(r.body, r.body_size) if r.body_size else b"",
            "error": _text(r.error, None),
        })
    callback = _RESPONSE(_guard(done))
    _alive.add(callback)
    lines = "\r\n".join(f"{k}: {v}" for k, v in (headers or {}).items())
    data = _bytes(body) if body is not None else b""
    _fetch(method.encode(), url.encode(), lines.encode() if lines else None, data or None, len(data), callback, None)


# ---- the services ----

def compress(data, format="gzip", level=-1):
    data = _bytes(data)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _compress(_COMPRESSION[format], data, len(data), level, ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "compression failed")
    return _buffer(out, size.value)


def decompress(data, format="gzip", max_size=0):
    data = _bytes(data)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _decompress(_COMPRESSION[format], data, len(data), max_size, ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "decompression failed")
    return _buffer(out, size.value)


def digest(data, algorithm="sha256"):
    data = _bytes(data)
    kind = _DIGEST[algorithm]
    out = ctypes.create_string_buffer(_digest_size(kind))
    if not _digest_compute(kind, data, len(data), out):
        raise Error("digest failed")
    return out.raw


def hmac(key, data, algorithm="sha256"):
    key, data = _bytes(key), _bytes(data)
    kind = _DIGEST[algorithm]
    out = ctypes.create_string_buffer(_digest_size(kind))
    if not _hmac(kind, key, len(key), data, len(data), out):
        raise Error("hmac failed")
    return out.raw


def random_bytes(count):
    out = ctypes.create_string_buffer(count)
    if not _random_bytes(out, count):
        raise Error("no random bytes")
    return out.raw


def aes_gcm_seal(key, nonce, plaintext, ad=b""):
    key, nonce, plaintext, ad = _bytes(key), _bytes(nonce), _bytes(plaintext), _bytes(ad)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _aes_gcm_seal(key, len(key), nonce, len(nonce), ad or None, len(ad), plaintext, len(plaintext),
                        ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "sealing failed")
    return _buffer(out, size.value)


def aes_gcm_open(key, nonce, sealed, ad=b""):
    key, nonce, sealed, ad = _bytes(key), _bytes(nonce), _bytes(sealed), _bytes(ad)
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _aes_gcm_open(key, len(key), nonce, len(nonce), ad or None, len(ad), sealed, len(sealed),
                        ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "opening failed")
    return _buffer(out, size.value)


def url_part(url, base=None, part="spec"):
    """A part of url, resolved against base, as a browser parses it; None
    when it is not a valid URL."""
    return _string(_url_get(url.encode(), base.encode() if base else None, _URL_PART[part]))


def registrable_domain(host_or_url):
    return _string(_url_registrable_domain(host_or_url.encode()))


def image_decode(data):
    """(width, height, RGBA bytes) of a PNG, JPEG or WebP file."""
    data = _bytes(data)
    width, height, error = ctypes.c_int(), ctypes.c_int(), ctypes.c_void_p()
    out = _image_decode(data, len(data), ctypes.byref(width), ctypes.byref(height), ctypes.byref(error))
    if not out:
        _raise(error, "decoding failed")
    return width.value, height.value, _buffer(out, width.value * height.value * 4)


def image_encode(rgba, width, height, format="png", quality=-1):
    rgba = _bytes(rgba)
    if len(rgba) != width * height * 4:
        raise Error("the pixels must be width * height * 4 bytes of RGBA")
    size, error = ctypes.c_size_t(), ctypes.c_void_p()
    out = _image_encode(_IMAGE[format], rgba, width, height, quality, ctypes.byref(size), ctypes.byref(error))
    if not out:
        _raise(error, "encoding failed")
    return _buffer(out, size.value)


def icu_init():
    return bool(_icu_init())


# ---- PDF: <zebkit/pdf.h> ----

class _RenderOptions(ctypes.Structure):
    _fields_ = [("scale", ctypes.c_float), ("rotation", ctypes.c_int), ("flags", ctypes.c_uint),
                ("background", ctypes.c_uint32), ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int)]


_pdf_open = _declare("zebkit_pdf_open", _P, _P, _Z, _S, _EP)
_pdf_open_file = _declare("zebkit_pdf_open_file", _P, _S, _S, _EP)
_pdf_close = _declare("zebkit_pdf_close", None, _P)
_pdf_save = _declare("zebkit_pdf_save", _P, _P, _U, _ZP, _EP)
_pdf_info = _declare("zebkit_pdf_info", _P, _P)
_pdf_set_metadata = _declare("zebkit_pdf_set_metadata", _B, _P, _S, _EP)
_pdf_page_count = _declare("zebkit_pdf_page_count", _I, _P)
_pdf_page = _declare("zebkit_pdf_page", _P, _P, _I, _EP)
_pdf_render = _declare("zebkit_pdf_render", _P, _P, _I, ctypes.POINTER(_RenderOptions), _IP, _IP, _EP)
_pdf_render_annotation = _declare("zebkit_pdf_render_annotation", _P, _P, _I, _I, _I,
                                  ctypes.POINTER(_RenderOptions), _IP, _IP, _EP)
_pdf_text = _declare("zebkit_pdf_text", _P, _P, _I, _EP)
_pdf_text_layout = _declare("zebkit_pdf_text_layout", _P, _P, _I, _EP)
_pdf_text_rects = _declare("zebkit_pdf_text_rects", _P, _P, _I, _I, _I, _EP)
_pdf_char_at = _declare("zebkit_pdf_char_at", _I, _P, _I, ctypes.c_double, ctypes.c_double, ctypes.c_double)
_pdf_search = _declare("zebkit_pdf_search", _P, _P, _S, _U, _I, _I, _EP)
_pdf_set_outline = _declare("zebkit_pdf_set_outline", _B, _P, _S, _EP)
_pdf_outline = _declare("zebkit_pdf_outline", _P, _P, _EP)
_pdf_links = _declare("zebkit_pdf_links", _P, _P, _I, _EP)
_pdf_annotations = _declare("zebkit_pdf_annotations", _P, _P, _I, _EP)
_pdf_annotation_add = _declare("zebkit_pdf_annotation_add", _I, _P, _I, _S, _EP)
_pdf_annotation_update = _declare("zebkit_pdf_annotation_update", _B, _P, _I, _I, _S, _EP)
_pdf_annotation_remove = _declare("zebkit_pdf_annotation_remove", _B, _P, _I, _I, _EP)
_pdf_flatten = _declare("zebkit_pdf_flatten", _B, _P, _I, _B, _EP)
_pdf_attachments = _declare("zebkit_pdf_attachments", _P, _P, _EP)
_pdf_attachment = _declare("zebkit_pdf_attachment", _P, _P, _I, _ZP, _EP)
_pdf_attach = _declare("zebkit_pdf_attach", _B, _P, _S, _P, _Z, _EP)
_pdf_detach = _declare("zebkit_pdf_detach", _B, _P, _I, _EP)

RENDER_ANNOTATIONS, RENDER_FORMS, RENDER_LCD_TEXT, RENDER_GRAYSCALE, RENDER_PRINTING, RENDER_NO_SMOOTHING = \
    1, 2, 4, 8, 16, 32


class Pdf:
    """A PDF document on the engine's PDFium (<zebkit/pdf.h>), from bytes or
    a path. Structured answers are Python values; coordinates are PDF page
    space. Needs no main(); any thread may use it, one at a time."""

    def __init__(self, source, password=None):
        error = ctypes.c_void_p()
        password = password.encode() if password else None
        if isinstance(source, (bytes, bytearray, memoryview)):
            data = bytes(source)
            self._handle = _pdf_open(data, len(data), password, ctypes.byref(error))
        else:
            self._handle = _pdf_open_file(os.fspath(source).encode(), password, ctypes.byref(error))
        if not self._handle:
            _raise(error, "the document cannot be opened")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()

    def close(self):
        if getattr(self, "_handle", None):
            _pdf_close(self._handle)
            self._handle = None

    def _check(self):
        if not getattr(self, "_handle", None):
            raise Error("the document is closed")
        return self._handle

    def _json(self, function, *args, fallback="failed"):
        error = ctypes.c_void_p()
        out = function(self._check(), *args, ctypes.byref(error))
        if not out:
            _raise(error, fallback)
        return json.loads(_string(out))

    def _ok(self, function, *args, fallback="failed"):
        error = ctypes.c_void_p()
        if not function(self._check(), *args, ctypes.byref(error)):
            _raise(error, fallback)

    def info(self):
        value = _string(_pdf_info(self._check()))
        if value is None:
            raise Error("the document has no information")
        return json.loads(value)

    def set_metadata(self, **entries):
        self._ok(_pdf_set_metadata, json.dumps(entries).encode())

    @property
    def page_count(self):
        return _pdf_page_count(self._check())

    def page(self, index):
        return self._json(_pdf_page, index, fallback="no page")

    def render(self, index, scale=1.0, rotation=0, flags=RENDER_ANNOTATIONS, background=0xFFFFFFFF,
               x=0, y=0, width=0, height=0):
        """(width, height, RGBA bytes) of a page, or of a tile of it."""
        options = _RenderOptions(scale, rotation, flags, background, x, y, width, height)
        out_width, out_height, error = ctypes.c_int(), ctypes.c_int(), ctypes.c_void_p()
        out = _pdf_render(self._check(), index, ctypes.byref(options), ctypes.byref(out_width),
                          ctypes.byref(out_height), ctypes.byref(error))
        if not out:
            _raise(error, "the page cannot be rendered")
        return out_width.value, out_height.value, _buffer(out, out_width.value * out_height.value * 4)

    def render_annotation(self, index, annotation, mode=0, scale=1.0, rotation=0, background=0):
        """(width, height, RGBA bytes) of an annotation's appearance alone (mode 0
        normal, 1 rollover, 2 down), or None when it has no such appearance."""
        options = _RenderOptions(scale, rotation, 0, background, 0, 0, 0, 0)
        out_width, out_height, error = ctypes.c_int(), ctypes.c_int(), ctypes.c_void_p()
        out = _pdf_render_annotation(self._check(), index, annotation, mode, ctypes.byref(options),
                                     ctypes.byref(out_width), ctypes.byref(out_height), ctypes.byref(error))
        if not out:
            message = _string(error.value) if error.value else "the annotation cannot be rendered"
            if message == "no appearance":
                return None
            raise Error(message)
        return out_width.value, out_height.value, _buffer(out, out_width.value * out_height.value * 4)

    def text(self, index):
        error = ctypes.c_void_p()
        out = _pdf_text(self._check(), index, ctypes.byref(error))
        if not out:
            _raise(error, "no text")
        return _string(out)

    def text_layout(self, index):
        return self._json(_pdf_text_layout, index)

    def text_rects(self, index, start, count):
        return self._json(_pdf_text_rects, index, start, count)

    def char_at(self, index, x, y, tolerance=2.0):
        found = _pdf_char_at(self._check(), index, x, y, tolerance)
        return None if found < 0 else found

    def search(self, query, match_case=False, whole_word=False, first=0, limit=0):
        flags = (1 if match_case else 0) | (2 if whole_word else 0)
        return self._json(_pdf_search, query.encode(), flags, first, limit)

    def set_outline(self, items):
        """Replace the bookmark tree; [] removes it. Validated before mutation."""
        self._ok(_pdf_set_outline, json.dumps(items).encode())

    def outline(self):
        return self._json(_pdf_outline)

    def links(self, index):
        return self._json(_pdf_links, index)

    def annotations(self, index):
        return self._json(_pdf_annotations, index)

    def add_annotation(self, index, annotation):
        error = ctypes.c_void_p()
        added = _pdf_annotation_add(self._check(), index, json.dumps(annotation).encode(), ctypes.byref(error))
        if added < 0:
            _raise(error, "the annotation cannot be made")
        return added

    def update_annotation(self, index, annotation, changes):
        self._ok(_pdf_annotation_update, index, annotation, json.dumps(changes).encode())

    def remove_annotation(self, index, annotation):
        self._ok(_pdf_annotation_remove, index, annotation)

    def flatten(self, index, for_print=False):
        self._ok(_pdf_flatten, index, for_print)

    def attachments(self):
        return self._json(_pdf_attachments)

    def attachment(self, index):
        size, error = ctypes.c_size_t(), ctypes.c_void_p()
        out = _pdf_attachment(self._check(), index, ctypes.byref(size), ctypes.byref(error))
        if not out:
            _raise(error, "no attachment")
        return _buffer(out, size.value)

    def attach(self, name, data):
        data = _bytes(data)
        self._ok(_pdf_attach, name.encode(), data, len(data))

    def detach(self, index):
        self._ok(_pdf_detach, index)

    def save(self, incremental=False, no_security=False):
        size, error = ctypes.c_size_t(), ctypes.c_void_p()
        out = _pdf_save(self._check(), (1 if incremental else 0) | (2 if no_security else 0), ctypes.byref(size),
                        ctypes.byref(error))
        if not out:
            _raise(error, "the document cannot be saved")
        return _buffer(out, size.value)


# ---- URL schemes of the application's own ----

SCHEME_STANDARD, SCHEME_SECURE, SCHEME_FETCH, SCHEME_BYPASS_CSP = 1, 2, 4, 8

_SCHEME = ctypes.CFUNCTYPE(None, _P, _P)
_scheme_register = _declare("zebkit_scheme_register", _B, _S, _U)
_scheme_handle = _declare("zebkit_scheme_handle", None, _S, _SCHEME, _P)
_request_url = _declare("zebkit_request_url", _S, _P)
_request_method = _declare("zebkit_request_method", _S, _P)
_request_headers = _declare("zebkit_request_headers", _S, _P)
_request_body = _declare("zebkit_request_body", ctypes.POINTER(ctypes.c_uint8), _P, _ZP)
_request_respond = _declare("zebkit_request_respond", None, _P, _I, _S, _P, _Z)
_request_respond_file = _declare("zebkit_request_respond_file", None, _P, _S, _S)


class Request:
    """A request for a URL of the application's scheme. Answer it once, with
    respond() or respond_file(), from any thread."""

    def __init__(self, handle):
        self._handle = handle
        self.url = _text(_request_url(handle))
        self.method = _text(_request_method(handle))
        self.headers = {}
        for line in _text(_request_headers(handle)).split("\r\n"):
            name, _, value = line.partition(":")
            if name:
                self.headers[name.strip()] = value.strip()
        size = ctypes.c_size_t()
        body = _request_body(handle, ctypes.byref(size))
        self.body = ctypes.string_at(body, size.value) if body else None

    def _take(self):
        handle, self._handle = self._handle, None
        if not handle:
            raise Error("the request is answered already")
        return handle

    def __del__(self):
        # Nobody answered it, and nobody can now: not found, so that the page
        # does not wait for good.
        if getattr(self, "_handle", None):
            _request_respond(self._take(), 404, None, b"", 0)

    def respond(self, status=200, body=b"", headers=None):
        body = _bytes(body)
        lines = "\r\n".join(f"{k}: {v}" for k, v in (headers or {}).items())
        _request_respond(self._take(), status, lines.encode(), body, len(body))

    def respond_file(self, path, headers=None):
        lines = "\r\n".join(f"{k}: {v}" for k, v in (headers or {}).items())
        _request_respond_file(self._take(), os.fspath(path).encode(), lines.encode())


def scheme_register(scheme, flags=SCHEME_STANDARD | SCHEME_SECURE | SCHEME_FETCH):
    """Makes scheme one of the application's: before main()."""
    return bool(_scheme_register(scheme.encode(), flags))


_scheme_callbacks = {}


def scheme_handle(scheme, function):
    """function(Request) answers the scheme's requests, on the engine's thread;
    None stops. A request function raises on, unanswered, is answered 500; one
    dropped unanswered, 404."""
    _require_thread("scheme_handle()")

    def handle(pointer, data):
        request = Request(pointer)
        try:
            function(request)
        except Exception:  # noqa: BLE001
            if request._handle:
                request.respond(500, b"the application's handler failed", {"Content-Type": "text/plain"})
            raise
    callback = _SCHEME(_guard(handle)) if function is not None else _SCHEME()
    _scheme_handle(scheme.encode(), callback, None)
    _retire(_scheme_callbacks.pop(scheme, None))
    if function is not None:
        _scheme_callbacks[scheme] = callback


# ---- The application ---------------------------------------------------

_MENU = ctypes.CFUNCTYPE(None, ctypes.c_char_p, _P)
_PATHS = ctypes.CFUNCTYPE(None, ctypes.c_char_p, _P)
_APP = ctypes.CFUNCTYPE(None, _P)
_app_set_menu = _declare("zebkit_app_set_menu", _B, _S, _MENU, _P, _EP)
_app_update_menu_item = _declare("zebkit_app_update_menu_item", _B, _S, _S, _EP)
_app_get_menu = _declare("zebkit_app_get_menu", _P)
_app_on_open_files = _declare("zebkit_app_on_open_files", None, _PATHS, _P)
_app_on_reopen = _declare("zebkit_app_on_reopen", None, _APP, _P)
_app_on_quit_request = _declare("zebkit_app_on_quit_request", None, _APP, _P)
_app_set_badge = _declare("zebkit_app_set_badge", _B, _S)
_app_set_progress = _declare("zebkit_app_set_progress", _B, ctypes.c_double)
_app_request_attention = _declare("zebkit_app_request_attention", None, _B)
_app_set_dock_menu = _declare("zebkit_app_set_dock_menu", _B, _S, _MENU, _P, _EP)
# The application's callbacks, one of each, as the engine holds them.
_app_callbacks = {}


def app_set_badge(text):
    """The badge on the application's icon in the Dock or on the taskbar; a
    number on Linux's launcher docks; "" or None for none."""
    _require_thread("app_set_badge()")
    return bool(_app_set_badge((text or "").encode()))


def app_set_progress(progress):
    """A progress from 0 to 1 on the application's icon; None (or below 0)
    for none."""
    _require_thread("app_set_progress()")
    return bool(_app_set_progress(-1.0 if progress is None else float(progress)))


def app_request_attention(critical=False):
    """The Dock's bounce (critical: until the application is active) or the
    taskbar button's flash; urgent on Linux's launcher docks."""
    _require_thread("app_request_attention()")
    _app_request_attention(bool(critical))


def app_set_dock_menu(items, function):
    """The Dock's menu (macOS): items as app_set_menu's; function(id) for one
    chosen. Raises elsewhere."""
    _require_thread("app_set_dock_menu()")
    text = items if isinstance(items, str) else json.dumps(items)
    callback = _MENU(_guard(lambda item, data: function(_text(item))))
    error = ctypes.c_void_p()
    if not _app_set_dock_menu(text.encode(), callback, None, ctypes.byref(error)):
        _raise(error, "the Dock's menu was refused")
    _retire(_app_callbacks.get("dock"))
    _app_callbacks["dock"] = callback


def app_set_menu(menu, function):
    """The application's menu: a list of {"title", "items"} (or its JSON),
    where an item is {"title", "id", "key", "modifiers"}, {"role"},
    {"title", "items"} or {"separator": True}; function(id) is called when
    an item with an id is chosen. The menu bar on macOS; elsewhere the
    items' keys work in the application's windows. On the engine's thread."""
    _require_thread("app_set_menu()")
    text = menu if isinstance(menu, str) else json.dumps(menu)
    callback = _MENU(_guard(lambda item, data: function(_text(item))))
    error = ctypes.c_void_p()
    if not _app_set_menu(text.encode(), callback, None, ctypes.byref(error)):
        _raise(error, "the menu was refused")
    # The one replaced may be running: a menu item that sets a new menu.
    _retire(_app_callbacks.get("menu"))
    _app_callbacks["menu"] = callback


def app_update_menu_item(item_id, **state):
    """Changes the menu item item_id without a new menu: enabled, checked,
    title; checking one of a radioGroup unchecks the others."""
    _require_thread("app_update_menu_item()")
    error = ctypes.c_void_p()
    if not _app_update_menu_item(item_id.encode(), json.dumps(state).encode(), ctypes.byref(error)):
        _raise(error, "the menu item was not changed")


def app_menu():
    """The menu with its items' state."""
    _require_thread("app_menu()")
    value = _string(_app_get_menu())
    return json.loads(value) if value else []


def _app_on(kind, setter, callback):
    _require_thread()
    setter(callback, None)
    _retire(_app_callbacks.get(kind))
    _app_callbacks[kind] = callback


def app_on_open_files(function):
    """function(paths) for files the system asks the running application to
    open (macOS: the Finder and the Dock); elsewhere they are arguments.
    None stops."""
    _app_on("open", _app_on_open_files, _PATHS(_guard(lambda paths, data: function(json.loads(_text(paths, "[]")))))
            if function is not None else _PATHS())


def app_on_reopen(function):
    """function() when the application's icon in the Dock is clicked with no
    window shown; None stops."""
    _app_on("reopen", _app_on_reopen, _APP(_guard(lambda data: function())) if function is not None else _APP())


def app_on_quit_request(function):
    """function() when the user asks the application to quit; it calls quit()
    or not. None: zebkit quits."""
    _app_on("quit", _app_on_quit_request, _APP(_guard(lambda data: function())) if function is not None else _APP())


def set_cookie_encryption_key(key):
    """Before run(): host-persisted 32-byte random key, or None for the OS store.

    Never log the key. Host cookies use a separate database.
    """
    if key is None:
        return bool(_cookie_key(None, 0))
    key = bytes(key)
    if len(key) != 32:
        raise ValueError("cookie key must contain exactly 32 bytes")
    buffer = ctypes.create_string_buffer(key)
    return bool(_cookie_key(buffer, len(key)))


def set_cookie_storage(storage="os", key_directory=None):
    """Select Zebkit's built-in 'os' or 'file' backend before main().

    File mode defaults to <profile>/Secrets and needs no host key-file code.
    Startup fails if the key is lost/corrupt. The file key relies on OS file
    permissions, with less protection than the system key store.
    """
    if storage not in ("os", "file"):
        raise ValueError("cookie storage must be 'os' or 'file'")
    directory = None if key_directory is None else os.fsencode(key_directory)
    if directory is not None and b"\0" in directory:
        raise ValueError("key directory contains a zero byte")
    return bool(_cookie_storage(0 if storage == "os" else 1, directory))


# ---- Canvas and text: <zebkit/canvas.h>, <zebkit/text.h> ----

_canvas_new = _declare("zebkit_canvas_new", _P, _I, _I, _EP)
_canvas_wrap = _declare("zebkit_canvas_wrap", _P, _P, _I, _I, _Z, _U, _EP)
_canvas_close = _declare("zebkit_canvas_close", None, _P)
_canvas_width = _declare("zebkit_canvas_width", _I, _P)
_canvas_height = _declare("zebkit_canvas_height", _I, _P)
_canvas_pixels = _declare("zebkit_canvas_pixels", _P, _P, _EP)
_canvas_png = _declare("zebkit_canvas_png", _P, _P, _ZP, _EP)
_canvas_run = _declare("zebkit_canvas_run", _B, _P, _S, _EP)
_font_new = _declare("zebkit_canvas_font_new", _P, _P, _Z, _EP)
_font_close = _declare("zebkit_canvas_font_close", None, _P)
_canvas_set_font = _declare("zebkit_canvas_set_font", None, _P, _P, ctypes.c_double)
_canvas_fill_text = _declare("zebkit_canvas_fill_text", _B, _P, _S, ctypes.c_double, ctypes.c_double, _EP)
_canvas_text_path = _declare("zebkit_canvas_text_path", _B, _P, _S, ctypes.c_double, ctypes.c_double, _EP)
_canvas_measure_text = _declare("zebkit_canvas_measure_text", ctypes.c_double, _P, _S)
_canvas_draw_image = _declare("zebkit_canvas_draw_image", _B, _P, _P, _I, _I, *([ctypes.c_double] * 4))
_canvas_set_blend = _declare("zebkit_canvas_set_blend", _B, _P, _S)
_canvas_set_line_cap = _declare("zebkit_canvas_set_line_cap", None, _P, _I)
_canvas_set_line_join = _declare("zebkit_canvas_set_line_join", None, _P, _I)
_canvas_arc = _declare("zebkit_canvas_arc", None, _P, *([ctypes.c_double] * 5), _B)
_canvas_fill = _declare("zebkit_canvas_fill", None, _P, _B)
_canvas_clip = _declare("zebkit_canvas_clip", None, _P, _B)
_canvas_set_dash = _declare("zebkit_canvas_set_dash", None, _P, ctypes.POINTER(ctypes.c_double), _I, ctypes.c_double)
_canvas_linear = _declare("zebkit_canvas_set_linear_gradient", _B, _P, *([ctypes.c_double] * 4),
                          ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_uint32), _I)
_canvas_radial = _declare("zebkit_canvas_set_radial_gradient", _B, _P, *([ctypes.c_double] * 3),
                          ctypes.POINTER(ctypes.c_double), ctypes.POINTER(ctypes.c_uint32), _I)
_text_shape = _declare("zebkit_text_shape", _P, _P, _Z, ctypes.c_double, _S, _S, _S, _EP)


def _numbers_call(name, count):
    function = _declare(name, None, _P, *([ctypes.c_double] * count))

    def call(self, *numbers):
        function(self._handle, *numbers)
    return call


def _simple_call(name):
    function = _declare(name, None, _P)

    def call(self):
        function(self._handle)
    return call


def _color_call(name):
    function = _declare(name, None, _P, ctypes.c_uint32)

    def call(self, rgba):
        function(self._handle, rgba)
    return call


class Font:
    """A font from its bytes, for Canvas.set_font()."""

    def __init__(self, data):
        error = ctypes.c_void_p()
        data = bytes(data)
        self._handle = _font_new(data, len(data), ctypes.byref(error))
        if not self._handle:
            _raise(error, "not a font")

    def close(self):
        if getattr(self, "_handle", None):
            _font_close(self._handle)
            self._handle = None

    def __del__(self):
        self.close()


class Canvas:
    """2D drawing on the engine's Skia: a canvas of width by height pixels, or
    over a bytearray of RGBA with Canvas.over(). Colours are 0xRRGGBBAA."""

    def __init__(self, width=None, height=None, _handle=None):
        if _handle is not None:
            self._handle, self._owned = _handle, False
            return
        error = ctypes.c_void_p()
        self._owned = True
        self._handle = _canvas_new(width, height, ctypes.byref(error))
        if not self._handle:
            _raise(error, "the canvas cannot be made")

    @classmethod
    def over(cls, pixels, width, height, stride=0, unpremultiplied=False):
        """Over a bytearray, R, G, B, A, kept until close()."""
        if not isinstance(pixels, bytearray):
            raise TypeError("pixels are a bytearray")
        if stride < 0 or len(pixels) < (stride or width * 4) * height:
            raise ValueError("too few pixels")
        self = cls.__new__(cls)
        buffer = (ctypes.c_uint8 * len(pixels)).from_buffer(pixels)
        error = ctypes.c_void_p()
        self._buffer, self._owned = buffer, True
        self._handle = _canvas_wrap(ctypes.addressof(buffer), width, height, stride, 1 if unpremultiplied else 0,
                                    ctypes.byref(error))
        if not self._handle:
            _raise(error, "the canvas cannot be made")
        return self

    def close(self):
        if getattr(self, "_handle", None) and self._owned:
            _canvas_close(self._handle)
        self._handle = None
        self._buffer = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        self.close()

    @property
    def width(self):
        return _canvas_width(self._handle)

    @property
    def height(self):
        return _canvas_height(self._handle)

    def pixels(self):
        """RGBA, not premultiplied, width * height * 4 bytes."""
        error = ctypes.c_void_p()
        out = _canvas_pixels(self._handle, ctypes.byref(error))
        if not out:
            _raise(error, "this canvas has no pixels")
        return _buffer(out, self.width * self.height * 4)

    def png(self):
        size, error = ctypes.c_size_t(), ctypes.c_void_p()
        out = _canvas_png(self._handle, ctypes.byref(size), ctypes.byref(error))
        if not out:
            _raise(error, "the PNG cannot be made")
        return _buffer(out, size.value)

    save = _simple_call("zebkit_canvas_save")
    restore = _simple_call("zebkit_canvas_restore")
    translate = _numbers_call("zebkit_canvas_translate", 2)
    scale = _numbers_call("zebkit_canvas_scale", 2)
    rotate = _numbers_call("zebkit_canvas_rotate", 1)
    transform = _numbers_call("zebkit_canvas_transform", 6)
    set_matrix = _numbers_call("zebkit_canvas_set_matrix", 6)
    set_fill_color = _color_call("zebkit_canvas_set_fill_color")
    set_stroke_color = _color_call("zebkit_canvas_set_stroke_color")
    set_alpha = _numbers_call("zebkit_canvas_set_alpha", 1)
    set_line_width = _numbers_call("zebkit_canvas_set_line_width", 1)
    set_miter_limit = _numbers_call("zebkit_canvas_set_miter_limit", 1)
    begin_path = _simple_call("zebkit_canvas_begin_path")
    move_to = _numbers_call("zebkit_canvas_move_to", 2)
    line_to = _numbers_call("zebkit_canvas_line_to", 2)
    quad_to = _numbers_call("zebkit_canvas_quad_to", 4)
    curve_to = _numbers_call("zebkit_canvas_curve_to", 6)
    rect = _numbers_call("zebkit_canvas_rect", 4)
    close_path = _simple_call("zebkit_canvas_close_path")
    stroke = _simple_call("zebkit_canvas_stroke")
    clear = _color_call("zebkit_canvas_clear")

    def set_line_cap(self, cap):
        _canvas_set_line_cap(self._handle, cap)

    def set_line_join(self, join):
        _canvas_set_line_join(self._handle, join)

    def set_blend(self, mode):
        return _canvas_set_blend(self._handle, mode.encode())

    def set_dash(self, lengths, phase=0.0):
        values = (ctypes.c_double * len(lengths))(*lengths)
        _canvas_set_dash(self._handle, values, len(lengths), phase)

    def set_linear_gradient(self, x0, y0, x1, y1, offsets, colors):
        n = len(offsets)
        return n == len(colors) and _canvas_linear(self._handle, x0, y0, x1, y1, (ctypes.c_double * n)(*offsets),
                                                   (ctypes.c_uint32 * n)(*colors), n)

    def set_radial_gradient(self, x, y, radius, offsets, colors):
        n = len(offsets)
        return n == len(colors) and _canvas_radial(self._handle, x, y, radius, (ctypes.c_double * n)(*offsets),
                                                   (ctypes.c_uint32 * n)(*colors), n)

    def arc(self, x, y, radius, start, end, counterclockwise=False):
        _canvas_arc(self._handle, x, y, radius, start, end, counterclockwise)

    def fill(self, even_odd=False):
        _canvas_fill(self._handle, even_odd)

    def clip(self, even_odd=False):
        _canvas_clip(self._handle, even_odd)

    def draw_image(self, rgba, width, height, x, y, dest_width=None, dest_height=None):
        if not isinstance(rgba, bytes):
            # bytes are passed as they are; others (a bytearray, a memoryview) copied once.
            rgba = bytes(rgba)
        if len(rgba) != width * height * 4:
            return False
        return _canvas_draw_image(self._handle, rgba, width, height, x, y,
                                  width if dest_width is None else dest_width,
                                  height if dest_height is None else dest_height)

    def set_font(self, font, size=12.0):
        _canvas_set_font(self._handle, font._handle if font else None, size)
        self._font = font

    def fill_text(self, text, x, y):
        error = ctypes.c_void_p()
        if not _canvas_fill_text(self._handle, text.encode(), x, y, ctypes.byref(error)):
            _raise(error, "the text cannot be drawn")

    def text_path(self, text, x, y):
        error = ctypes.c_void_p()
        if not _canvas_text_path(self._handle, text.encode(), x, y, ctypes.byref(error)):
            _raise(error, "the text cannot be drawn")

    def measure_text(self, text):
        width = _canvas_measure_text(self._handle, text.encode())
        return None if width < 0 else width

    def run(self, operations):
        """A list of operations, as <zebkit/canvas.h> says."""
        error = ctypes.c_void_p()
        if not _canvas_run(self._handle, json.dumps(operations).encode(), ctypes.byref(error)):
            _raise(error, "the operations cannot be run")


def shape_text(font, size, text, language=None, direction=None):
    """Shapes a line of text on the engine's HarfBuzz: {direction, script,
    unitsPerEm, advance, glyphs: [{glyph, cluster, advance, offset}]}."""
    font = bytes(font)
    error = ctypes.c_void_p()
    out = _text_shape(font, len(font), size, text.encode(), language.encode() if language else None,
                      direction.encode() if direction else None, ctypes.byref(error))
    if not out:
        _raise(error, "the text cannot be shaped")
    return json.loads(_string(out))


# ---- PDF, more annotations and hooks ----

_D = ctypes.c_double
_pdf_string = _declare("zebkit_pdf_string", _P, _S)
_pdf_bytes = _declare("zebkit_pdf_bytes", _P, _P, _Z)
_pdf_hook_remove = _declare("zebkit_pdf_hook_remove", _B, _I)
_pdf_annotation_at = _declare("zebkit_pdf_annotation_at", _I, _P, _I, _D, _D, _D, _EP)
_pdf_annotation_move = _declare("zebkit_pdf_annotation_move", _B, _P, _I, _I, _I, _EP)
_pdf_annotation_copy = _declare("zebkit_pdf_annotation_copy", _I, _P, _I, _I, _P, _I, _EP)
_pdf_annotation_flatten = _declare("zebkit_pdf_annotation_flatten", _B, _P, _I, _I, _EP)
_pdf_annotation_stamp_page = _declare("zebkit_pdf_annotation_stamp_page", _B, _P, _I, _I, _P, _I, _EP)
_pdf_annotation_attach_file = _declare("zebkit_pdf_annotation_attach_file", _B, _P, _I, _I, _S, _P, _Z, _EP)
_pdf_annotation_file = _declare("zebkit_pdf_annotation_file", _P, _P, _I, _I, _EP, _ZP, _EP)
_pdf_annotation_draw = _declare("zebkit_pdf_annotation_draw", _B, _P, _I, _I, _S, _EP)
_pdf_annotation_action = _declare("zebkit_pdf_annotation_action", _P, _P, _I, _I, _EP)
_pdf_annotations_export = _declare("zebkit_pdf_annotations_export", _P, _P, _I, _I, _I, _ZP, _EP)
_pdf_annotations_import = _declare("zebkit_pdf_annotations_import", _I, _P, _I, _P, _Z, _EP)
_pdf_apply = _declare("zebkit_pdf_apply", _B, _P, _S, _EP)
_pdf_page_data = _declare("zebkit_pdf_page_data", _P, _P, _I, _S, _EP)
_pdf_font_add = _declare("zebkit_pdf_font_add", _B, _P, _S, _P, _Z, _EP)
_pdf_filter_input = _declare("zebkit_pdf_filter_input", _P, _P, _S, _S, _S, _EP)

_HOOK_DESTROY_T = ctypes.CFUNCTYPE(None, ctypes.c_void_p)
_BEFORE = ctypes.CFUNCTYPE(_I, _P, _S, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p), _P)
_AFTER = ctypes.CFUNCTYPE(None, _P, _S, _P)
_APPEARANCE = ctypes.CFUNCTYPE(_B, _P, _P, _I, _I, _S, _D, _D, _P)
_CHECK = ctypes.CFUNCTYPE(_P, _S, _P)
_HIT = ctypes.CFUNCTYPE(_B, _P, _I, _I, _S, _D, _D, _P)


class _LayerInfo(ctypes.Structure):
    _fields_ = [("width", _I), ("height", _I), ("scale", _D), ("rotation", _I), ("matrix", _D * 6)]


_LAYER = ctypes.CFUNCTYPE(None, _P, _P, _I, ctypes.POINTER(_LayerInfo), _P)
_OCR = ctypes.CFUNCTYPE(_P, _P, _I, ctypes.POINTER(ctypes.c_uint8), _I, _I, _D, _P)
_FONT = ctypes.CFUNCTYPE(_P, _P, _S, _ZP, _P)
_TRUST = ctypes.CFUNCTYPE(_I, _P, _S, ctypes.POINTER(ctypes.c_uint8), _Z, _P)
_ACTION = ctypes.CFUNCTYPE(_B, _P, _S, _P)
_INPUT = ctypes.CFUNCTYPE(_P, _P, _S, _S, _P)


class _Kind(ctypes.Structure):
    _fields_ = [("name", _S), ("prefix", _S), ("subtype", _S), ("schema", _S), ("check", _CHECK),
                ("draw", _APPEARANCE), ("hit_test", _HIT), ("data", _P), ("destroy", _HOOK_DESTROY_T)]


_register_before = _declare("zebkit_pdf_hook_before", _I, _P, _S, _BEFORE, _P, _HOOK_DESTROY_T)
_register_after = _declare("zebkit_pdf_hook_after", _I, _P, _S, _AFTER, _P, _HOOK_DESTROY_T)
_register_appearance = _declare("zebkit_pdf_hook_appearance", _I, _P, _S, _APPEARANCE, _P, _HOOK_DESTROY_T)
_register_kind = _declare("zebkit_pdf_hook_kind", _I, _P, ctypes.POINTER(_Kind), _EP)
_register_layer = _declare("zebkit_pdf_hook_layer", _I, _P, _I, _LAYER, _P, _HOOK_DESTROY_T)
_register_ocr = _declare("zebkit_pdf_hook_ocr", _I, _P, _OCR, _P, _HOOK_DESTROY_T)
_register_font = _declare("zebkit_pdf_hook_font", _I, _P, _FONT, _P, _HOOK_DESTROY_T)
_register_trust = _declare("zebkit_pdf_hook_trust", _I, _P, _TRUST, _P, _HOOK_DESTROY_T)
_register_action = _declare("zebkit_pdf_hook_action", _I, _P, _ACTION, _P, _HOOK_DESTROY_T)
_register_input = _declare("zebkit_pdf_hook_input", _I, _P, _S, _INPUT, _P, _HOOK_DESTROY_T)

# What C holds of a hook lives here until the engine says it is done with it,
# never while a call of it runs.
_hook_keep = {}
_hook_next = [1]


@_HOOK_DESTROY_T
def _hook_destroy(key):
    _hook_keep.pop(key, None)


def _keep(*things):
    key = _hook_next[0]
    _hook_next[0] += 1
    _hook_keep[key] = things
    return key


def _give(text):
    # What a hook says must get to the engine, whatever it holds.
    return _pdf_string(text.encode("utf-8", "replace"))


def _lent(handle, draw):
    """Runs draw(canvas) over a canvas that is the engine's for this call: one
    the function keeps afterwards draws on nothing, not on what has gone."""
    canvas = Canvas(_handle=handle)
    try:
        return draw(canvas)
    finally:
        canvas._handle = None


class PdfHooks:
    """Hooks of <zebkit/pdf.h> for one document (Pdf.hooks), or with no document
    for all (zebkit.pdf_hooks). Each method returns the hook's id for
    remove_pdf_hook(). Operations and annotations are Python values."""

    def __init__(self, pdf=None):
        self._pdf = pdf

    @property
    def _handle(self):
        # A closed document's hook must not become one for all documents.
        if self._pdf is not None and not self._pdf._handle:
            raise Error("the document is closed")
        return self._pdf._handle if self._pdf is not None else None

    def before(self, event, function):
        """function(operation): None goes on, False or {"refuse": why} refuses,
        another operation changes what happens. An exception refuses."""
        def call(pdf, operation, change, reason, data):
            # All of it in here: what leaves this function as an exception
            # comes to C as 0, which is "go on".
            try:
                answer = function(json.loads(operation))
                if answer is False or (isinstance(answer, dict) and "refuse" in answer):
                    why = answer["refuse"] if answer is not False else None
                    if why:
                        reason[0] = _give(str(why))
                    return 1
                if isinstance(answer, dict):
                    change[0] = _give(json.dumps(answer))
                return 0
            except BaseException as e:  # noqa: BLE001
                reason[0] = _give(f"a hook failed: {e}")
                return 1
        thunk = _BEFORE(call)
        return _register_before(self._handle, event.encode(), thunk, _keep(thunk), _hook_destroy)

    def after(self, event, function):
        """function(operation), with "inverse" when it can be undone."""
        thunk = _AFTER(_guard(lambda pdf, operation, data: function(json.loads(operation))))
        return _register_after(self._handle, event.encode(), thunk, _keep(thunk), _hook_destroy)

    def appearance(self, type_name, function):
        """function(canvas, annotation, width, height) -> True when drawn. The
        canvas writes PDF operators: units are points, the origin its top left."""
        def call(canvas, pdf, page, index, annotation, width, height, data):
            try:
                return bool(_lent(canvas, lambda lent: function(lent, json.loads(annotation), width, height)))
            except Exception:  # noqa: BLE001
                return False
        thunk = _APPEARANCE(call)
        return _register_appearance(self._handle, type_name.encode(), thunk, _keep(thunk), _hook_destroy)

    def kind(self, name, prefix, subtype=None, schema=None, check=None, draw=None, hit_test=None):
        """A kind of annotation of the application's own."""
        def draw_call(canvas, pdf, page, index, annotation, width, height, data):
            try:
                return bool(_lent(canvas, lambda lent: draw(lent, json.loads(annotation), width, height)))
            except Exception:  # noqa: BLE001
                return False

        def check_call(data_json, data):
            # As for before(): nothing back is "the data is fine".
            try:
                problem = check(json.loads(data_json))
                return _give(str(problem)) if problem else None
            except BaseException as e:  # noqa: BLE001
                return _give(f"a check failed: {e}")

        def hit_call(pdf, page, index, annotation, x, y, data):
            try:
                return bool(hit_test(json.loads(annotation), x, y))
            except Exception:  # noqa: BLE001
                return False
        thunks = (_CHECK(check_call) if check else _CHECK(), _APPEARANCE(draw_call) if draw else _APPEARANCE(),
                  _HIT(hit_call) if hit_test else _HIT())
        kept = _keep(*thunks)
        raw = _Kind(name.encode(), prefix.encode(), subtype.encode() if subtype else None,
                    json.dumps(schema).encode() if schema is not None else None, thunks[0], thunks[1], thunks[2],
                    kept, _hook_destroy)
        error = ctypes.c_void_p()
        hook = _register_kind(self._handle, ctypes.byref(raw), ctypes.byref(error))
        if hook < 0:
            _hook_keep.pop(kept, None)  # what fails to register is not destroyed
            _raise(error, "the kind cannot be registered")
        return hook

    def layer(self, function, above=True):
        """function(canvas, page, info): over the page's bitmap, in page space."""
        def call(canvas, pdf, page, info, data):
            i = info.contents
            _lent(canvas, lambda lent: function(lent, page, {"width": i.width, "height": i.height, "scale": i.scale,
                                                             "rotation": i.rotation, "matrix": list(i.matrix)}))
        thunk = _LAYER(_guard(call))
        return _register_layer(self._handle, 1 if above else 0, thunk, _keep(thunk), _hook_destroy)

    def ocr(self, function):
        """function(page, width, height, rgba, scale) -> [{"text", "box"}] or None."""
        def call(pdf, page, rgba, width, height, scale, data):
            try:
                lines = function(page, width, height, ctypes.string_at(rgba, width * height * 4), scale)
            except Exception:  # noqa: BLE001
                return None
            return _give(json.dumps(lines)) if lines else None
        thunk = _OCR(call)
        return _register_ocr(self._handle, thunk, _keep(thunk), _hook_destroy)

    def font(self, function):
        """function(family) -> a font's bytes, or None. "" is for what the standard fonts lack."""
        def call(pdf, family, size, data):
            try:
                bytes_ = function(family.decode())
            except Exception:  # noqa: BLE001
                return None
            if not bytes_:
                return None
            size[0] = len(bytes_)
            return _pdf_bytes(bytes_, len(bytes_))
        thunk = _FONT(call)
        return _register_font(self._handle, thunk, _keep(thunk), _hook_destroy)

    def trust(self, function):
        """function(signature, pkcs7) -> True, False or None."""
        def call(pdf, signature, contents, size, data):
            try:
                answer = function(json.loads(signature), ctypes.string_at(contents, size) if contents else b"")
            except Exception:  # noqa: BLE001
                return 0
            return 1 if answer is True else -1 if answer is False else 0
        thunk = _TRUST(call)
        return _register_trust(self._handle, thunk, _keep(thunk), _hook_destroy)

    def action(self, function):
        """function(action) -> True when the application has done it."""
        def call(pdf, action, data):
            try:
                return bool(function(json.loads(action)))
            except Exception:  # noqa: BLE001
                return False
        thunk = _ACTION(call)
        return _register_action(self._handle, thunk, _keep(thunk), _hook_destroy)

    def input(self, tool, function):
        """function(points) -> new points, or None for no change."""
        def call(pdf, tool_name, points, data):
            try:
                changed = function(json.loads(points))
            except Exception:  # noqa: BLE001
                return None
            return _give(json.dumps(changed)) if changed is not None else None
        thunk = _INPUT(call)
        return _register_input(self._handle, tool.encode(), thunk, _keep(thunk), _hook_destroy)


pdf_hooks = PdfHooks()


def remove_pdf_hook(hook):
    return _pdf_hook_remove(hook)


def pdf_filter_input(points, options=None):
    """The built-in filters of a drawing tool's points, with no document."""
    error = ctypes.c_void_p()
    out = _pdf_filter_input(None, None, json.dumps(points).encode(),
                            json.dumps(options).encode() if options is not None else None, ctypes.byref(error))
    if not out:
        _raise(error, "the points cannot be filtered")
    return json.loads(_string(out))


def _pdf_more(cls):
    """What Pdf has beyond the first part of this file."""
    def annotation_at(self, index, x, y, tolerance=2.0):
        """The top annotation at a point, or None."""
        error = ctypes.c_void_p()
        at = _pdf_annotation_at(self._check(), index, x, y, tolerance, ctypes.byref(error))
        if at < 0 and error.value:
            _raise(error, "no page")
        return None if at < 0 else at

    def move_annotation(self, index, annotation, to):
        self._ok(_pdf_annotation_move, index, annotation, to, fallback="the annotation cannot be moved")

    def copy_annotation(self, index, annotation, target=None, target_page=0):
        error = ctypes.c_void_p()
        target_handle = (target or self)._handle
        at = _pdf_annotation_copy(self._check(), index, annotation, target_handle, target_page, ctypes.byref(error))
        if at < 0:
            _raise(error, "the annotation cannot be copied")
        return at

    def flatten_annotation(self, index, annotation):
        self._ok(_pdf_annotation_flatten, index, annotation, fallback="the annotation cannot be flattened")

    def stamp_from_page(self, index, annotation, source, source_page=0):
        self._ok(_pdf_annotation_stamp_page, index, annotation, source._handle, source_page,
                 fallback="the page cannot be made a stamp")

    def attach_file(self, index, annotation, name, data):
        data = bytes(data)
        self._ok(_pdf_annotation_attach_file, index, annotation, name.encode(), data, len(data),
                 fallback="the file cannot be attached")

    def annotation_file(self, index, annotation):
        """(name, bytes) of a file attachment annotation's file."""
        name, size, error = ctypes.c_void_p(), ctypes.c_size_t(), ctypes.c_void_p()
        out = _pdf_annotation_file(self._check(), index, annotation, ctypes.byref(name), ctypes.byref(size),
                                   ctypes.byref(error))
        if not out:
            _raise(error, "the annotation has no file")
        return _string(name.value), _buffer(out, size.value)

    def draw_annotation(self, index, annotation, operations):
        """A list of canvas operations as the annotation's appearance."""
        self._ok(_pdf_annotation_draw, index, annotation, json.dumps(operations).encode(),
                 fallback="the appearance cannot be drawn")

    def annotation_action(self, index, annotation):
        return self._json(_pdf_annotation_action, index, annotation, fallback="no annotation")

    def export_annotations(self, format="xfdf", page=-1, index=-1):
        size, error = ctypes.c_size_t(), ctypes.c_void_p()
        out = _pdf_annotations_export(self._check(), 1 if format == "fdf" else 0, page, index, ctypes.byref(size),
                                      ctypes.byref(error))
        if not out:
            _raise(error, "the annotations cannot be written")
        return _buffer(out, size.value)

    def import_annotations(self, data, format="xfdf"):
        data = bytes(data)
        error = ctypes.c_void_p()
        count = _pdf_annotations_import(self._check(), 1 if format == "fdf" else 0, data, len(data), ctypes.byref(error))
        if count < 0:
            _raise(error, "the annotations cannot be read")
        return count

    def apply(self, operation):
        """Runs an operation a hook saw, or its "inverse"."""
        self._ok(_pdf_apply, json.dumps(operation).encode(), fallback="the operation cannot be run")

    def page_data(self, index, prefix):
        return self._json(_pdf_page_data, index, prefix.encode(), fallback="no page")

    def add_font(self, family, font):
        font = bytes(font)
        self._ok(_pdf_font_add, family.encode(), font, len(font), fallback="the font cannot be used")

    def filter_input(self, tool, points, options=None):
        error = ctypes.c_void_p()
        out = _pdf_filter_input(self._check(), tool.encode(), json.dumps(points).encode(),
                                json.dumps(options).encode() if options is not None else None, ctypes.byref(error))
        if not out:
            _raise(error, "the points cannot be filtered")
        return json.loads(_string(out))

    for function in (annotation_at, move_annotation, copy_annotation, flatten_annotation, stamp_from_page,
                     attach_file, annotation_file, draw_annotation, annotation_action, export_annotations,
                     import_annotations, apply, page_data, add_font, filter_input):
        setattr(cls, function.__name__, function)
    cls.hooks = property(lambda self: PdfHooks(self))


_pdf_more(Pdf)


# ---- Contexts, views' windows, script outside pages: <zebkit/zebkit.h> ----

_URL = ctypes.CFUNCTYPE(None, _V, _S, _P)
_DROP = ctypes.CFUNCTYPE(None, _V, _S, _P)
_CAPTURE = ctypes.CFUNCTYPE(None, _V, _P, _Z, _S, _P)
_context_new = _declare("zebkit_context_new", _P)
_context_close = _declare("zebkit_context_close", None, _P)
_view_new_in = _declare("zebkit_view_new_in", _V, _P, _S, _I, _I)
_view_set_size_limits = _declare("zebkit_view_set_size_limits", None, _V, _I, _I, _B)
_view_attach = _declare("zebkit_view_attach", _B, _V, _V, _I, _I, _I, _I)
_view_set_visible = _declare("zebkit_view_set_visible", None, _V, _B)
_view_set_title = _declare("zebkit_view_set_title", None, _V, _S)
_view_add_script = _declare("zebkit_view_add_script", None, _V, _S)
_view_stop = _declare("zebkit_view_stop", None, _V)
_view_is_loading = _declare("zebkit_view_is_loading", _B, _V)
_view_set_user_agent = _declare("zebkit_view_set_user_agent", None, _V, _S)
_view_capture = _declare("zebkit_view_capture", None, _V, _CAPTURE, _P)
_view_on_new_window = _declare("zebkit_view_on_new_window", None, _V, _URL, _P)
_view_on_drop = _declare("zebkit_view_on_drop", None, _V, _DROP, _P)
_script_new = _declare("zebkit_script_new", _P, _I, ctypes.POINTER(ctypes.c_char_p))
_script_run_module = _declare("zebkit_script_run_module", None, _P, _S, _RESULT, _P)
_script_evaluate = _declare("zebkit_script_evaluate", None, _P, _S, _RESULT, _P)
_script_on_idle = _declare("zebkit_script_on_idle", None, _P, _TASK, _P)
_script_destroy = _declare("zebkit_script_destroy", None, _P)


class Context:
    """Cookies, storage and cache of their own for the views made in it, as a
    browser's profile or Playwright's BrowserContext; off the record. close()
    closes its views."""

    def __init__(self):
        self._handle = _context_new()
        if not self._handle:
            raise Error("cannot create a context")

    def _check(self):
        _require_thread("a context")
        if not self._handle:
            raise Error("the context is closed")
        return self._handle

    def close(self):
        handle, self._handle = self._handle, None
        if handle:
            _context_close(handle)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class Script:
    """JavaScript outside any page, on the engine's V8, as zebkit-js runs it:
    zebkit.* is there, without a page. Engine thread only."""

    def __init__(self, args=()):
        argv = [b"zebkit"] + [str(a).encode() for a in args]
        array = (ctypes.c_char_p * len(argv))(*argv)
        self._handle = _script_new(len(argv), array)
        if not self._handle:
            raise Error("cannot create a script runtime")

    def _check(self):
        _require_thread("a script runtime")
        if not self._handle:
            raise Error("the script runtime is closed")
        return self._handle

    def _result(self, function, value_from):
        def done(view, result, error, data):
            _alive.discard(callback)
            if function:
                if error is not None:
                    function(None, _text(error))
                else:
                    function(value_from(result), None)
        callback = _RESULT(_guard(done))
        _alive.add(callback)
        return callback

    def run_module(self, path, function=None):
        """Runs the ES module at path; function(None, error_or_None) when it
        has finished, its promises included."""
        handle = self._check()
        callback = self._result(function, lambda result: None)
        _script_run_module(handle, os.fspath(path).encode(), callback, None)

    def evaluate(self, source, function=None):
        """Evaluates source; function(value, error), value as from JSON."""
        handle = self._check()
        callback = self._result(function, lambda result: json.loads(_text(result)) if result else None)
        _script_evaluate(handle, source.encode(), callback, None)

    def on_idle(self, function):
        """function() once nothing is left to run: no timers, no pending work."""
        handle = self._check()
        def idle(data):
            _alive.discard(callback)
            function()
        callback = _TASK(_guard(idle))
        _alive.add(callback)
        _script_on_idle(handle, callback, None)

    def close(self):
        handle, self._handle = self._handle, None
        if handle:
            _script_destroy(handle)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def _view_core(cls):
    def set_size_limits(self, min_width=0, min_height=0, resizable=True):
        """The window's smallest size, and whether it may be resized."""
        _view_set_size_limits(self._check(), min_width, min_height, bool(resizable))

    def attach(self, parent, x, y, width, height):
        """Shows this view inside parent's window, at (x, y), width by height.
        Raises Error for the view itself as parent, or a parent without a
        window to be in."""
        if not _view_attach(self._check(), parent._check(), x, y, width, height):
            raise Error("the view was not attached: it is the parent, or the parent has no window to be in")

    def set_visible(self, visible):
        _view_set_visible(self._check(), bool(visible))

    def set_title(self, title):
        """The window's title; None for the page's."""
        _view_set_title(self._check(), title.encode() if title is not None else None)

    def add_script(self, source):
        """Runs source in every document of the view, before the page's own."""
        _view_add_script(self._check(), source.encode())

    def stop(self):
        _view_stop(self._check())

    @property
    def is_loading(self):
        return bool(_view_is_loading(self._check()))

    def set_user_agent(self, user_agent):
        """The User-Agent the view sends; None for the engine's."""
        _view_set_user_agent(self._check(), user_agent.encode() if user_agent is not None else None)

    def capture(self, function):
        """function(png_bytes, error): a PNG of what the view shows."""
        handle = self._check()
        def done(view, png, size, error, data):
            _alive.discard(callback)
            if error is not None:
                function(None, _text(error))
            else:
                function(ctypes.string_at(png, size) if png and size else b"", None)
        callback = _CAPTURE(_guard(done))
        _alive.add(callback)
        _view_capture(handle, callback, None)

    def on_new_window(self, function):
        """function(url) for a link or window.open() the page wanted in a new
        window, when no on_window_open takes it; None removes it."""
        if function is None:
            return self._set("new-window", _URL(), _view_on_new_window)
        self._set("new-window", _URL(_guard(lambda h, url, d: function(_text(url)))), _view_on_new_window)

    def on_drop(self, function):
        """function(paths): files the person dropped on the view's window."""
        if function is None:
            return self._set("drop", _DROP(), _view_on_drop)
        self._set("drop", _DROP(_guard(lambda h, paths, d: function(json.loads(_text(paths, "[]"))))), _view_on_drop)

    for function in (set_size_limits, attach, set_visible, set_title, add_script, stop, is_loading,
                     set_user_agent, capture, on_new_window, on_drop):
        setattr(cls, getattr(function, "__name__", None) or function.fget.__name__, function)


_view_core(View)


# ---- Off-screen views: <zebkit/offscreen.h> ----

class _Rect(ctypes.Structure):
    _fields_ = [("x", ctypes.c_int32), ("y", ctypes.c_int32), ("width", ctypes.c_int32), ("height", ctypes.c_int32)]


class _Frame(ctypes.Structure):
    _fields_ = [("sequence", ctypes.c_uint64), ("layer", ctypes.c_int), ("bounds", _Rect),
                ("width", ctypes.c_int32), ("height", ctypes.c_int32), ("scale", ctypes.c_float),
                ("damage", ctypes.POINTER(_Rect)), ("damage_count", ctypes.c_size_t),
                ("drawn_us", ctypes.c_int64), ("ready_us", ctypes.c_int64),
                ("kind", ctypes.c_int), ("format", ctypes.c_int), ("buffer_id", ctypes.c_uint64),
                ("pixels", ctypes.c_void_p), ("stride", ctypes.c_size_t), ("fd", ctypes.c_int),
                ("size", ctypes.c_size_t), ("offset", ctypes.c_size_t), ("iosurface", ctypes.c_void_p),
                ("plane_count", ctypes.c_int), ("fds", ctypes.c_int * 4), ("strides", ctypes.c_uint32 * 4),
                ("offsets", ctypes.c_uint32 * 4), ("modifier", ctypes.c_uint64), ("fourcc", ctypes.c_uint32),
                ("handle", ctypes.c_void_p)]


class _KeyEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("modifiers", ctypes.c_uint), ("key_code", ctypes.c_int),
                ("native_key_code", ctypes.c_int), ("code", ctypes.c_char_p), ("key", ctypes.c_char_p),
                ("text", ctypes.c_char_p), ("unmodified_text", ctypes.c_char_p)]


class _EditCommand(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("value", ctypes.c_char_p)]


class _CursorImage(ctypes.Structure):
    _fields_ = [("pixels", ctypes.c_void_p), ("width", ctypes.c_int32), ("height", ctypes.c_int32),
                ("hot_x", ctypes.c_int32), ("hot_y", ctypes.c_int32), ("scale", ctypes.c_float)]


_FRAME = ctypes.CFUNCTYPE(None, _V, ctypes.POINTER(_Frame), _P)
_CARET = ctypes.CFUNCTYPE(None, _V, ctypes.POINTER(_Rect), _P)
_CURSOR = ctypes.CFUNCTYPE(None, _V, _S, ctypes.POINTER(_CursorImage), _P)
_POPUP = ctypes.CFUNCTYPE(None, _V, _B, ctypes.POINTER(_Rect), _P)
_view_new_offscreen = _declare("zebkit_view_new_offscreen", _V, _S, _I, _I, ctypes.c_float)
_view_new_offscreen_in = _declare("zebkit_view_new_offscreen_in", _V, _P, _S, _I, _I, ctypes.c_float)
_view_is_offscreen = _declare("zebkit_view_is_offscreen", _B, _V)
_view_set_size = _declare("zebkit_view_set_size", None, _V, _I, _I, ctypes.c_float)
_view_on_frame = _declare("zebkit_view_on_frame", None, _V, _FRAME, _P)
_frame_release = _declare("zebkit_frame_release", None, ctypes.POINTER(_Frame))
_view_set_frame_rate = _declare("zebkit_view_set_frame_rate", None, _V, ctypes.c_double)
_view_max_frames = _declare("zebkit_view_set_max_frames_in_flight", None, _V, _I)
_view_frame_buffers = _declare("zebkit_view_set_frame_buffers", None, _V, _I)
_view_request_frame = _declare("zebkit_view_request_frame", None, _V)
_frame_mach_port = _declare("zebkit_frame_iosurface_mach_port", ctypes.c_uint32, ctypes.POINTER(_Frame))
_view_send_mouse = _declare("zebkit_view_send_mouse", None, _V, _I, ctypes.c_float, ctypes.c_float, _I, _I, _U)
_view_send_wheel = _declare("zebkit_view_send_wheel", None, _V, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                            ctypes.c_float, _B, _I, _U)
_view_send_key = _declare("zebkit_view_send_key", None, _V, ctypes.POINTER(_KeyEvent))
_view_send_key_commands = _declare("zebkit_view_send_key_with_commands", _B, _V, ctypes.POINTER(_KeyEvent),
                                   ctypes.POINTER(_EditCommand), _Z)
_view_ime_set_composition = _declare("zebkit_view_ime_set_composition", None, _V, _S, _I, _I)
_view_ime_commit = _declare("zebkit_view_ime_commit", None, _V, _S)
_view_ime_cancel = _declare("zebkit_view_ime_cancel", None, _V)
_view_on_caret = _declare("zebkit_view_on_caret", None, _V, _CARET, _P)
_view_set_focus = _declare("zebkit_view_set_focus", None, _V, _B)
_view_on_cursor = _declare("zebkit_view_on_cursor", None, _V, _CURSOR, _P)
_view_on_popup = _declare("zebkit_view_on_popup", None, _V, _POPUP, _P)

FRAME_SHARED_MEMORY, FRAME_IOSURFACE, FRAME_DMABUF, FRAME_D3D11 = 1, 2, 3, 4
LAYER_VIEW, LAYER_POPUP = 0, 1
FORMAT_BGRA8, FORMAT_RGBA8 = 1, 2
BUFFERS_AUTO, BUFFERS_SHARED_MEMORY, BUFFERS_GPU = 0, 1, 2
MODIFIER_SHIFT, MODIFIER_CONTROL, MODIFIER_ALT, MODIFIER_META = 1, 2, 4, 8
MODIFIER_CAPS_LOCK, MODIFIER_NUM_LOCK, MODIFIER_LEFT_BUTTON, MODIFIER_MIDDLE_BUTTON = 16, 32, 64, 128
MODIFIER_RIGHT_BUTTON, MODIFIER_KEYPAD, MODIFIER_AUTO_REPEAT = 256, 512, 1024
MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP, MOUSE_LEAVE = 0, 1, 2, 3
BUTTON_NONE, BUTTON_LEFT, BUTTON_MIDDLE, BUTTON_RIGHT, BUTTON_BACK, BUTTON_FORWARD = 0, 1, 2, 3, 4, 5
WHEEL_PHASE_NONE, WHEEL_PHASE_BEGAN, WHEEL_PHASE_CHANGED, WHEEL_PHASE_ENDED, WHEEL_PHASE_MOMENTUM = 0, 1, 2, 3, 4
KEY_DOWN, KEY_UP, KEY_CHAR = 0, 1, 2


def _rect(rect):
    return {"x": rect.x, "y": rect.y, "width": rect.width, "height": rect.height}


class Frame:
    """A frame of an off-screen view (<zebkit/offscreen.h>): its buffer is
    the engine's until release(), which dropping the Frame also does. Keep
    it to keep the buffer; the engine holds back frames while the
    application holds as many as set_max_frames_in_flight allows."""

    def __init__(self, pointer):
        self._pointer = pointer
        frame = pointer.contents
        self.sequence = frame.sequence
        self.layer = frame.layer
        self.bounds = _rect(frame.bounds)
        self.width, self.height, self.scale = frame.width, frame.height, frame.scale
        self.damage = [_rect(frame.damage[i]) for i in range(frame.damage_count)] if frame.damage else []
        self.drawn_us, self.ready_us = frame.drawn_us, frame.ready_us
        self.kind, self.format, self.buffer_id = frame.kind, frame.format, frame.buffer_id
        self.stride, self.fd, self.size, self.offset = frame.stride, frame.fd, frame.size, frame.offset
        self.iosurface = frame.iosurface
        self.handle = frame.handle
        planes = max(0, min(4, frame.plane_count))
        self.fds = list(frame.fds[:planes])
        self.strides = list(frame.strides[:planes])
        self.offsets = list(frame.offsets[:planes])
        self.modifier, self.fourcc = frame.modifier, frame.fourcc

    def pixels(self):
        """The shared memory frame's pixels, copied: stride bytes a row,
        format BGRA8 or RGBA8; None for a GPU buffer or once released."""
        if not self._pointer:
            return None
        frame = self._pointer.contents
        if frame.kind != FRAME_SHARED_MEMORY or not frame.pixels or frame.height <= 0:
            return None
        return ctypes.string_at(frame.pixels, frame.stride * (frame.height - 1) + frame.width * 4)

    def memory(self):
        """The shared memory frame's pixels without a copy, as pixels()
        has them: a read-only memoryview valid until release(), which must
        not be used after it; None for a GPU buffer or once released."""
        if not self._pointer:
            return None
        frame = self._pointer.contents
        if frame.kind != FRAME_SHARED_MEMORY or not frame.pixels or frame.height <= 0:
            return None
        size = frame.stride * (frame.height - 1) + frame.width * 4
        return memoryview((ctypes.c_char * size).from_address(frame.pixels)).toreadonly()

    def iosurface_mach_port(self):
        """macOS: a send right to the frame's IOSurface, for another process; 0 elsewhere."""
        return _frame_mach_port(self._pointer) if self._pointer else 0

    def release(self):
        pointer, self._pointer = self._pointer, None
        if pointer:
            _frame_release(pointer)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.release()

    def __del__(self):
        self.release()


def _key_event(kind, key_code=0, code=None, key=None, text=None, unmodified_text=None, modifiers=0,
               native_key_code=0):
    enc = lambda value: value.encode() if value is not None else None  # noqa: E731
    return _KeyEvent(kind, modifiers, key_code, native_key_code, enc(code), enc(key), enc(text), enc(unmodified_text))


def _view_offscreen(cls):
    @classmethod
    def offscreen(cls, url=None, width=800, height=600, scale=1.0, context=None):
        """A view with no window: its frames go to on_frame, and the
        application sends its input (send_mouse, send_key, ...)."""
        url = url.encode() if url else None
        if context is not None:
            handle = _view_new_offscreen_in(context._check(), url, width, height, scale)
        else:
            handle = _view_new_offscreen(url, width, height, scale)
        if not handle:
            raise Error("cannot create an off-screen view")
        view = cls.__new__(cls)
        view._handle = handle
        view._initialize_callbacks()
        return view

    @property
    def is_offscreen(self):
        return bool(_view_is_offscreen(self._check()))

    def set_size(self, width, height, scale=1.0):
        _view_set_size(self._check(), width, height, scale)

    def on_frame(self, function):
        """function(Frame) for each frame; the frame is released when the
        Frame is dropped or released. None stops."""
        if function is None:
            return self._set("frame", _FRAME(), _view_on_frame)

        def frame(handle, pointer, data):
            given = Frame(pointer)
            try:
                function(given)
            finally:
                del given
        self._set("frame", _FRAME(_guard(frame)), _view_on_frame)

    def set_frame_rate(self, fps):
        _view_set_frame_rate(self._check(), float(fps))

    def set_max_frames_in_flight(self, count):
        _view_max_frames(self._check(), count)

    def set_frame_buffers(self, buffers):
        """BUFFERS_AUTO, BUFFERS_SHARED_MEMORY or BUFFERS_GPU."""
        _view_frame_buffers(self._check(), buffers)

    def request_frame(self):
        """A new frame now, even when nothing changed."""
        _view_request_frame(self._check())

    def send_mouse(self, kind, x, y, button=BUTTON_NONE, click_count=0, modifiers=0):
        """kind MOUSE_MOVE, MOUSE_DOWN, MOUSE_UP or MOUSE_LEAVE, at (x, y) in CSS pixels."""
        _view_send_mouse(self._check(), kind, x, y, button, click_count, modifiers)

    def send_wheel(self, x, y, delta_x, delta_y, precise=False, phase=WHEEL_PHASE_NONE, modifiers=0):
        _view_send_wheel(self._check(), x, y, delta_x, delta_y, bool(precise), phase, modifiers)

    def send_key(self, kind, key_code=0, code=None, key=None, text=None, unmodified_text=None, modifiers=0,
                 native_key_code=0):
        """kind KEY_DOWN, KEY_UP or KEY_CHAR (with text)."""
        event = _key_event(kind, key_code, code, key, text, unmodified_text, modifiers, native_key_code)
        _view_send_key(self._check(), ctypes.byref(event))

    def send_key_with_commands(self, commands, kind=KEY_DOWN, key_code=0, code=None, key=None, text=None,
                               unmodified_text=None, modifiers=0, native_key_code=0):
        """A key that runs editing commands, [(name, value_or_None), ...] such
        as [("selectAll", None)], as the system's key bindings do. False when
        a command is unknown."""
        event = _key_event(kind, key_code, code, key, text, unmodified_text, modifiers, native_key_code)
        items = [(name.encode(), value.encode() if value is not None else None) for name, value in commands]
        array = (_EditCommand * len(items))(*[_EditCommand(n, v) for n, v in items])
        return bool(_view_send_key_commands(self._check(), ctypes.byref(event), array, len(items)))

    def ime_set_composition(self, text, selection_start=-1, selection_end=-1):
        _view_ime_set_composition(self._check(), text.encode(), selection_start, selection_end)

    def ime_commit(self, text):
        _view_ime_commit(self._check(), text.encode())

    def ime_cancel(self):
        _view_ime_cancel(self._check())

    def set_focus(self, focused):
        _view_set_focus(self._check(), bool(focused))

    def on_caret(self, function):
        """function(rect_or_None): where the text caret is, in CSS pixels."""
        if function is None:
            return self._set("caret", _CARET(), _view_on_caret)
        self._set("caret", _CARET(_guard(lambda h, rect, d: function(_rect(rect.contents) if rect else None))),
                  _view_on_caret)

    def on_cursor(self, function):
        """function(cursor, image): the CSS cursor's name, and for a custom
        one {"pixels", "width", "height", "hotX", "hotY", "scale"}, else None."""
        def cursor(handle, name, image, data):
            picture = None
            if image:
                i = image.contents
                picture = {"pixels": ctypes.string_at(i.pixels, i.width * i.height * 4) if i.pixels else b"",
                           "width": i.width, "height": i.height, "hotX": i.hot_x, "hotY": i.hot_y, "scale": i.scale}
            function(name.decode() if name else "", picture)
        if function is None:
            return self._set("cursor", _CURSOR(), _view_on_cursor)
        self._set("cursor", _CURSOR(_guard(cursor)), _view_on_cursor)

    def on_popup(self, function):
        """function(shown, bounds_or_None): a popup (a <select>'s list) over the view."""
        if function is None:
            return self._set("popup", _POPUP(), _view_on_popup)
        self._set("popup", _POPUP(_guard(lambda h, shown, rect, d: function(bool(shown),
                                                                            _rect(rect.contents) if rect else None))),
                  _view_on_popup)

    cls.offscreen = offscreen
    for function in (is_offscreen, set_size, on_frame, set_frame_rate, set_max_frames_in_flight, set_frame_buffers,
                     request_frame, send_mouse, send_wheel, send_key, send_key_with_commands, ime_set_composition,
                     ime_commit, ime_cancel, set_focus, on_caret, on_cursor, on_popup):
        setattr(cls, getattr(function, "__name__", None) or function.fget.__name__, function)


_view_offscreen(View)


# ---- Dialogs, closing, printing, drag and drop: <zebkit/page.h> ----

class _Dialog(ctypes.Structure):
    _fields_ = [("id", ctypes.c_uint64), ("type", ctypes.c_int), ("message", ctypes.c_char_p),
                ("default_prompt", ctypes.c_char_p), ("url", ctypes.c_char_p), ("is_reload", ctypes.c_bool),
                ("held", ctypes.c_bool)]


class _Drag(ctypes.Structure):
    _fields_ = [("data_json", ctypes.c_char_p), ("operations", ctypes.c_uint), ("x", ctypes.c_float),
                ("y", ctypes.c_float), ("image", ctypes.c_void_p), ("image_width", ctypes.c_int),
                ("image_height", ctypes.c_int), ("image_stride", ctypes.c_int), ("image_scale", ctypes.c_float),
                ("image_offset_x", ctypes.c_float), ("image_offset_y", ctypes.c_float)]


_DIALOG = ctypes.CFUNCTYPE(None, _V, ctypes.POINTER(_Dialog), _P)
_DIALOG_CLOSED = ctypes.CFUNCTYPE(None, _V, ctypes.c_uint64, _B, _S, _P)
_PDF_DONE = ctypes.CFUNCTYPE(None, _V, _P, _Z, _S, _P)
_DRAG = ctypes.CFUNCTYPE(_B, _V, ctypes.POINTER(_Drag), _P)
_DRAG_OPERATION = ctypes.CFUNCTYPE(None, _V, _U, _P)
_view_on_dialog = _declare("zebkit_view_on_dialog", None, _V, _DIALOG, _P)
_view_on_dialog_closed = _declare("zebkit_view_on_dialog_closed", None, _V, _DIALOG_CLOSED, _P)
_view_set_dialog_policy = _declare("zebkit_view_set_dialog_policy", None, _V, _I)
_view_get_dialog_policy = _declare("zebkit_view_get_dialog_policy", _I, _V)
_view_dialog_respond = _declare("zebkit_view_dialog_respond", _B, _V, ctypes.c_uint64, _B, _S)
_view_dialogs = _declare("zebkit_view_dialogs", _P, _V)
_view_request_close = _declare("zebkit_view_request_close", None, _V)
_view_print_to_pdf = _declare("zebkit_view_print_to_pdf", None, _V, _S, _PDF_DONE, _P)
_view_on_drag_start = _declare("zebkit_view_on_drag_start", None, _V, _DRAG, _P)
_view_on_drag_end = _declare("zebkit_view_on_drag_end", None, _V, _DRAG_OPERATION, _P)
_view_is_dragging = _declare("zebkit_view_is_dragging", _B, _V)
_view_drag_enter = _declare("zebkit_view_drag_enter", None, _V, _S, ctypes.c_float, ctypes.c_float, _U, _U)
_view_drag_over = _declare("zebkit_view_drag_over", None, _V, ctypes.c_float, ctypes.c_float, _U, _U,
                           _DRAG_OPERATION, _P)
_view_drag_leave = _declare("zebkit_view_drag_leave", None, _V)
_view_drop = _declare("zebkit_view_drop", None, _V, ctypes.c_float, ctypes.c_float, _U, _DRAG_OPERATION, _P)
_view_drag_source_end = _declare("zebkit_view_drag_source_end", None, _V, ctypes.c_float, ctypes.c_float, _U)

DIALOG_ALERT, DIALOG_CONFIRM, DIALOG_PROMPT, DIALOG_BEFOREUNLOAD, DIALOG_PRINT = 0, 1, 2, 3, 4
DIALOG_POLICY_DEFAULT, DIALOG_POLICY_HOLD, DIALOG_POLICY_ACCEPT, DIALOG_POLICY_DISMISS = 0, 1, 2, 3
DRAG_NONE, DRAG_COPY, DRAG_LINK, DRAG_MOVE, DRAG_ALL = 0, 1, 2, 16, 19


def _view_page(cls):
    def on_dialog(self, function):
        """function(dialog): {"id", "type", "message", "defaultPrompt", "url",
        "isReload", "held"} for each alert, confirm, prompt, beforeunload or
        print; a held one waits for dialog_respond(). None removes it."""
        def dialog(handle, pointer, data):
            d = pointer.contents
            text = lambda value: value.decode() if value else ""  # noqa: E731
            function({"id": d.id, "type": d.type, "message": text(d.message),
                      "defaultPrompt": text(d.default_prompt), "url": text(d.url),
                      "isReload": bool(d.is_reload), "held": bool(d.held)})
        if function is None:
            return self._set("dialog", _DIALOG(), _view_on_dialog)
        self._set("dialog", _DIALOG(_guard(dialog)), _view_on_dialog)

    def on_dialog_closed(self, function):
        """function(dialog_id, accepted, user_input_or_None)."""
        if function is None:
            return self._set("dialog-closed", _DIALOG_CLOSED(), _view_on_dialog_closed)
        self._set("dialog-closed", _DIALOG_CLOSED(_guard(
            lambda h, dialog_id, accepted, text, d: function(dialog_id, bool(accepted),
                                                             text.decode() if text is not None else None))),
            _view_on_dialog_closed)

    def set_dialog_policy(self, policy):
        """DIALOG_POLICY_DEFAULT, _HOLD, _ACCEPT or _DISMISS."""
        _view_set_dialog_policy(self._check(), policy)

    @property
    def dialog_policy(self):
        return _view_get_dialog_policy(self._check())

    def respond_dialog(self, dialog_id, accept, prompt_text=None):
        """Answers a held dialog; False when there is no such dialog."""
        return bool(_view_dialog_respond(self._check(), dialog_id, bool(accept),
                                         prompt_text.encode() if prompt_text is not None else None))

    def dialogs(self):
        """The dialogs open now, as the engine lists them."""
        value = _string(_view_dialogs(self._check()))
        return json.loads(value) if value else []

    def request_close(self):
        """Closes the view as a person would: the page's beforeunload first."""
        _view_request_close(self._check())

    def print_to_pdf(self, function, options=None):
        """function(pdf_bytes, error): the page printed, as Chrome's
        Page.printToPDF, with its options (landscape, paperWidth, ...)."""
        handle = self._check()
        def done(view, pdf, size, error, data):
            _alive.discard(callback)
            if error is not None:
                function(None, _text(error))
            else:
                function(ctypes.string_at(pdf, size) if pdf and size else b"", None)
        callback = _PDF_DONE(_guard(done))
        _alive.add(callback)
        _view_print_to_pdf(handle, json.dumps(options).encode() if options is not None else None,
                           callback, None)

    def on_drag_start(self, function):
        """function(drag) -> bool: the page started a drag: {"data",
        "operations", "x", "y", "image": {"pixels", "width", "height",
        "stride", "scale", "offsetX", "offsetY"} or None}. Return True to run
        it yourself, False to let the engine run it in the page."""
        def drag(handle, pointer, data):
            d = pointer.contents
            image = None
            if d.image:
                image = {"pixels": ctypes.string_at(d.image, d.image_stride * d.image_height),
                         "width": d.image_width, "height": d.image_height, "stride": d.image_stride,
                         "scale": d.image_scale, "offsetX": d.image_offset_x, "offsetY": d.image_offset_y}
            try:
                return bool(function({"data": json.loads(d.data_json.decode()) if d.data_json else None,
                                      "operations": d.operations, "x": d.x, "y": d.y, "image": image}))
            except Exception:  # noqa: BLE001
                sys.excepthook(*sys.exc_info())
                return False
        if function is None:
            return self._set("drag-start", _DRAG(), _view_on_drag_start)
        self._set("drag-start", _DRAG(drag), _view_on_drag_start)

    def on_drag_end(self, function):
        """function(operation): what the page's drag ended with (DRAG_*)."""
        if function is None:
            return self._set("drag-end", _DRAG_OPERATION(), _view_on_drag_end)
        self._set("drag-end", _DRAG_OPERATION(_guard(lambda h, operation, d: function(operation))),
                  _view_on_drag_end)

    @property
    def is_dragging(self):
        return bool(_view_is_dragging(self._check()))

    def drag_enter(self, data, x, y, operations=DRAG_ALL, modifiers=0):
        """A drag from outside enters the view with data (a dict of the drag's
        items, as zebkit_view_drag_enter takes them, or its JSON)."""
        text = data if isinstance(data, str) else json.dumps(data)
        _view_drag_enter(self._check(), text.encode(), x, y, operations, modifiers)

    def _operation(self, function):
        def done(view, operation, data):
            _alive.discard(callback)
            if function:
                function(operation)
        callback = _DRAG_OPERATION(_guard(done))
        _alive.add(callback)
        return callback

    def drag_over(self, x, y, operations=DRAG_ALL, modifiers=0, function=None):
        """function(operation): what the page would do with it there."""
        _view_drag_over(self._check(), x, y, operations, modifiers, self._operation(function), None)

    def drag_leave(self):
        _view_drag_leave(self._check())

    def drop(self, x, y, modifiers=0, function=None):
        """Drops the drag there; function(operation) with what the page did."""
        _view_drop(self._check(), x, y, modifiers, self._operation(function), None)

    def drag_source_end(self, x, y, operation):
        """Ends the page's own drag that the application ran (on_drag_start's True)."""
        _view_drag_source_end(self._check(), x, y, operation)

    for function in (on_dialog, on_dialog_closed, set_dialog_policy, dialog_policy, respond_dialog, dialogs,
                     request_close, print_to_pdf, on_drag_start, on_drag_end, is_dragging, drag_enter, _operation,
                     drag_over, drag_leave, drop, drag_source_end):
        setattr(cls, getattr(function, "__name__", None) or function.fget.__name__, function)


_view_page(View)


# ---- Media a page plays: <zebkit/media.h> ----

_MEDIA_CAPTURE = ctypes.CFUNCTYPE(None, _P, _S, _P)
_view_media_list = _declare("zebkit_view_media_list", None, _V, _RESULT, _P)
_view_media_at = _declare("zebkit_view_media_at", None, _V, ctypes.c_float, ctypes.c_float, _RESULT, _P)
_media_keep_history = _declare("zebkit_media_keep_history", None, _B)
_media_capture_start = _declare("zebkit_media_capture_start", _P, _V, _S, _S, _S, _EP)
_media_capture_on_progress = _declare("zebkit_media_capture_on_progress", None, _P, _MEDIA_CAPTURE, _P)
_media_capture_on_done = _declare("zebkit_media_capture_on_done", None, _P, _MEDIA_CAPTURE, _P)
_media_capture_stop = _declare("zebkit_media_capture_stop", None, _P)
_media_capture_status = _declare("zebkit_media_capture_status", _P, _P)
_media_capture_free = _declare("zebkit_media_capture_free", None, _P)


def media_keep_history(keep):
    """Whether the engine keeps what media played before a capture starts,
    so that a capture can begin from the start."""
    _media_keep_history(bool(keep))


class MediaCapture:
    """What a page plays, written to a file as it plays (<zebkit/media.h>).
    View.media_capture() starts it; close() frees it."""

    def __init__(self, view, media_id, path, **options):
        error = _P()
        self._handle = _media_capture_start(view._check(), str(media_id).encode(), os.fspath(path).encode(),
                                            json.dumps(options).encode() if options else None, ctypes.byref(error))
        if not self._handle:
            _raise(error, "the capture did not start")
        self._callbacks = {}
        _alive.add(self)

    def _check(self):
        _require_thread("a media capture")
        if not self._handle:
            raise Error("the capture is closed")
        return self._handle

    def _on(self, kind, setter, function):
        handle = self._check()
        callback = _MEDIA_CAPTURE(_guard(lambda capture, text, data: function(json.loads(_text(text, "{}"))))) \
            if function is not None else _MEDIA_CAPTURE()
        setter(handle, callback, None)
        _retire(self._callbacks.pop(kind, None))
        if function is not None:
            self._callbacks[kind] = callback

    def on_progress(self, function):
        """function(progress) as it writes."""
        self._on("progress", _media_capture_on_progress, function)

    def on_done(self, function):
        """function(result) once the file is finished, or the capture failed."""
        self._on("done", _media_capture_on_done, function)

    def stop(self):
        if self._handle:
            _media_capture_stop(self._handle)

    def status(self):
        if not self._handle:
            return None
        value = _string(_media_capture_status(self._handle))
        return json.loads(value) if value else None

    def close(self):
        handle, self._handle = getattr(self, "_handle", None), None
        if handle:
            _media_capture_free(handle)
            for callback in getattr(self, "_callbacks", {}).values():
                _retire(callback)
            self._callbacks = {}
        _alive.discard(self)


def _view_media(cls):
    def _media_result(self, function):
        def done(view, result, error, data):
            _alive.discard(callback)
            if error is not None:
                function(None, _text(error))
            else:
                function(json.loads(_text(result)) if result else None, None)
        callback = _RESULT(_guard(done))
        _alive.add(callback)
        return callback

    def media_list(self, function):
        """function(media, error): what the page's frames play."""
        _view_media_list(self._check(), self._media_result(function), None)

    def media_at(self, x, y, function):
        """function(media, error): the media element at (x, y), or None."""
        _view_media_at(self._check(), x, y, self._media_result(function), None)

    def media_capture(self, media_id, path, **options):
        """Starts writing media_id (from media_list) to path: a MediaCapture."""
        return MediaCapture(self, media_id, path, **options)

    for function in (_media_result, media_list, media_at, media_capture):
        setattr(cls, function.__name__, function)


_view_media(View)


# ---- PDF, the rest of <zebkit/pdf.h> ----

_pdf_text_lines = _declare("zebkit_pdf_text_lines", _P, _P, _I, _EP)
_pdf_images = _declare("zebkit_pdf_images", _P, _P, _I, _EP)
_pdf_image_data = _declare("zebkit_pdf_image_data", _P, _P, _I, _I, _ZP, ctypes.POINTER(ctypes.c_bool), _EP)
_pdf_signatures = _declare("zebkit_pdf_signatures", _P, _P, _EP)
_pdf_signature_contents = _declare("zebkit_pdf_signature_contents", _P, _P, _I, _ZP, _EP)
_pdf_annotation_set_image = _declare("zebkit_pdf_annotation_set_image", _B, _P, _I, _I, _P, _I, _I, _EP)
_pdf_redact = _declare("zebkit_pdf_redact", _I, _P, _I, _S, ctypes.c_uint32, _EP)
_pdf_form_fields = _declare("zebkit_pdf_form_fields", _P, _P, _EP)
_pdf_form_set = _declare("zebkit_pdf_form_set", _B, _P, _S, _S, _EP)
_pdf_form_reset = _declare("zebkit_pdf_form_reset", _B, _P, _EP)
_pdf_page_rotate = _declare("zebkit_pdf_page_rotate", _B, _P, _I, _I, _EP)
_pdf_pages_delete = _declare("zebkit_pdf_pages_delete", _B, _P, _IP, _I, _EP)
_pdf_pages_move = _declare("zebkit_pdf_pages_move", _B, _P, _IP, _I, _I, _EP)
_pdf_page_insert = _declare("zebkit_pdf_page_insert", _B, _P, _I, ctypes.c_double, ctypes.c_double, _EP)
_pdf_pages_import = _declare("zebkit_pdf_pages_import", _B, _P, _P, _IP, _I, _I, _EP)
_pdf_new = _declare("zebkit_pdf_new", _P, _EP)


def _ints(values):
    values = list(values)
    return (ctypes.c_int * len(values))(*values), len(values)


def _pdf_rest(cls):
    @classmethod
    def new(cls):
        """A new, empty document, for building one from others' pages."""
        error = _P()
        handle = _pdf_new(ctypes.byref(error))
        if not handle:
            _raise(error, "cannot create a document")
        pdf = cls.__new__(cls)
        pdf._handle = handle
        return pdf

    def text_lines(self, index):
        """The page's lines: [{"text", "box", "unmapped"}, ...]."""
        return self._json(_pdf_text_lines, index)

    def images(self, index):
        """The images the page draws: [{"width", "height", "rect", "jpeg"}, ...]."""
        return self._json(_pdf_images, index)

    def image_data(self, index, image):
        """(bytes, jpeg): the image as stored (a JPEG file) or decoded (RGBA)."""
        size, jpeg, error = ctypes.c_size_t(), ctypes.c_bool(), _P()
        out = _pdf_image_data(self._check(), index, image, ctypes.byref(size), ctypes.byref(jpeg), ctypes.byref(error))
        if not out:
            _raise(error, "no such image")
        return _buffer(out, size.value), bool(jpeg.value)

    def signatures(self):
        """The document's signatures, each checked against the document."""
        return self._json(_pdf_signatures)

    def signature_contents(self, index):
        """Signature index's DER-encoded PKCS#7 or time stamp."""
        size, error = ctypes.c_size_t(), _P()
        out = _pdf_signature_contents(self._check(), index, ctypes.byref(size), ctypes.byref(error))
        if not out:
            _raise(error, "no such signature")
        return _buffer(out, size.value)

    def set_annotation_image(self, index, annotation, rgba, width, height):
        """Makes RGBA pixels, width by height, the appearance of a stamp annotation."""
        rgba = bytes(rgba)
        if len(rgba) < width * height * 4:
            raise ValueError("rgba is smaller than width * height * 4")
        self._ok(_pdf_annotation_set_image, index, annotation, rgba, width, height,
                 fallback="the image cannot be set")

    def redact(self, index, areas=None, fill=0):
        """Takes what is under areas ([[l, b, r, t], ...]; None for the page's
        redaction marks) out of the page for good, with a box of fill
        (0xRRGGBBAA, 0 for none) over each. The number of areas."""
        error = _P()
        count = _pdf_redact(self._check(), index, json.dumps(areas).encode() if areas is not None else None,
                            fill, ctypes.byref(error))
        if count < 0:
            _raise(error, "the page cannot be redacted")
        return count

    def form_fields(self):
        return self._json(_pdf_form_fields)

    def set_field(self, name, value):
        """Sets a form field (by its full name) to value; appearances follow."""
        self._ok(_pdf_form_set, name.encode(), str(value).encode(), fallback="the field cannot be set")

    def reset_form(self):
        self._ok(_pdf_form_reset, fallback="the form cannot be reset")

    def rotate_page(self, index, rotation):
        """0, 90, 180 or 270."""
        self._ok(_pdf_page_rotate, index, rotation, fallback="the page cannot be rotated")

    def delete_pages(self, pages):
        array, count = _ints(pages)
        self._ok(_pdf_pages_delete, array, count, fallback="the pages cannot be deleted")

    def move_pages(self, pages, to):
        """Moves pages, in the order given, to start at index to."""
        array, count = _ints(pages)
        self._ok(_pdf_pages_move, array, count, to, fallback="the pages cannot be moved")

    def insert_page(self, at, width=612.0, height=792.0):
        """A blank page, width by height points, at index at."""
        self._ok(_pdf_page_insert, at, float(width), float(height), fallback="the page cannot be inserted")

    def import_pages(self, source, pages=None, at=None):
        """Copies pages of source (all for None) in at index at (the end for None)."""
        if pages is None:
            array, count = None, 0
        else:
            array, count = _ints(pages)
        if at is None:
            at = self.page_count
        self._ok(_pdf_pages_import, source._handle, array, count, at, fallback="the pages cannot be imported")

    cls.new = new
    for function in (text_lines, images, image_data, signatures, signature_contents, set_annotation_image, redact,
                     form_fields, set_field, reset_form, rotate_page, delete_pages, move_pages, insert_page,
                     import_pages):
        setattr(cls, function.__name__, function)


_pdf_rest(Pdf)

__all__ += [
    "Context", "Script", "Frame", "MediaCapture", "media_keep_history",
    "FRAME_SHARED_MEMORY", "FRAME_IOSURFACE", "FRAME_DMABUF", "FRAME_D3D11", "LAYER_VIEW", "LAYER_POPUP",
    "FORMAT_BGRA8", "FORMAT_RGBA8", "BUFFERS_AUTO", "BUFFERS_SHARED_MEMORY", "BUFFERS_GPU",
    "MODIFIER_SHIFT", "MODIFIER_CONTROL", "MODIFIER_ALT", "MODIFIER_META", "MODIFIER_CAPS_LOCK", "MODIFIER_NUM_LOCK",
    "MODIFIER_LEFT_BUTTON", "MODIFIER_MIDDLE_BUTTON", "MODIFIER_RIGHT_BUTTON", "MODIFIER_KEYPAD",
    "MODIFIER_AUTO_REPEAT", "MOUSE_MOVE", "MOUSE_DOWN", "MOUSE_UP", "MOUSE_LEAVE",
    "BUTTON_NONE", "BUTTON_LEFT", "BUTTON_MIDDLE", "BUTTON_RIGHT", "BUTTON_BACK", "BUTTON_FORWARD",
    "WHEEL_PHASE_NONE", "WHEEL_PHASE_BEGAN", "WHEEL_PHASE_CHANGED", "WHEEL_PHASE_ENDED", "WHEEL_PHASE_MOMENTUM",
    "KEY_DOWN", "KEY_UP", "KEY_CHAR", "DIALOG_ALERT", "DIALOG_CONFIRM", "DIALOG_PROMPT", "DIALOG_BEFOREUNLOAD",
    "DIALOG_PRINT", "DIALOG_POLICY_DEFAULT", "DIALOG_POLICY_HOLD", "DIALOG_POLICY_ACCEPT", "DIALOG_POLICY_DISMISS",
    "DRAG_NONE", "DRAG_COPY", "DRAG_LINK", "DRAG_MOVE", "DRAG_ALL",
]
