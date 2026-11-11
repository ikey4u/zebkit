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
// engine's thread, now or later; an unanswered request ends at its deadline.
// Once answered, its methods return ErrClosed.
type HookRequest struct {
	ptr      unsafe.Pointer
	text     string
	metadata map[string]any
	decoded  bool
}

func newHookRequest(ptr unsafe.Pointer, text string) HookRequest {
	return HookRequest{ptr: ptr, text: text}
}

// Metadata is the request as the engine asked it (<zebkit/hooks.h> has its
// members per hook); nil when it was not a JSON object.
func (r *HookRequest) Metadata() map[string]any {
	if !r.decoded {
		r.decoded = true
		_ = json.Unmarshal([]byte(r.text), &r.metadata)
	}
	return r.metadata
}

// pointer is the engine's request, or ErrClosed once answered.
func (r *HookRequest) pointer() (unsafe.Pointer, error) {
	if r.ptr == nil {
		return nil, ErrClosed
	}
	onEngineThread()
	return r.ptr, nil
}

// answer answers the request with respond, which says whether the engine
// took the answer; the request is closed once it did.
func (r *HookRequest) answer(respond func(p unsafe.Pointer) error) error {
	p, err := r.pointer()
	if err != nil {
		return err
	}
	if err := respond(p); err != nil {
		return err
	}
	r.ptr = nil
	return nil
}

// info decodes a request's current state, from info.
func (r *HookRequest) info(info func(p unsafe.Pointer) *C.char) (map[string]any, error) {
	p, err := r.pointer()
	if err != nil {
		return nil, err
	}
	var value map[string]any
	return value, takeJSON(info(p), &value, "the request is gone")
}

// PermissionState is a permission's decision.
type PermissionState int

const (
	PermissionInvalid PermissionState = -1 // what View.Permission cannot say
	PermissionPrompt  PermissionState = 0
	PermissionGranted PermissionState = 1
	PermissionDenied  PermissionState = 2
)

// SetPermission sets the decision for permission ("geolocation",
// "notifications", "camera", ...) of origin ("" for the page's).
func (v *View) SetPermission(origin, permission string, state PermissionState) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	corigin := optionalString(origin)
	defer free(corigin)
	cpermission := cString(permission)
	defer free(cpermission)
	var message *C.char
	return failed(C.zebkit_view_set_permission(raw, corigin, cpermission, C.zebkit_permission_state(state), &message),
		message, "the permission was refused")
}

// PermissionRequest is a page's request of a permission.
type PermissionRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *PermissionRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_permission_request_info((*C.zebkit_permission_request)(p))
	})
}

// Respond decides; remember keeps the decision for the origin.
func (r *PermissionRequest) Respond(state PermissionState, remember bool) error {
	return r.answer(func(p unsafe.Pointer) error {
		return check(C.zebkit_permission_respond((*C.zebkit_permission_request)(p), C.zebkit_permission_state(state), C.bool(remember)),
			"the answer was refused")
	})
}

// OnPermission calls function for each permission a page asks for; nil
// removes it (the view's table then decides).
func (v *View) OnPermission(function func(*PermissionRequest)) {
	v.asked("permission", wrap(function, func(h HookRequest) *PermissionRequest { return &PermissionRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_permission(raw, C.zebkit_permission_func(f), d)
		})
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

// Info is the request as it is now.
func (r *NavigationRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_navigation_request_info((*C.zebkit_navigation_request)(p))
	})
}

// Respond continues, cancels, or redirects to replacementURL.
func (r *NavigationRequest) Respond(action NavigationAction, replacementURL string) error {
	return r.answer(func(p unsafe.Pointer) error {
		url := optionalString(replacementURL)
		defer free(url)
		var message *C.char
		return failed(C.zebkit_navigation_respond((*C.zebkit_navigation_request)(p), C.zebkit_navigation_action(action), url, &message),
			message, "the answer was refused")
	})
}

// OnNavigation calls function before each navigation; nil removes it.
func (v *View) OnNavigation(function func(*NavigationRequest)) {
	v.asked("navigation", wrap(function, func(h HookRequest) *NavigationRequest { return &NavigationRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_navigation(raw, C.zebkit_navigation_func(f), d)
		})
}

// MediaAccessRequest is a page's request of the camera, the microphone, or
// (Metadata()["type"] == "display") a screen to capture.
type MediaAccessRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *MediaAccessRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_media_access_request_info((*C.zebkit_media_access_request)(p))
	})
}

