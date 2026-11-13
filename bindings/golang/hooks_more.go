package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"runtime"
	"unsafe"
)

// settingsOf decodes an engine-allocated JSON object.
func settingsOf(text *C.char, what string) (map[string]any, error) {
	var value map[string]any
	return value, takeJSON(text, &value, what)
}

// SetSettings changes the view's preferences, JSON: colorScheme, contrast,
// reducedMotion, javaScript, images, ... (docs/API.md); null resets one.
func (v *View) SetSettings(settings any) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := jsonArg(settings)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_view_set_settings(raw, text, &message), message, "the settings were refused")
}

// Settings are the view's preferences that are set.
func (v *View) Settings() (map[string]any, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	return settingsOf(C.zebkit_view_get_settings(raw), "no settings")
}

// SetEngineSettings changes the engine's runtime settings, JSON.
func SetEngineSettings(settings any) error {
	onEngineThread()
	text, err := jsonArg(settings)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_set_engine_settings(text, &message), message, "the engine settings were refused")
}

// EngineSettings are the engine's runtime settings: explicit and effective.
func EngineSettings() (map[string]any, error) {
	onEngineThread()
	return settingsOf(C.zebkit_get_engine_settings(), "no engine settings")
}

// SetRuntimeSettings changes the view's runtime settings, JSON.
func (v *View) SetRuntimeSettings(settings any) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := jsonArg(settings)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_view_set_runtime_settings(raw, text, &message), message, "the runtime settings were refused")
}

// RuntimeSettings are the view's runtime settings: explicit and effective.
func (v *View) RuntimeSettings() (map[string]any, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	return settingsOf(C.zebkit_view_get_runtime_settings(raw), "no runtime settings")
}

// Permission is the view's decision for permission of origin ("" for the
// page's); PermissionInvalid for what it cannot say.
func (v *View) Permission(origin, permission string) PermissionState {
	raw, ok := v.raw()
	if !ok {
		return PermissionInvalid
	}
	corigin := optionalString(origin)
	defer free(corigin)
	cpermission := cString(permission)
	defer free(cpermission)
	return PermissionState(C.zebkit_view_get_permission(raw, corigin, cpermission))
}

// ResetPermissions forgets the view's decisions for origin, or all of them
// for "".
func (v *View) ResetPermissions(origin string) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	corigin := optionalString(origin)
	defer free(corigin)
	var message *C.char
	return failed(C.zebkit_view_reset_permissions(raw, corigin, &message), message, "the permissions were not reset")
}

// SetPermissionTimeout sets the deadline of the view's permission requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetPermissionTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_permission_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetNavigationTimeout sets the deadline of the view's navigation requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetNavigationTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_navigation_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetCertificateTimeout sets the deadline of the view's certificate requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetCertificateTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_certificate_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetHTTPAuthTimeout sets the deadline of the view's HTTP authentication requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetHTTPAuthTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_http_auth_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetDownloadTimeout sets the deadline of the view's download requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetDownloadTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_download_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetClientCertificateTimeout sets the deadline of the view's clientCertificate requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetClientCertificateTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_client_certificate_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetFileChooserTimeout sets the deadline of the view's fileChooser requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetFileChooserTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_file_chooser_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetEyeDropperTimeout sets the deadline of the view's eyeDropper requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetEyeDropperTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_eye_dropper_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetDeviceChooserTimeout sets the deadline of the view's deviceChooser requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetDeviceChooserTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_device_chooser_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetShareTimeout sets the deadline of the view's share requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetShareTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_share_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetMediaAccessTimeout sets the deadline of the view's mediaAccess requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetMediaAccessTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_media_access_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetNetworkTimeout sets the deadline of the view's network requests, 1 to 300000 ms
// (30000 by default).
func (v *View) SetNetworkTimeout(ms uint32) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_network_timeout(raw, C.uint32_t(ms)), "a deadline is 1 to 300000 ms")
}

// SetNavigationFailClosed cancels navigations whose request is unanswered
// at its deadline, instead of continuing them.
func (v *View) SetNavigationFailClosed(failClosed bool) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_navigation_fail_closed(raw, C.bool(failClosed)), "refused")
}

// OnPermissionResult tells function the outcome of each permission request the application was asked: its metadata, state, reason (answered, timeout, cancelled, ...), remembered; nil stops.
func (v *View) OnPermissionResult(function func(map[string]any)) {
	v.told("permission-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_permission_result(raw, C.zebkit_permission_result_func(f), d)
	})
}

