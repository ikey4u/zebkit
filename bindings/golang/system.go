package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"unsafe"
)

// Displays are the screens: [{id, bounds, workArea, scale, rotation, primary}].
func Displays() []map[string]any {
	var value []map[string]any
	takeJSON(C.zebkit_get_displays(), &value)
	return value
}

var powerSlot, displaysSlot, notificationSlot slot

// OnPowerEvent calls function with {"event": "suspend" | "resume" | "lock" |
// "unlock"} and {"event": "power-source", "onBattery"}; nil stops.
func OnPowerEvent(function func(map[string]any)) {
	if function == nil {
		C.zebkit_on_power_event(nil, nil)
		powerSlot.clear()
		return
	}
	h := powerSlot.set(jsonFunc(function))
	C.zebkit_on_power_event(C.zebkit_system_event_func(C.zk_json), data(h))
}

// OnDisplaysChanged calls function with {"event": "displays", "displays": [...]}.
func OnDisplaysChanged(function func(map[string]any)) {
	if function == nil {
		C.zebkit_on_displays_changed(nil, nil)
		displaysSlot.clear()
		return
	}
	h := displaysSlot.set(jsonFunc(function))
	C.zebkit_on_displays_changed(C.zebkit_system_event_func(C.zk_json), data(h))
}

// PreventSleep keeps the system awake (and the display on, with
// keepDisplayOn) until AllowSleep(token).
func PreventSleep(reason string, keepDisplayOn bool) (uint64, error) {
	creason := cString(reason)
	defer free(creason)
	token := uint64(C.zebkit_prevent_sleep(C.bool(keepDisplayOn), creason))
	if token == 0 {
		return 0, &Error{"the system was not kept awake"}
	}
	return token, nil
}

func AllowSleep(token uint64) bool { return bool(C.zebkit_allow_sleep(C.uint64_t(token))) }

// OpenExternal gives url to the system's handler, never through a shell.
func OpenExternal(url string) error {
	curl := cString(url)
	defer free(curl)
	var message *C.char
	if !C.zebkit_open_external(curl, &message) {
		return takeError(message, "the URL was not opened")
	}
	return nil
}

// ShowInFolder shows path, absolute, selected in the file manager.
func ShowInFolder(path string) error {
	cpath := cString(path)
	defer free(cpath)
	var message *C.char
	if !C.zebkit_show_in_folder(cpath, &message) {
		return takeError(message, "the file was not shown")
	}
	return nil
}

// OnNotification makes the application show the pages' notifications:
// function gets {"event": "show", ...} and {"event": "close", "id"}; nil gives
// them back to the system.
func OnNotification(function func(map[string]any)) {
	if function == nil {
		C.zebkit_on_notification(nil, nil)
		notificationSlot.clear()
		return
	}
	h := notificationSlot.set(jsonFunc(function))
	C.zebkit_on_notification(C.zebkit_notification_func(C.zk_json), data(h))
}

// NotificationClicked tells the page its notification id was clicked
// (action: the button's index, or -1).
func NotificationClicked(id string, action int) bool {
	cid := cString(id)
	defer free(cid)
	return bool(C.zebkit_notification_clicked(cid, C.int(action)))
}

func NotificationClosed(id string, byUser bool) bool {
	cid := cString(id)
	defer free(cid)
	return bool(C.zebkit_notification_closed(cid, C.bool(byUser)))
}

var appMenuSlot, dockMenuSlot slot

func menuJSON(items any) (*C.char, error) {
	if text, ok := items.(string); ok {
		return cString(text), nil
	}
	text, err := json.Marshal(items)
	if err != nil {
		return nil, err
	}
	return cString(string(text)), nil
}

// AppSetMenu sets the application's menu: a list of {"title", "items"},
// where an item is {"title", "id", "key", "modifiers", "enabled", "checked",
// "radioGroup"}, {"role"}, {"title", "items"} or {"separator": true};
// function gets a chosen item's id. The menu bar on macOS; elsewhere the
// items' keys work in the application's windows.
func AppSetMenu(items any, function func(id string)) error {
	text, err := menuJSON(items)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	h := appMenuSlot.set(function)
	if !C.zebkit_app_set_menu(text, C.zebkit_menu_func(C.zk_menu), data(h), &message) {
		return takeError(message, "the menu was refused")
	}
	return nil
}

// AppUpdateMenuItem changes an item in place: {"enabled", "checked", "title"}.
func AppUpdateMenuItem(id string, state map[string]any) error {
	text, err := json.Marshal(state)
	if err != nil {
		return err
	}
	cid, cstate := cString(id), cString(string(text))
	defer free(cid)
	defer free(cstate)
	var message *C.char
	if !C.zebkit_app_update_menu_item(cid, cstate, &message) {
		return takeError(message, "the menu item was not changed")
	}
	return nil
}

// AppMenu is the menu with its items' state.
func AppMenu() []map[string]any {
	var value []map[string]any
	takeJSON(C.zebkit_app_get_menu(), &value)
	return value
}

// AppSetBadge sets the badge on the application's icon; "" clears it.
func AppSetBadge(text string) bool {
	ctext := cString(text)
	defer free(ctext)
	return bool(C.zebkit_app_set_badge(ctext))
}

// AppSetProgress shows a progress from 0 to 1 on the icon; below 0 clears it.
func AppSetProgress(progress float64) bool {
	return bool(C.zebkit_app_set_progress(C.double(progress)))
}

