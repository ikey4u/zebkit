package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"unsafe"
)

// HookRequest is what a page or the engine asks the application: its
// metadata, as the engine gave it when asking. Answer it once, on the
// engine's thread; an unanswered request ends at its deadline.
type HookRequest struct {
	ptr      unsafe.Pointer
	JSON     string
	Metadata map[string]any
}

func newHookRequest(ptr unsafe.Pointer, text string) HookRequest {
	request := HookRequest{ptr: ptr, JSON: text}
	_ = json.Unmarshal([]byte(text), &request.Metadata)
	return request
}

func infoJSON(text *C.char) map[string]any {
	var value map[string]any
	if !takeJSON(text, &value) {
		return nil
	}
	return value
}

// PermissionState is a permission's decision.
type PermissionState int

const (
	PermissionPrompt  PermissionState = 0
	PermissionGranted PermissionState = 1
	PermissionDenied  PermissionState = 2
)

// SetPermission sets the decision for permission ("geolocation",
// "notifications", "camera", ...) of origin ("" for the page's).
func (v *View) SetPermission(origin, permission string, state PermissionState) error {
	corigin := optionalString(origin)
	defer free(corigin)
	cpermission := cString(permission)
	defer free(cpermission)
	var message *C.char
	if !C.zebkit_view_set_permission(v.view, corigin, cpermission, C.zebkit_permission_state(state), &message) {
		return takeError(message, "the permission was refused")
	}
	return nil
}

func optionalString(text string) *C.char {
	if text == "" {
		return nil
	}
	return cString(text)
}

// PermissionRequest is a page's request of a permission.
type PermissionRequest struct{ HookRequest }

func (r *PermissionRequest) Info() map[string]any {
	return infoJSON(C.zebkit_permission_request_info((*C.zebkit_permission_request)(r.ptr)))
}

// Respond decides; remember keeps the decision for the origin.
func (r *PermissionRequest) Respond(state PermissionState, remember bool) bool {
	return bool(C.zebkit_permission_respond((*C.zebkit_permission_request)(r.ptr), C.zebkit_permission_state(state), C.bool(remember)))
}