// OnNavigationResult tells function the outcome of each navigation asked about: its metadata, state (continued, cancelled,
// redirected), reason (answered, timeout, cancelled, handler-changed, opener-closed) and replacementUrl;
// a popup's go to its opener's function until it has one of its own. nil stops.
func (v *View) OnNavigationResult(function func(map[string]any)) {
	v.told("navigation-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_navigation_result(raw, C.zebkit_navigation_result_func(f), d)
	})
}

// OnPopupBlocked tells function each window a page was kept from opening (SetPopupPolicy): its url, features and gesture; nil stops.
func (v *View) OnPopupBlocked(function func(map[string]any)) {
	v.told("popup-blocked", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_popup_blocked(raw, C.zebkit_popup_blocked_func(f), d)
	})
}

// OnWindowOpenEvent tells function each window a page opened: how, with its url, features and gesture; nil stops.
func (v *View) OnWindowOpenEvent(function func(map[string]any)) {
	v.told("window-open-event", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_window_open_event(raw, C.zebkit_window_open_event_func(f), d)
	})
}

// OnConsole tells function the page's console messages: level, message, source, line, frameId, mainFrame, origin, stackTrace, truncated,
// dropped (a number: the reports skipped since the previous one); nil stops.
func (v *View) OnConsole(function func(map[string]any)) {
	v.told("console", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_console(raw, C.zebkit_console_func(f), d)
	})
}

// OnCertificateResult tells function the outcome of each certificate error asked about; nil stops.
func (v *View) OnCertificateResult(function func(map[string]any)) {
	v.told("certificate-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_certificate_result(raw, C.zebkit_certificate_result_func(f), d)
	})
}

// OnHTTPAuthResult tells function the outcome of each HTTP authentication asked about: its metadata, state (provided, cancelled)
// and reason; nil stops.
func (v *View) OnHTTPAuthResult(function func(map[string]any)) {
	v.told("http-auth-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_http_auth_result(raw, C.zebkit_http_auth_result_func(f), d)
	})
}

// OnExternalProtocol tells function each link to another program (mailto:, ...): url, scheme, mainFrame, userGesture, initiatorOrigin, frameId; the engine opens nothing; nil stops.
func (v *View) OnExternalProtocol(function func(map[string]any)) {
	v.told("external-protocol", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_external_protocol(raw, C.zebkit_external_protocol_func(f), d)
	})
}

// OnDownloadProgress tells function the view's downloads as they go, asked or not: downloadId, guid, url,
// state (in-progress, complete, cancelled, interrupted), outcome (in-progress, complete, cancelled, failed),
// path, savedPath, receivedBytes, totalBytes, mimeType, reason, error ({category, code, name, message,
// resumable} or nil), done; nil stops. What OnDownload's function decided is OnDownloadResult's.
func (v *View) OnDownloadProgress(function func(map[string]any)) {
	v.told("download-progress", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_download_progress(raw, C.zebkit_download_progress_func(f), d)
	})
}

// OnDownloadResult tells function the outcome of each download OnDownload's function was asked about: its
// request's metadata with state "saved" or "cancelled" and reason (answered, timeout, handler-changed,
// cancelled, opener-closed), as every hook's outcome; a popup's go to its opener's function while
// that decides for it. nil stops.
func (v *View) OnDownloadResult(function func(map[string]any)) {
	v.told("download-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_download_result(raw, C.zebkit_download_result_func(f), d)
	})
}

// OnClientCertificateResult tells function the outcome of each client certificate request asked about; nil stops.
func (v *View) OnClientCertificateResult(function func(map[string]any)) {
	v.told("client-certificate-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_client_certificate_result(raw, C.zebkit_client_certificate_result_func(f), d)
	})
}

// OnFileChooserResult tells function the outcome of each file chooser asked about; nil stops.
func (v *View) OnFileChooserResult(function func(map[string]any)) {
	v.told("file-chooser-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_file_chooser_result(raw, C.zebkit_file_chooser_result_func(f), d)
	})
}

// OnEyeDropperResult tells function the outcome of each EyeDropper.open() asked about; nil stops.
func (v *View) OnEyeDropperResult(function func(map[string]any)) {
	v.told("eye-dropper-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_eye_dropper_result(raw, C.zebkit_eye_dropper_result_func(f), d)
	})
}

