package zebkit

/*
#include <stdlib.h>
#include "bridge_a.h"
*/
import "C"

import (
	"encoding/json"
	"unsafe"
)

// settings parses an engine-allocated JSON object.
func settingsJSON(text *C.char) map[string]any {
	value := map[string]any{}
	takeJSON(text, &value)
	return value
}

// SetSettings changes the view's preferences: colorScheme, contrast,
// reducedMotion, javaScript, images, ... (docs/API.md); null resets one.
func (v *View) SetSettings(settings any) error {
	text, err := jsonText(settings)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	if !C.zebkit_view_set_settings(v.view, text, &message) {
		return takeError(message, "the settings were refused")
	}
	return nil
}

// Settings are the view's preferences that are set.
func (v *View) Settings() map[string]any { return settingsJSON(C.zebkit_view_get_settings(v.view)) }

// SetEngineSettings changes the engine's runtime settings.
func SetEngineSettings(settings any) error {
	text, err := jsonText(settings)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	if !C.zebkit_set_engine_settings(text, &message) {
		return takeError(message, "the engine settings were refused")
	}
	return nil
}

// EngineSettings are the engine's runtime settings.
func EngineSettings() map[string]any { return settingsJSON(C.zebkit_get_engine_settings()) }

// SetRuntimeSettings changes the view's runtime settings.
func (v *View) SetRuntimeSettings(settings any) error {
	text, err := jsonText(settings)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	if !C.zebkit_view_set_runtime_settings(v.view, text, &message) {
		return takeError(message, "the runtime settings were refused")
	}
	return nil
}

// RuntimeSettings are the view's runtime settings.
func (v *View) RuntimeSettings() map[string]any {
	return settingsJSON(C.zebkit_view_get_runtime_settings(v.view))
}

// PermissionInvalid is Permission's answer for what it cannot say.
const PermissionInvalid PermissionState = -1

// Permission is the view's decision for permission of origin ("" for the
// page's).
func (v *View) Permission(origin, permission string) PermissionState {
	corigin := optionalString(origin)
	defer free(corigin)
	cpermission := cString(permission)
	defer free(cpermission)
	return PermissionState(C.zebkit_view_get_permission(v.view, corigin, cpermission))
}

// ResetPermissions forgets the view's decisions for origin, or all of them
// for "".
func (v *View) ResetPermissions(origin string) error {
	corigin := optionalString(origin)
	defer free(corigin)
	var message *C.char
	if !C.zebkit_view_reset_permissions(v.view, corigin, &message) {
		return takeError(message, "the permissions were not reset")
	}
	return nil
}

// A request's deadline, 1 to 300000 milliseconds.

func (v *View) SetPermissionTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_permission_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetNavigationTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_navigation_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetCertificateTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_certificate_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetHTTPAuthTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_http_auth_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetDownloadTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_download_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetClientCertificateTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_client_certificate_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetFileChooserTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_file_chooser_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetEyeDropperTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_eye_dropper_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetDeviceChooserTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_device_chooser_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetShareTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_share_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetMediaAccessTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_media_access_timeout(v.view, C.uint32_t(ms)))
}
func (v *View) SetNetworkTimeout(ms uint32) bool {
	return bool(C.zebkit_view_set_network_timeout(v.view, C.uint32_t(ms)))
}

// SetNavigationFailClosed cancels navigations whose request is unanswered
// at its deadline, instead of continuing them.
func (v *View) SetNavigationFailClosed(failClosed bool) bool {
	return bool(C.zebkit_view_set_navigation_fail_closed(v.view, C.bool(failClosed)))
}

// toldHook installs a JSON-telling hook through the C setter set.
func (v *View) toldHook(name string, function func(map[string]any), set func(f, d unsafe.Pointer)) {
	v.told(name, function, set)
}

// The outcomes of the asked hooks, and what the engine tells as it happens.