// Respond allows or refuses; the device ids are those of the metadata ("" for
// the page's choice). For display capture, videoDevice is the source's id
// ("view" or "screen") and any audioDevice gives the view's sound.
func (r *MediaAccessRequest) Respond(allow bool, audioDevice, videoDevice string) error {
	return r.answer(func(p unsafe.Pointer) error {
		audio, video := optionalString(audioDevice), optionalString(videoDevice)
		defer free(audio)
		defer free(video)
		return check(C.zebkit_media_access_respond((*C.zebkit_media_access_request)(p), C.bool(allow), audio, video),
			"the answer was refused")
	})
}

// OnMediaAccess calls function for getUserMedia() and getDisplayMedia(); nil
// removes it.
func (v *View) OnMediaAccess(function func(*MediaAccessRequest)) {
	v.asked("media-access", wrap(function, func(h HookRequest) *MediaAccessRequest { return &MediaAccessRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_media_access(raw, C.zebkit_media_access_func(f), d)
		})
}

// EyeDropperRequest is a page's EyeDropper.open().
type EyeDropperRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *EyeDropperRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_eye_dropper_request_info((*C.zebkit_eye_dropper_request)(p))
	})
}

// Respond gives the page color ("#rrggbb"), or "" to cancel.
func (r *EyeDropperRequest) Respond(color string) error {
	return r.answer(func(p unsafe.Pointer) error {
		c := optionalString(color)
		defer free(c)
		return check(C.zebkit_eye_dropper_respond((*C.zebkit_eye_dropper_request)(p), c), "the answer was refused")
	})
}

// OnEyeDropper calls function for the page's EyeDropper.open(); without one
// it is cancelled.
func (v *View) OnEyeDropper(function func(*EyeDropperRequest)) {
	v.asked("eye-dropper", wrap(function, func(h HookRequest) *EyeDropperRequest { return &EyeDropperRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_eye_dropper(raw, C.zebkit_eye_dropper_func(f), d)
		})
}

// ShareRequest is a page's navigator.share().
type ShareRequest struct{ HookRequest }

// Info is the request as it is now.
func (r *ShareRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_share_request_info((*C.zebkit_share_request)(p))
	})
}

// Respond says whether it was shared.
func (r *ShareRequest) Respond(shared bool) error {
	return r.answer(func(p unsafe.Pointer) error {
		return check(C.zebkit_share_respond((*C.zebkit_share_request)(p), C.bool(shared)), "the answer was refused")
	})
}

// OnShare calls function for the page's navigator.share(); nil removes it.
func (v *View) OnShare(function func(*ShareRequest)) {
	v.asked("share", wrap(function, func(h HookRequest) *ShareRequest { return &ShareRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) { C.zebkit_view_on_share(raw, C.zebkit_share_func(f), d) })
}

// DeviceChooserRequest is a page's USB, HID or serial chooser:
// Metadata()["kind"], and Metadata()["devices"] to choose from.
type DeviceChooserRequest struct{ HookRequest }

// Info has the device list as it is now.
func (r *DeviceChooserRequest) Info() (map[string]any, error) {
	return r.info(func(p unsafe.Pointer) *C.char {
		return C.zebkit_device_chooser_request_info((*C.zebkit_device_chooser_request)(p))
	})
}

// Respond gives the page the device of that id, or none for "".
func (r *DeviceChooserRequest) Respond(deviceID string) error {
	return r.answer(func(p unsafe.Pointer) error {
		id := optionalString(deviceID)
		defer free(id)
		return check(C.zebkit_device_chooser_respond((*C.zebkit_device_chooser_request)(p), id), "the answer was refused")
	})
}

// OnDeviceChooser calls function for the page's device choosers; without one
// the page gets no device.
func (v *View) OnDeviceChooser(function func(*DeviceChooserRequest)) {
	v.asked("device-chooser", wrap(function, func(h HookRequest) *DeviceChooserRequest { return &DeviceChooserRequest{h} }),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_view_on_device_chooser(raw, C.zebkit_device_chooser_func(f), d)
		})
}

// OnContextMenu tells function what was right-clicked and the default
// items ({"menuId", "x", "y", "items", ...}); while set, the engine shows no
// menu itself.
func (v *View) OnContextMenu(function func(map[string]any)) {
	v.told("context-menu", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_context_menu(raw, C.zebkit_context_menu_func(f), d)
	})
}

// wrap turns a typed request function into the bridge's; nil stays nil.
func wrap[T any](function func(T), make func(HookRequest) T) func(unsafe.Pointer, string) {
	if function == nil {
		return nil
	}
	return func(ptr unsafe.Pointer, text string) { function(make(newHookRequest(ptr, text))) }
}