// OnDeviceChooserResult tells function the outcome of each device chooser asked about; nil stops.
func (v *View) OnDeviceChooserResult(function func(map[string]any)) {
	v.told("device-chooser-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_device_chooser_result(raw, C.zebkit_device_chooser_result_func(f), d)
	})
}

// OnShareResult tells function the outcome of each navigator.share() asked about; nil stops.
func (v *View) OnShareResult(function func(map[string]any)) {
	v.told("share-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_share_result(raw, C.zebkit_share_result_func(f), d)
	})
}

// OnMediaAccessResult tells function the outcome of each media access request asked about; nil stops.
func (v *View) OnMediaAccessResult(function func(map[string]any)) {
	v.told("media-access-result", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_media_access_result(raw, C.zebkit_media_access_result_func(f), d)
	})
}

// OnReservedKey tells function each reserved key pressed (SetReservedKeys): combo, key, code, autoRepeat; nil stops.
func (v *View) OnReservedKey(function func(map[string]any)) {
	v.told("reserved-key", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_reserved_key(raw, C.zebkit_reserved_key_func(f), d)
	})
}

// OnFullscreen tells function each time a page enters or leaves fullscreen; nil stops.
func (v *View) OnFullscreen(function func(map[string]any)) {
	v.told("fullscreen", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_fullscreen(raw, C.zebkit_fullscreen_func(f), d)
	})
}

// OnNetworkEvent tells function what the observe rules of SetNetworkFilter match: request, response, redirect, finished or failed,
// with dropped (a number) counting reports skipped, and the held requests' outcomes: state (continued, blocked, redirected,
// fulfilled; refused for a stage that fails closed) and reason; nil stops.
func (v *View) OnNetworkEvent(function func(map[string]any)) {
	v.told("network-event", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_network_event(raw, C.zebkit_network_event_func(f), d)
	})
}

// PopupPolicy says which windows pages may open.
type PopupPolicy int

const (
	PopupBlockWithoutGesture PopupPolicy = 0
	PopupAllow               PopupPolicy = 1
	PopupBlock               PopupPolicy = 2
)

// SetPopupPolicy says which windows the view's pages may open.
func (v *View) SetPopupPolicy(policy PopupPolicy) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_popup_policy(raw, C.zebkit_popup_policy(policy)), "no such popup policy")
}

// PopupPolicy is the view's popup policy.
func (v *View) PopupPolicy() PopupPolicy {
	raw, ok := v.raw()
	if !ok {
		return PopupBlockWithoutGesture
	}
	return PopupPolicy(C.zebkit_view_get_popup_policy(raw))
}

// WindowOpenInfo is how a popup view was opened: its opener, the URL, the
// features, the gesture.
func (v *View) WindowOpenInfo() (map[string]any, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	return settingsOf(C.zebkit_view_get_window_open_info(raw), "the view was not opened by a page")
}

// CertificateRequest is a certificate error the application decides on.
type CertificateRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *CertificateRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_certificate_request_info((*C.zebkit_certificate_request)(p))
	})
}

// Respond allows the certificate for this load, or refuses it.
func (r *CertificateRequest) Respond(allow bool) error {
	return r.answer(func(p unsafe.Pointer) error {
		return check(C.zebkit_certificate_respond((*C.zebkit_certificate_request)(p), C.bool(allow)), "the answer was refused")
	})
}

// OnCertificate calls function for each certificate error of the view's
// loads; nil removes it (the load fails).
func (v *View) OnCertificate(function func(*CertificateRequest)) {
	v.asked("certificate", wrap(function, func(h HookRequest) *CertificateRequest { return &CertificateRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_certificate(raw, C.zebkit_certificate_func(f), d)
		})
}

// HTTPAuthRequest is an HTTP authentication challenge.
type HTTPAuthRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *HTTPAuthRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_http_auth_request_info((*C.zebkit_http_auth_request)(p))
	})
}

// Respond gives the credentials.
func (r *HTTPAuthRequest) Respond(username, password string) error {
	return r.answer(func(p unsafe.Pointer) error {
		u, pw := cString(username), cString(password)
		defer free(u)
		defer free(pw)
		return check(C.zebkit_http_auth_respond((*C.zebkit_http_auth_request)(p), u, pw), "the answer was refused")
	})
}