func (v *View) OnPermissionResult(function func(map[string]any)) {
	v.toldHook("permission-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_permission_result(v.view, C.zebkit_permission_result_func(f), d)
	})
}
func (v *View) OnNavigationResult(function func(map[string]any)) {
	v.toldHook("navigation-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_navigation_result(v.view, C.zebkit_navigation_result_func(f), d)
	})
}
func (v *View) OnPopupBlocked(function func(map[string]any)) {
	v.toldHook("popup-blocked", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_popup_blocked(v.view, C.zebkit_popup_blocked_func(f), d)
	})
}
func (v *View) OnWindowOpenEvent(function func(map[string]any)) {
	v.toldHook("window-open-event", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_window_open_event(v.view, C.zebkit_window_open_event_func(f), d)
	})
}
func (v *View) OnConsole(function func(map[string]any)) {
	v.toldHook("console", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_console(v.view, C.zebkit_console_func(f), d)
	})
}
func (v *View) OnCertificateResult(function func(map[string]any)) {
	v.toldHook("certificate-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_certificate_result(v.view, C.zebkit_certificate_result_func(f), d)
	})
}
func (v *View) OnHTTPAuthResult(function func(map[string]any)) {
	v.toldHook("http-auth-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_http_auth_result(v.view, C.zebkit_http_auth_result_func(f), d)
	})
}
func (v *View) OnExternalProtocol(function func(map[string]any)) {
	v.toldHook("external-protocol", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_external_protocol(v.view, C.zebkit_external_protocol_func(f), d)
	})
}
func (v *View) OnDownloadProgress(function func(map[string]any)) {
	v.toldHook("download-progress", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_download_progress(v.view, C.zebkit_download_progress_func(f), d)
	})
}
func (v *View) OnClientCertificateResult(function func(map[string]any)) {
	v.toldHook("client-certificate-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_client_certificate_result(v.view, C.zebkit_client_certificate_result_func(f), d)
	})
}
func (v *View) OnFileChooserResult(function func(map[string]any)) {
	v.toldHook("file-chooser-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_file_chooser_result(v.view, C.zebkit_file_chooser_result_func(f), d)
	})
}
func (v *View) OnEyeDropperResult(function func(map[string]any)) {
	v.toldHook("eye-dropper-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_eye_dropper_result(v.view, C.zebkit_eye_dropper_result_func(f), d)
	})
}
func (v *View) OnDeviceChooserResult(function func(map[string]any)) {
	v.toldHook("device-chooser-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_device_chooser_result(v.view, C.zebkit_device_chooser_result_func(f), d)
	})
}
func (v *View) OnShareResult(function func(map[string]any)) {
	v.toldHook("share-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_share_result(v.view, C.zebkit_share_result_func(f), d)
	})
}
func (v *View) OnMediaAccessResult(function func(map[string]any)) {
	v.toldHook("media-access-result", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_media_access_result(v.view, C.zebkit_media_access_result_func(f), d)
	})
}
func (v *View) OnReservedKey(function func(map[string]any)) {
	v.toldHook("reserved-key", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_reserved_key(v.view, C.zebkit_reserved_key_func(f), d)
	})
}
func (v *View) OnFullscreen(function func(map[string]any)) {
	v.toldHook("fullscreen", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_fullscreen(v.view, C.zebkit_fullscreen_func(f), d)
	})
}
func (v *View) OnNetworkEvent(function func(map[string]any)) {
	v.toldHook("network-event", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_network_event(v.view, C.zebkit_network_event_func(f), d)
	})
}

// PopupPolicy says which windows pages may open.
type PopupPolicy int

const (
	PopupBlockWithoutGesture PopupPolicy = 0
	PopupAllow               PopupPolicy = 1
	PopupBlock               PopupPolicy = 2
)

func (v *View) SetPopupPolicy(policy PopupPolicy) bool {
	return bool(C.zebkit_view_set_popup_policy(v.view, C.zebkit_popup_policy(policy)))
}
func (v *View) PopupPolicy() PopupPolicy { return PopupPolicy(C.zebkit_view_get_popup_policy(v.view)) }

// WindowOpenInfo is how a popup view was opened: its opener, the URL, the
// features, the gesture.
func (v *View) WindowOpenInfo() map[string]any {
	return settingsJSON(C.zebkit_view_get_window_open_info(v.view))
}

// CertificateRequest is a certificate error the application decides on.
type CertificateRequest struct{ HookRequest }

func (r *CertificateRequest) Info() map[string]any {
	return infoJSON(C.zebkit_certificate_request_info((*C.zebkit_certificate_request)(r.ptr)))
}

// Respond allows the certificate for this load, or refuses it.
func (r *CertificateRequest) Respond(allow bool) bool {
	return bool(C.zebkit_certificate_respond((*C.zebkit_certificate_request)(r.ptr), C.bool(allow)))
}