// AppRequestAttention bounces the Dock icon or flashes the taskbar button.
func AppRequestAttention(critical bool) { C.zebkit_app_request_attention(C.bool(critical)) }

// AppSetDockMenu sets the Dock's menu (macOS); elsewhere it is refused.
func AppSetDockMenu(items any, function func(id string)) error {
	text, err := menuJSON(items)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	h := dockMenuSlot.set(function)
	if !C.zebkit_app_set_dock_menu(text, C.zebkit_menu_func(C.zk_menu), data(h), &message) {
		return takeError(message, "the Dock's menu was refused")
	}
	return nil
}

// Tray is an icon in the system tray or the menu bar.
type Tray struct {
	tray        *C.zebkit_tray
	menu, click slot
}

func bytesPointer(b []byte) (*C.uint8_t, C.size_t, func()) {
	if len(b) == 0 {
		return nil, 0, func() {}
	}
	p := C.CBytes(b)
	return (*C.uint8_t)(p), C.size_t(len(b)), func() { C.free(p) }
}

// NewTray shows png (PNG bytes) with tooltip.
func NewTray(png []byte, tooltip string) (*Tray, error) {
	p, n, done := bytesPointer(png)
	defer done()
	ctooltip := cString(tooltip)
	defer free(ctooltip)
	var message *C.char
	tray := C.zebkit_tray_new(p, n, ctooltip, &message)
	if tray == nil {
		return nil, takeError(message, "the tray was refused")
	}
	return &Tray{tray: tray}, nil
}

func (t *Tray) SetIcon(png []byte) error {
	p, n, done := bytesPointer(png)
	defer done()
	var message *C.char
	if !C.zebkit_tray_set_icon(t.tray, p, n, &message) {
		return takeError(message, "the icon was refused")
	}
	return nil
}

func (t *Tray) SetTooltip(tooltip string) {
	ctooltip := cString(tooltip)
	defer free(ctooltip)
	C.zebkit_tray_set_tooltip(t.tray, ctooltip)
}

// SetMenu takes AppSetMenu's items; function gets a chosen item's id.
func (t *Tray) SetMenu(items any, function func(id string)) error {
	text, err := menuJSON(items)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	h := t.menu.set(function)
	if !C.zebkit_tray_set_menu(t.tray, text, C.zebkit_menu_func(C.zk_menu), data(h), &message) {
		return takeError(message, "the menu was refused")
	}
	return nil
}

// OnClick calls function when the icon is clicked.
func (t *Tray) OnClick(function func()) {
	if function == nil {
		C.zebkit_tray_on_click(t.tray, nil, nil)
		t.click.clear()
		return
	}
	h := t.click.set(function)
	C.zebkit_tray_on_click(t.tray, C.zebkit_app_func(C.zk_task), data(h))
}

// Close removes the icon.
func (t *Tray) Close() {
	if t.tray == nil {
		return
	}
	C.zebkit_tray_close(t.tray)
	t.tray = nil
	t.menu.clear()
	t.click.clear()
}

// SchemeFlags say what an application's URL scheme is (SchemeRegister).
type SchemeFlags uint

const (
	SchemeStandard  SchemeFlags = 1 << 0
	SchemeSecure    SchemeFlags = 1 << 1
	SchemeFetch     SchemeFlags = 1 << 2
	SchemeBypassCSP SchemeFlags = 1 << 3
)

// SchemeRegister makes scheme ("app") one of the application's, before Main
// or Init.
func SchemeRegister(scheme string, flags SchemeFlags) bool {
	cscheme := cString(scheme)
	defer free(cscheme)
	return bool(C.zebkit_scheme_register(cscheme, C.uint(flags)))
}

var schemeSlots = map[string]*slot{}

// SchemeHandle answers the scheme's requests with function, on the engine's
// thread.
func SchemeHandle(scheme string, function func(*Request)) {
	s, ok := schemeSlots[scheme]
	if !ok {
		s = &slot{}
		schemeSlots[scheme] = s
	}
	h := s.set(function)
	cscheme := cString(scheme)
	defer free(cscheme)
	C.zebkit_scheme_handle(cscheme, C.zebkit_scheme_func(C.zk_scheme), data(h))
}

// Request is a request to an application's scheme; respond to it once.
type Request struct{ request *C.zebkit_request }

func (r *Request) URL() string     { return goString(C.zebkit_request_url(r.request)) }
func (r *Request) Method() string  { return goString(C.zebkit_request_method(r.request)) }
func (r *Request) Headers() string { return goString(C.zebkit_request_headers(r.request)) }

func (r *Request) Body() []byte {
	var size C.size_t
	body := C.zebkit_request_body(r.request, &size)
	if body == nil || size == 0 {
		return nil
	}
	return C.GoBytes(unsafe.Pointer(body), C.int(size))
}

// Respond answers with status, headers ("Name: value" lines separated by
// "\r\n", or "") and body.
func (r *Request) Respond(status int, headers string, body []byte) {
	cheaders := optionalString(headers)
	defer free(cheaders)
	var p unsafe.Pointer
	if len(body) > 0 {
		p = C.CBytes(body)
		defer C.free(p)
	}
	C.zebkit_request_respond(r.request, C.int(status), cheaders, p, C.size_t(len(body)))
}

// RespondFile answers with the file at path, in ranges for media.
func (r *Request) RespondFile(path, headers string) {
	cpath := cString(path)
	defer free(cpath)
	cheaders := optionalString(headers)
	defer free(cheaders)
	C.zebkit_request_respond_file(r.request, cpath, cheaders)
}