// Cancel answers with none.
func (r *HTTPAuthRequest) Cancel() error {
	return r.answer(func(p unsafe.Pointer) error {
		return check(C.zebkit_http_auth_respond((*C.zebkit_http_auth_request)(p), nil, nil), "the answer was refused")
	})
}

// OnHTTPAuth calls function for each HTTP authentication challenge; nil
// removes it (the challenge is cancelled).
func (v *View) OnHTTPAuth(function func(*HTTPAuthRequest)) {
	v.asked("http-auth", wrap(function, func(h HookRequest) *HTTPAuthRequest { return &HTTPAuthRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_http_auth(raw, C.zebkit_http_auth_func(f), d)
		})
}

// DownloadRequest is a download the application saves or cancels.
type DownloadRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *DownloadRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_download_request_info((*C.zebkit_download_request)(p))
	})
}

// Save saves the download at path, absolute.
func (r *DownloadRequest) Save(path string) error {
	return r.answer(func(p unsafe.Pointer) error {
		cpath := cString(path)
		defer free(cpath)
		var message *C.char
		return failed(C.zebkit_download_respond((*C.zebkit_download_request)(p), C.ZEBKIT_DOWNLOAD_SAVE, cpath, &message),
			message, "the download was not saved")
	})
}

// Cancel cancels the download.
func (r *DownloadRequest) Cancel() error {
	return r.answer(func(p unsafe.Pointer) error {
		var message *C.char
		return failed(C.zebkit_download_respond((*C.zebkit_download_request)(p), C.ZEBKIT_DOWNLOAD_CANCEL, nil, &message),
			message, "the download was not cancelled")
	})
}

// OnDownload calls function for each download of the view; nil removes it.
func (v *View) OnDownload(function func(*DownloadRequest)) {
	v.asked("download", wrap(function, func(h HookRequest) *DownloadRequest { return &DownloadRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_download(raw, C.zebkit_download_func(f), d)
		})
}

// CancelDownload cancels a download of the view by its downloadId.
func (v *View) CancelDownload(id string) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	cid := cString(id)
	defer free(cid)
	return check(C.zebkit_view_cancel_download(raw, cid), "no such download")
}

// ClientCertificateRequest is a server's request of a client certificate.
type ClientCertificateRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *ClientCertificateRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_client_certificate_request_info((*C.zebkit_client_certificate_request)(p))
	})
}

func (r *ClientCertificateRequest) respond(action C.zebkit_client_certificate_action, certificate, key string) error {
	return r.answer(func(p unsafe.Pointer) error {
		c, k := optionalString(certificate), optionalString(key)
		defer free(c)
		defer free(k)
		var message *C.char
		return failed(C.zebkit_client_certificate_respond((*C.zebkit_client_certificate_request)(p), action, c, k, &message),
			message, "the answer was refused")
	})
}

// None continues without a certificate.
func (r *ClientCertificateRequest) None() error {
	return r.respond(C.ZEBKIT_CLIENT_CERTIFICATE_NONE, "", "")
}

// Cancel fails the load.
func (r *ClientCertificateRequest) Cancel() error {
	return r.respond(C.ZEBKIT_CLIENT_CERTIFICATE_CANCEL, "", "")
}

// Provide gives a certificate and its private key, PEM.
func (r *ClientCertificateRequest) Provide(certificatePEM, keyPEM string) error {
	return r.respond(C.ZEBKIT_CLIENT_CERTIFICATE_PROVIDE, certificatePEM, keyPEM)
}

// Choose gives one of the system store's identities, by its index in the
// metadata's identities.
func (r *ClientCertificateRequest) Choose(index int) error {
	return r.answer(func(p unsafe.Pointer) error {
		var message *C.char
		return failed(C.zebkit_client_certificate_choose((*C.zebkit_client_certificate_request)(p), C.size_t(index), &message),
			message, "the identity was refused")
	})
}

// OnClientCertificate calls function for each server's request of a client
// certificate; nil removes it.
func (v *View) OnClientCertificate(function func(*ClientCertificateRequest)) {
	v.asked("client-certificate", wrap(function, func(h HookRequest) *ClientCertificateRequest { return &ClientCertificateRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_client_certificate(raw, C.zebkit_client_certificate_func(f), d)
		})
}

// AddWorldScript runs source in each new document before the page's own
// scripts, in an isolated world named world ("" for the page's).
func (v *View) AddWorldScript(world, source string, mainFrameOnly bool) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	cworld, csource := cString(world), cString(source)
	defer free(cworld)
	defer free(csource)
	var message *C.char
	return failed(C.zebkit_view_add_world_script(raw, cworld, csource, C.bool(mainFrameOnly), &message), message, "the script was refused")
}