func (v *View) OnCertificate(function func(*CertificateRequest)) {
	v.asked("certificate", wrap(function, func(h HookRequest) *CertificateRequest { return &CertificateRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_certificate(v.view, C.zebkit_certificate_func(f), d) })
}

// HTTPAuthRequest is an HTTP authentication challenge.
type HTTPAuthRequest struct{ HookRequest }

func (r *HTTPAuthRequest) Info() map[string]any {
	return infoJSON(C.zebkit_http_auth_request_info((*C.zebkit_http_auth_request)(r.ptr)))
}

// Respond gives the credentials.
func (r *HTTPAuthRequest) Respond(username, password string) bool {
	u, p := cString(username), cString(password)
	defer free(u)
	defer free(p)
	return bool(C.zebkit_http_auth_respond((*C.zebkit_http_auth_request)(r.ptr), u, p))
}

// Cancel answers with none.
func (r *HTTPAuthRequest) Cancel() bool {
	return bool(C.zebkit_http_auth_respond((*C.zebkit_http_auth_request)(r.ptr), nil, nil))
}

func (v *View) OnHTTPAuth(function func(*HTTPAuthRequest)) {
	v.asked("http-auth", wrap(function, func(h HookRequest) *HTTPAuthRequest { return &HTTPAuthRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_http_auth(v.view, C.zebkit_http_auth_func(f), d) })
}

// DownloadRequest is a download the application saves or cancels.
type DownloadRequest struct{ HookRequest }

func (r *DownloadRequest) Info() map[string]any {
	return infoJSON(C.zebkit_download_request_info((*C.zebkit_download_request)(r.ptr)))
}

// Save saves the download at path, absolute.
func (r *DownloadRequest) Save(path string) error {
	cpath := cString(path)
	defer free(cpath)
	var message *C.char
	if !C.zebkit_download_respond((*C.zebkit_download_request)(r.ptr), C.ZEBKIT_DOWNLOAD_SAVE, cpath, &message) {
		return takeError(message, "the download was not saved")
	}
	return nil
}

// Cancel cancels the download.
func (r *DownloadRequest) Cancel() error {
	var message *C.char
	if !C.zebkit_download_respond((*C.zebkit_download_request)(r.ptr), C.ZEBKIT_DOWNLOAD_CANCEL, nil, &message) {
		return takeError(message, "the download was not cancelled")
	}
	return nil
}

func (v *View) OnDownload(function func(*DownloadRequest)) {
	v.asked("download", wrap(function, func(h HookRequest) *DownloadRequest { return &DownloadRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_download(v.view, C.zebkit_download_func(f), d) })
}

// CancelDownload cancels a download of the view by its downloadId.
func (v *View) CancelDownload(id string) bool {
	cid := cString(id)
	defer free(cid)
	return bool(C.zebkit_view_cancel_download(v.view, cid))
}

// ClientCertificateRequest is a server's request of a client certificate.
type ClientCertificateRequest struct{ HookRequest }

func (r *ClientCertificateRequest) Info() map[string]any {
	return infoJSON(C.zebkit_client_certificate_request_info((*C.zebkit_client_certificate_request)(r.ptr)))
}

func (r *ClientCertificateRequest) respond(action C.zebkit_client_certificate_action, certificate, key *C.char) error {
	var message *C.char
	if !C.zebkit_client_certificate_respond((*C.zebkit_client_certificate_request)(r.ptr), action, certificate, key, &message) {
		return takeError(message, "the answer was refused")
	}
	return nil
}

// None continues without a certificate.
func (r *ClientCertificateRequest) None() error {
	return r.respond(C.ZEBKIT_CLIENT_CERTIFICATE_NONE, nil, nil)
}

// Cancel fails the load.
func (r *ClientCertificateRequest) Cancel() error {
	return r.respond(C.ZEBKIT_CLIENT_CERTIFICATE_CANCEL, nil, nil)
}

// Provide gives a certificate and its private key, PEM.
func (r *ClientCertificateRequest) Provide(certificatePEM, keyPEM string) error {
	c, k := cString(certificatePEM), cString(keyPEM)
	defer free(c)
	defer free(k)
	return r.respond(C.ZEBKIT_CLIENT_CERTIFICATE_PROVIDE, c, k)
}

// Choose gives one of the system store's identities, by its index in the
// metadata's identities.
func (r *ClientCertificateRequest) Choose(index int) error {
	var message *C.char
	if !C.zebkit_client_certificate_choose((*C.zebkit_client_certificate_request)(r.ptr), C.size_t(index), &message) {
		return takeError(message, "the identity was refused")
	}
	return nil
}

func (v *View) OnClientCertificate(function func(*ClientCertificateRequest)) {
	v.asked("client-certificate", wrap(function, func(h HookRequest) *ClientCertificateRequest { return &ClientCertificateRequest{h} }),
		func(f, d unsafe.Pointer) {
			C.zebkit_view_on_client_certificate(v.view, C.zebkit_client_certificate_func(f), d)
		})
}

// AddWorldScript runs source in each new document before the page's own
// scripts, in an isolated world named world ("" for the page's).
func (v *View) AddWorldScript(world, source string, mainFrameOnly bool) error {
	cworld, csource := cString(world), cString(source)
	defer free(cworld)
	defer free(csource)
	var message *C.char
	if !C.zebkit_view_add_world_script(v.view, cworld, csource, C.bool(mainFrameOnly), &message) {
		return takeError(message, "the script was refused")
	}
	return nil
}

// FileChooserRequest is a page's file chooser: <input type=file> or File
// System Access.
type FileChooserRequest struct{ HookRequest }

func (r *FileChooserRequest) Info() map[string]any {
	return infoJSON(C.zebkit_file_chooser_request_info((*C.zebkit_file_chooser_request)(r.ptr)))
}

// Respond gives the page paths, absolute; none cancels.
func (r *FileChooserRequest) Respond(paths []string) error {
	list := make([]*C.char, len(paths)+1)
	for i, path := range paths {
		list[i] = cString(path)
	}
	defer func() {
		for _, p := range list[:len(paths)] {
			free(p)
		}
	}()
	array := C.malloc(C.size_t(len(list)) * C.size_t(unsafe.Sizeof(uintptr(0))))
	defer C.free(array)
	copy(unsafe.Slice((**C.char)(array), len(list)), list)
	var message *C.char
	if !C.zebkit_file_chooser_respond((*C.zebkit_file_chooser_request)(r.ptr), (**C.char)(array), C.size_t(len(paths)), &message) {
		return takeError(message, "the files were refused")
	}
	return nil
}

func (v *View) OnFileChooser(function func(*FileChooserRequest)) {
	v.asked("file-chooser", wrap(function, func(h HookRequest) *FileChooserRequest { return &FileChooserRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_file_chooser(v.view, C.zebkit_file_chooser_func(f), d) })
}

func (r *EyeDropperRequest) Info() map[string]any {
	return infoJSON(C.zebkit_eye_dropper_request_info((*C.zebkit_eye_dropper_request)(r.ptr)))
}

func (r *ShareRequest) Info() map[string]any {
	return infoJSON(C.zebkit_share_request_info((*C.zebkit_share_request)(r.ptr)))
}

// SetReservedKeys keeps keys from pages, as ["Ctrl+Shift+KeyI"]; OnReservedKey
// tells them.
func (v *View) SetReservedKeys(keys []string) error {
	text, err := json.Marshal(keys)
	if err != nil {
		return err
	}
	ctext := cString(string(text))
	defer free(ctext)
	var message *C.char
	if !C.zebkit_view_set_reserved_keys(v.view, ctext, &message) {
		return takeError(message, "the keys were refused")
	}
	return nil
}

// ContextMenuRun runs a default item's command against what was
// right-clicked.
func (v *View) ContextMenuRun(menuID uint64, command string) error {
	ccommand := cString(command)
	defer free(ccommand)
	var message *C.char
	if !C.zebkit_view_context_menu_run(v.view, C.uint64_t(menuID), ccommand, &message) {
		return takeError(message, "the command was refused")
	}
	return nil
}

// ContextMenuShow shows a menu where it was asked for: the default items
// (nil) or a list mixing default ids and the application's own items;
// function gets the id of an item of the application's ("" for none).
func (v *View) ContextMenuShow(menuID uint64, items any, function func(id string)) error {
	var text *C.char
	if items != nil {
		var err error
		if text, err = jsonText(items); err != nil {
			return err
		}
		defer free(text)
	}
	var f C.zebkit_context_menu_item_func
	var d unsafe.Pointer
	if function != nil {
		h := v.slot("context-menu-item").set(function)
		f, d = C.zebkit_context_menu_item_func(C.zka_item), dataA(h)
	}
	var message *C.char
	if !C.zebkit_view_context_menu_show(v.view, C.uint64_t(menuID), text, f, d, &message) {
		return takeError(message, "the menu was refused")
	}
	return nil
}

// FullscreenPolicy says whether pages may go fullscreen.
type FullscreenPolicy int

const (
	FullscreenAllow FullscreenPolicy = 0
	FullscreenDeny  FullscreenPolicy = 1
)

func (v *View) SetFullscreenPolicy(policy FullscreenPolicy) bool {
	return bool(C.zebkit_view_set_fullscreen_policy(v.view, C.zebkit_fullscreen_policy(policy)))
}

// SetNetworkFilter limits which requests OnNetwork is asked about
// (docs/API.md).
func (v *View) SetNetworkFilter(filter any) error {
	text, err := jsonText(filter)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	if !C.zebkit_view_set_network_filter(v.view, text, &message) {
		return takeError(message, "the filter was refused")
	}
	return nil
}

// SetNetworkFilterForWorkers asks about workers' requests too.
func (v *View) SetNetworkFilterForWorkers(on bool) bool {
	return bool(C.zebkit_view_set_network_filter_for_workers(v.view, C.bool(on)))
}

// NetworkRequest is a request the application continues, changes, blocks,
// redirects or answers itself.
type NetworkRequest struct{ HookRequest }

func (r *NetworkRequest) Info() map[string]any {
	return infoJSON(C.zebkit_network_request_info((*C.zebkit_network_request)(r.ptr)))
}

// Respond replies once: {"action": "continue"|"block"|"redirect"|"fulfill",
// ...}, with body for "fulfill".
func (r *NetworkRequest) Respond(reply any, body []byte) error {
	text, err := jsonText(reply)
	if err != nil {
		return err
	}
	defer free(text)
	var p unsafe.Pointer
	if len(body) > 0 {
		p = C.CBytes(body)
		defer C.free(p)
	}
	var message *C.char
	if !C.zebkit_network_respond((*C.zebkit_network_request)(r.ptr), text, p, C.size_t(len(body)), &message) {
		return takeError(message, "the reply was refused")
	}
	return nil
}

// RespondFile fulfils the request with the file at path, absolute; reply
// has status and headers.
func (r *NetworkRequest) RespondFile(reply any, path string) error {
	text, err := jsonText(reply)
	if err != nil {
		return err
	}
	defer free(text)
	cpath := cString(path)
	defer free(cpath)
	var message *C.char
	if !C.zebkit_network_respond_file((*C.zebkit_network_request)(r.ptr), text, cpath, &message) {
		return takeError(message, "the reply was refused")
	}
	return nil
}

// RespondStream fulfils the request with a body written as it comes.
func (r *NetworkRequest) RespondStream(reply any) (*NetworkStream, error) {
	text, err := jsonText(reply)
	if err != nil {
		return nil, err
	}
	defer free(text)
	var message *C.char
	stream := C.zebkit_network_respond_stream((*C.zebkit_network_request)(r.ptr), text, &message)
	if stream == nil {
		return nil, takeError(message, "the reply was refused")
	}
	return &NetworkStream{stream: stream}, nil
}

// NetworkStream is a response body the application writes.
type NetworkStream struct {
	stream   *C.zebkit_network_stream
	writable slot
}

// Write writes bytes; false when the stream is full (wait for OnWritable) or
// closed.
func (s *NetworkStream) Write(bytes []byte) bool {
	if len(bytes) == 0 {
		return bool(C.zebkit_network_stream_write(s.stream, nil, 0))
	}
	p := C.CBytes(bytes)
	defer C.free(p)
	return bool(C.zebkit_network_stream_write(s.stream, p, C.size_t(len(bytes))))
}

// OnWritable calls function when the stream takes bytes again.
func (s *NetworkStream) OnWritable(function func()) {
	if function == nil {
		C.zebkit_network_stream_on_writable(s.stream, nil, nil)
		s.writable.clear()
		return
	}
	h := s.writable.set(function)
	C.zebkit_network_stream_on_writable(s.stream, C.zebkit_network_stream_writable_func(C.zka_writable), dataA(h))
}

// IsOpen says whether the page still reads the stream.
func (s *NetworkStream) IsOpen() bool { return bool(C.zebkit_network_stream_is_open(s.stream)) }

// End ends the body, as a failure with failed.
func (s *NetworkStream) End(failed bool) {
	C.zebkit_network_stream_end(s.stream, C.bool(failed))
	s.writable.clear()
}

// OnNetwork calls function for the requests SetNetworkFilter selects: it is
// asked about nothing until a filter is set. Each request is answered once:
// continue, block, redirect, fulfil, or a stream; nil removes it.
func (v *View) OnNetwork(function func(*NetworkRequest)) {
	v.asked("network", wrap(function, func(h HookRequest) *NetworkRequest { return &NetworkRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_network(v.view, C.zebkit_network_func(f), d) })
}

// OnNetworkBody gets the responses' bodies as they pass: the request's id,
// the bytes, whether it ended and whether it was cut short.
func (v *View) OnNetworkBody(function func(requestID string, bytes []byte, end, truncated bool)) {
	if function == nil {
		C.zebkit_view_on_network_body(v.view, nil, nil)
		v.slot("network-body").clear()
		return
	}
	h := v.slot("network-body").set(function)
	C.zebkit_view_on_network_body(v.view, C.zebkit_network_body_func(C.zka_body), dataA(h))
}