// OnPermission calls function for each permission a page asks for; nil
// removes it (the view's table then decides).
func (v *View) OnPermission(function func(*PermissionRequest)) {
	v.asked("permission", wrap(function, func(h HookRequest) *PermissionRequest { return &PermissionRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_permission(v.view, C.zebkit_permission_func(f), d) })
}

// NavigationAction is a navigation's decision.
type NavigationAction int

const (
	NavigationContinue NavigationAction = 0
	NavigationCancel   NavigationAction = 1
	NavigationRedirect NavigationAction = 2
)

// NavigationRequest is a navigation about to happen.
type NavigationRequest struct{ HookRequest }

func (r *NavigationRequest) Info() map[string]any {
	return infoJSON(C.zebkit_navigation_request_info((*C.zebkit_navigation_request)(r.ptr)))
}

// Respond continues, cancels, or redirects to replacementURL.
func (r *NavigationRequest) Respond(action NavigationAction, replacementURL string) error {
	url := optionalString(replacementURL)
	defer free(url)
	var message *C.char
	if !C.zebkit_navigation_respond((*C.zebkit_navigation_request)(r.ptr), C.zebkit_navigation_action(action), url, &message) {
		return takeError(message, "the answer was refused")
	}
	return nil
}

// OnNavigation calls function before each navigation; nil removes it.
func (v *View) OnNavigation(function func(*NavigationRequest)) {
	v.asked("navigation", wrap(function, func(h HookRequest) *NavigationRequest { return &NavigationRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_navigation(v.view, C.zebkit_navigation_func(f), d) })
}

// MediaAccessRequest is a page's request of the camera, the microphone, or
// (Metadata["type"] == "display") a screen to capture.
type MediaAccessRequest struct{ HookRequest }

func (r *MediaAccessRequest) Info() map[string]any {
	return infoJSON(C.zebkit_media_access_request_info((*C.zebkit_media_access_request)(r.ptr)))
}

// Respond allows or refuses; the device ids are those of the metadata ("" for
// the page's choice). For display capture, videoDevice is the source's id
// ("view" or "screen") and any audioDevice gives the view's sound.
func (r *MediaAccessRequest) Respond(allow bool, audioDevice, videoDevice string) bool {
	audio, video := optionalString(audioDevice), optionalString(videoDevice)
	defer free(audio)
	defer free(video)
	return bool(C.zebkit_media_access_respond((*C.zebkit_media_access_request)(r.ptr), C.bool(allow), audio, video))
}

// OnMediaAccess calls function for getUserMedia() and getDisplayMedia().
func (v *View) OnMediaAccess(function func(*MediaAccessRequest)) {
	v.asked("media-access", wrap(function, func(h HookRequest) *MediaAccessRequest { return &MediaAccessRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_media_access(v.view, C.zebkit_media_access_func(f), d) })
}

// EyeDropperRequest is a page's EyeDropper.open().
type EyeDropperRequest struct{ HookRequest }

// Respond gives the page color ("#rrggbb"), or "" to cancel.
func (r *EyeDropperRequest) Respond(color string) bool {
	c := optionalString(color)
	defer free(c)
	return bool(C.zebkit_eye_dropper_respond((*C.zebkit_eye_dropper_request)(r.ptr), c))
}

// OnEyeDropper calls function for the page's EyeDropper.open(); without one
// it is cancelled.
func (v *View) OnEyeDropper(function func(*EyeDropperRequest)) {
	v.asked("eye-dropper", wrap(function, func(h HookRequest) *EyeDropperRequest { return &EyeDropperRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_eye_dropper(v.view, C.zebkit_eye_dropper_func(f), d) })
}

// ShareRequest is a page's navigator.share().
type ShareRequest struct{ HookRequest }

// Respond says whether it was shared.
func (r *ShareRequest) Respond(shared bool) bool {
	return bool(C.zebkit_share_respond((*C.zebkit_share_request)(r.ptr), C.bool(shared)))
}

// OnShare calls function for the page's navigator.share().
func (v *View) OnShare(function func(*ShareRequest)) {
	v.asked("share", wrap(function, func(h HookRequest) *ShareRequest { return &ShareRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_share(v.view, C.zebkit_share_func(f), d) })
}

// DeviceChooserRequest is a page's USB, HID or serial chooser:
// Metadata["kind"], and Metadata["devices"] to choose from.
type DeviceChooserRequest struct{ HookRequest }

// Info has the device list as it is now.
func (r *DeviceChooserRequest) Info() map[string]any {
	return infoJSON(C.zebkit_device_chooser_request_info((*C.zebkit_device_chooser_request)(r.ptr)))
}

// Respond gives the page the device of that id, or none for "".
func (r *DeviceChooserRequest) Respond(deviceID string) bool {
	id := optionalString(deviceID)
	defer free(id)
	return bool(C.zebkit_device_chooser_respond((*C.zebkit_device_chooser_request)(r.ptr), id))
}

// OnDeviceChooser calls function for the page's device choosers; without one
// the page gets no device.
func (v *View) OnDeviceChooser(function func(*DeviceChooserRequest)) {
	v.asked("device-chooser", wrap(function, func(h HookRequest) *DeviceChooserRequest { return &DeviceChooserRequest{h} }),
		func(f, d unsafe.Pointer) { C.zebkit_view_on_device_chooser(v.view, C.zebkit_device_chooser_func(f), d) })
}

// OnContextMenu tells function what was right-clicked and the default
// items; while set, the engine shows no menu itself.
func (v *View) OnContextMenu(function func(map[string]any)) {
	v.told("context-menu", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_context_menu(v.view, C.zebkit_context_menu_func(f), d)
	})
}

// wrap turns a typed request function into the bridge's.
func wrap[T any](function func(T), make func(HookRequest) T) func(unsafe.Pointer, string) {
	if function == nil {
		return nil
	}
	return func(ptr unsafe.Pointer, text string) { function(make(newHookRequest(ptr, text))) }
}