// FileChooserRequest is a page's file chooser: <input type=file> or File
// System Access.
type FileChooserRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *FileChooserRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_file_chooser_request_info((*C.zebkit_file_chooser_request)(p))
	})
}

// Respond gives the page paths, absolute; none cancels.
func (r *FileChooserRequest) Respond(paths []string) error {
	return r.answer(func(p unsafe.Pointer) error {
		count, array, done := cStrings(paths)
		defer done()
		var message *C.char
		return failed(C.zebkit_file_chooser_respond((*C.zebkit_file_chooser_request)(p), array, C.size_t(count), &message),
			message, "the files were refused")
	})
}

// OnFileChooser calls function for the page's file choosers; nil removes it.
func (v *View) OnFileChooser(function func(*FileChooserRequest)) {
	v.asked("file-chooser", wrap(function, func(h HookRequest) *FileChooserRequest { return &FileChooserRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_file_chooser(raw, C.zebkit_file_chooser_func(f), d)
		})
}

// SetReservedKeys keeps keys from pages, as ["Ctrl+Shift+KeyI"]; OnReservedKey
// tells them.
func (v *View) SetReservedKeys(keys []string) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	if keys == nil {
		keys = []string{}
	}
	text, err := jsonArg(keys)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_view_set_reserved_keys(raw, text, &message), message, "the keys were refused")
}

// RunContextMenuCommand runs a default item's command against what was
// right-clicked.
func (v *View) RunContextMenuCommand(menuID uint64, command string) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	ccommand := cString(command)
	defer free(ccommand)
	var message *C.char
	return failed(C.zebkit_view_context_menu_run(raw, C.uint64_t(menuID), ccommand, &message), message, "the command was refused")
}

// ShowContextMenu shows a menu where it was asked for: the default items
// (nil) or a list mixing default ids and the application's own items, JSON;
// function, called once, gets the id of an item of the application's ("" for
// none).
func (v *View) ShowContextMenu(menuID uint64, items any, function func(id string)) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := optionalJSON(items)
	if err != nil {
		return err
	}
	defer free(text)
	var f C.zebkit_context_menu_item_func
	var h C.uintptr_t
	if function != nil {
		h = onceHandle(function)
		f = C.zebkit_context_menu_item_func(C.zk_view_text)
	}
	var message *C.char
	if !C.zebkit_view_context_menu_show(raw, C.uint64_t(menuID), text, f, data(h), &message) {
		release(h)
		return takeError(message, "the menu was refused")
	}
	// Called once, or let go with the view, or by the next menu.
	v.slot("context-menu-item").replace(h)
	return nil
}

// FullscreenPolicy says whether pages may go fullscreen.
type FullscreenPolicy int

const (
	FullscreenAllow FullscreenPolicy = 0
	FullscreenDeny  FullscreenPolicy = 1
)

// SetFullscreenPolicy says whether the view's pages may go fullscreen.
func (v *View) SetFullscreenPolicy(policy FullscreenPolicy) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_fullscreen_policy(raw, C.zebkit_fullscreen_policy(policy)), "no such fullscreen policy")
}

// SetNetworkFilter limits which requests OnNetwork is asked about, JSON
// (docs/API.md).
func (v *View) SetNetworkFilter(filter any) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := jsonArg(filter)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_view_set_network_filter(raw, text, &message), message, "the filter was refused")
}

// SetNetworkFilterForWorkers asks about workers' requests too.
func (v *View) SetNetworkFilterForWorkers(on bool) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_network_filter_for_workers(raw, C.bool(on)), "refused")
}

// NetworkRequest is a request the application continues, changes, blocks,
// redirects or answers itself.
type NetworkRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *NetworkRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_network_request_info((*C.zebkit_network_request)(p))
	})
}

// Respond replies once, JSON: {"action": "continue"|"block"|"redirect"|"fulfill",
// ...}, with body for "fulfill".
func (r *NetworkRequest) Respond(reply any, body []byte) error {
	return r.answer(func(p unsafe.Pointer) error {
		text, err := jsonArg(reply)
		if err != nil {
			return err
		}
		defer free(text)
		var message *C.char
		return failed(C.zebkit_network_respond((*C.zebkit_network_request)(p), text, bytesArg(body), C.size_t(len(body)), &message),
			message, "the reply was refused")
	})
}

// RespondFile fulfils the request with the file at path, absolute; reply
// has status and headers, JSON.
func (r *NetworkRequest) RespondFile(reply any, path string) error {
	return r.answer(func(p unsafe.Pointer) error {
		text, err := jsonArg(reply)
		if err != nil {
			return err
		}
		defer free(text)
		cpath := cString(path)
		defer free(cpath)
		var message *C.char
		return failed(C.zebkit_network_respond_file((*C.zebkit_network_request)(p), text, cpath, &message),
			message, "the reply was refused")
	})
}

// RespondStream fulfils the request with a body written as it comes.
func (r *NetworkRequest) RespondStream(reply any) (*NetworkStream, error) {
	var stream *C.zebkit_network_stream
	err := r.answer(func(p unsafe.Pointer) error {
		text, err := jsonArg(reply)
		if err != nil {
			return err
		}
		defer free(text)
		var message *C.char
		stream = C.zebkit_network_respond_stream((*C.zebkit_network_request)(p), text, &message)
		if stream == nil {
			return takeError(message, "the reply was refused")
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	s := &NetworkStream{stream: stream}
	runtime.SetFinalizer(s, (*NetworkStream).dropped)
	return s, nil
}

// NetworkStream is a response body the application writes; End it. One the
// program lets go open is ended as failed once the garbage collector finds
// it, as Rust's on Drop and Python's when it is collected, through Post on
// the engine's thread; that can be late, so End every stream.
type NetworkStream struct {
	stream   *C.zebkit_network_stream
	writable slot
}

// Write writes bytes, all or none, and says whether the stream took them.
// false is not an error: the bytes do not fit in what waits now, as a full
// pipe's write would block (OnWritable's function is called once when they
// do), or the page has gone or stopped reading (OnWritable's function is
// called then too, and IsOpen says which). A write over 1 MiB never fits:
// write it in parts.
func (s *NetworkStream) Write(bytes []byte) bool {
	if s.stream == nil {
		return false
	}
	onEngineThread()
	return bool(C.zebkit_network_stream_write(s.stream, bytesArg(bytes), C.size_t(len(bytes))))
}

// OnWritable calls function when the stream takes bytes again; nil stops.
func (s *NetworkStream) OnWritable(function func()) {
	if s.stream == nil {
		return
	}
	onEngineThread()
	if function == nil {
		C.zebkit_network_stream_on_writable(s.stream, nil, nil)
		s.writable.clear()
		return
	}
	h := s.writable.set(function)
	C.zebkit_network_stream_on_writable(s.stream, C.zebkit_network_stream_writable_func(C.zk_writable), data(h))
}

// IsOpen says whether the page still reads the stream.
func (s *NetworkStream) IsOpen() bool {
	if s.stream == nil {
		return false
	}
	onEngineThread()
	return bool(C.zebkit_network_stream_is_open(s.stream))
}

// dropped ends a stream let go open, as failed.
func (s *NetworkStream) dropped() {
	stream := s.stream
	if stream == nil {
		return
	}
	s.stream = nil
	h := s.writable.h
	s.writable.h = 0
	_ = Post(func() {
		C.zebkit_network_stream_end(stream, C.bool(true))
		release(h)
	})
}

// End ends the body, as a failure with failed, and lets the stream go.
func (s *NetworkStream) End(failed bool) {
	if s.stream == nil {
		return
	}
	onEngineThread()
	C.zebkit_network_stream_end(s.stream, C.bool(failed))
	s.stream = nil
	s.writable.clear()
}

// OnNetwork calls function for the requests SetNetworkFilter selects: it is
// asked about nothing until a filter is set. Each request is answered once:
// continue, block, redirect, fulfil, or a stream; nil removes it.
func (v *View) OnNetwork(function func(*NetworkRequest)) {
	v.asked("network", wrap(function, func(h HookRequest) *NetworkRequest { return &NetworkRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_network(raw, C.zebkit_network_func(f), d)
		})
}

// OnNetworkBody gets the responses' bodies as they pass: the request's id,
// the bytes, whether it ended and whether it was cut short. nil stops.
func (v *View) OnNetworkBody(function func(requestID string, bytes []byte, end, truncated bool)) {
	v.hook("network-body", function, function, unsafe.Pointer(C.zk_body), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_network_body(raw, C.zebkit_network_body_func(f), d)
	})
}
