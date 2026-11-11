package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"runtime"
	"sync/atomic"
	"unsafe"
)

// Displays are the screens: [{id, bounds, workArea, scale, rotation, primary}].
func Displays() ([]map[string]any, error) {
	onEngineThread()
	var value []map[string]any
	return value, takeJSON(C.zebkit_get_displays(), &value, "no displays")
}

var powerSlot, displaysSlot, notificationSlot slot

// OnPowerEvent calls function with {"event": "suspend" | "resume" | "lock" |
// "unlock"} and {"event": "power-source", "onBattery"}; nil stops.
func OnPowerEvent(function func(map[string]any)) {
	onEngineThread()
	if function == nil {
		C.zebkit_on_power_event(nil, nil)
		powerSlot.clear()
		return
	}
	h := powerSlot.set(jsonFunc(function))
	C.zebkit_on_power_event(C.zebkit_system_event_func(C.zk_text), data(h))
}

// OnDisplaysChanged calls function with {"event": "displays", "displays": [...]};
// nil stops.
func OnDisplaysChanged(function func(map[string]any)) {
	onEngineThread()
	if function == nil {
		C.zebkit_on_displays_changed(nil, nil)
		displaysSlot.clear()
		return
	}
	h := displaysSlot.set(jsonFunc(function))
	C.zebkit_on_displays_changed(C.zebkit_system_event_func(C.zk_text), data(h))
}

// PreventSleep keeps the system awake (and the display on, with
// keepDisplayOn) until AllowSleep(token).
func PreventSleep(reason string, keepDisplayOn bool) (uint64, error) {
	onEngineThread()
	creason := cString(reason)
	defer free(creason)
	token := uint64(C.zebkit_prevent_sleep(C.bool(keepDisplayOn), creason))
	if token == 0 {
		return 0, refused("the system was not kept awake")
	}
	return token, nil
}

// AllowSleep ends PreventSleep's token.
func AllowSleep(token uint64) error {
	onEngineThread()
	return check(C.zebkit_allow_sleep(C.uint64_t(token)), "no such token")
}

// OpenExternal gives url to the system's handler, never through a shell.
func OpenExternal(url string) error {
	onEngineThread()
	curl := cString(url)
	defer free(curl)
	var message *C.char
	return failed(C.zebkit_open_external(curl, &message), message, "the URL was not opened")
}

// ShowInFolder shows path, absolute, selected in the file manager.
func ShowInFolder(path string) error {
	onEngineThread()
	cpath := cString(path)
	defer free(cpath)
	var message *C.char
	return failed(C.zebkit_show_in_folder(cpath, &message), message, "the file was not shown")
}

// OnNotification makes the application show the pages' notifications:
// function gets {"event": "show", ...} and {"event": "close", "id"}; nil gives
// them back to the system.
func OnNotification(function func(map[string]any)) {
	onEngineThread()
	if function == nil {
		C.zebkit_on_notification(nil, nil)
		notificationSlot.clear()
		return
	}
	h := notificationSlot.set(jsonFunc(function))
	C.zebkit_on_notification(C.zebkit_notification_func(C.zk_text), data(h))
}

// NotificationClicked tells the page its notification id was clicked
// (action: the button's index, or -1).
func NotificationClicked(id string, action int) error {
	onEngineThread()
	cid := cString(id)
	defer free(cid)
	return check(C.zebkit_notification_clicked(cid, C.int(action)), "no such notification")
}

// NotificationClosed tells the page its notification id was closed.
func NotificationClosed(id string, byUser bool) error {
	onEngineThread()
	cid := cString(id)
	defer free(cid)
	return check(C.zebkit_notification_closed(cid, C.bool(byUser)), "no such notification")
}

var appMenuSlot, dockMenuSlot slot

// menuFunction is the handle of a menu's function, 0 for none.
func menuFunction(function func(id string)) C.uintptr_t {
	if function == nil {
		return 0
	}
	return handle(function)
}

// SetAppMenu sets the application's menu, JSON: a list of {"title", "items"},
// where an item is {"title", "id", "key", "modifiers", "enabled", "checked",
// "radioGroup"}, {"role"}, {"title", "items"} or {"separator": true};
// function gets a chosen item's id. The menu bar on macOS; elsewhere the
// items' keys work in the application's windows.
func SetAppMenu(items any, function func(id string)) error {
	onEngineThread()
	text, err := jsonArg(items)
	if err != nil {
		return err
	}
	defer free(text)
	h := menuFunction(function)
	var message *C.char
	if !C.zebkit_app_set_menu(text, C.zebkit_menu_func(C.zk_text), data(h), &message) {
		// The engine keeps the menu it had, with its function.
		release(h)
		return takeError(message, "the menu was refused")
	}
	appMenuSlot.replace(h)
	return nil
}

// UpdateAppMenuItem changes an item in place, JSON: {"enabled", "checked", "title"}.
func UpdateAppMenuItem(id string, state any) error {
	onEngineThread()
	text, err := jsonArg(state)
	if err != nil {
		return err
	}
	defer free(text)
	cid := cString(id)
	defer free(cid)
	var message *C.char
	return failed(C.zebkit_app_update_menu_item(cid, text, &message), message, "the menu item was not changed")
}

// AppMenu is the menu with its items' state.
func AppMenu() ([]map[string]any, error) {
	onEngineThread()
	var value []map[string]any
	return value, takeJSON(C.zebkit_app_get_menu(), &value, "no menu")
}

// SetAppBadge sets the badge on the application's icon; "" clears it.
func SetAppBadge(text string) error {
	onEngineThread()
	ctext := cString(text)
	defer free(ctext)
	return check(C.zebkit_app_set_badge(ctext), "the badge was refused")
}

// SetAppProgress shows a progress from 0 to 1 on the icon; below 0 clears it.
func SetAppProgress(progress float64) error {
	onEngineThread()
	return check(C.zebkit_app_set_progress(C.double(progress)), "a progress is at most 1")
}

// RequestAppAttention bounces the Dock icon or flashes the taskbar button.
func RequestAppAttention(critical bool) {
	onEngineThread()
	C.zebkit_app_request_attention(C.bool(critical))
}

// SetDockMenu sets the Dock's menu (macOS), JSON as SetAppMenu's; elsewhere it
// is refused.
func SetDockMenu(items any, function func(id string)) error {
	onEngineThread()
	text, err := jsonArg(items)
	if err != nil {
		return err
	}
	defer free(text)
	h := menuFunction(function)
	var message *C.char
	if !C.zebkit_app_set_dock_menu(text, C.zebkit_menu_func(C.zk_text), data(h), &message) {
		release(h)
		return takeError(message, "the Dock's menu was refused")
	}
	dockMenuSlot.replace(h)
	return nil
}

// Tray is an icon in the system tray or the menu bar.
type Tray struct {
	tray        *C.zebkit_tray
	menu, click slot
}

// NewTray shows png (PNG bytes) with tooltip.
func NewTray(png []byte, tooltip string) (*Tray, error) {
	onEngineThread()
	ctooltip := cString(tooltip)
	defer free(ctooltip)
	var message *C.char
	tray := C.zebkit_tray_new((*C.uint8_t)(bytesArg(png)), C.size_t(len(png)), ctooltip, &message)
	if tray == nil {
		return nil, takeError(message, "the tray was refused")
	}
	return &Tray{tray: tray}, nil
}

// SetIcon changes the icon, PNG bytes.
func (t *Tray) SetIcon(png []byte) error {
	if t.tray == nil {
		return ErrClosed
	}
	onEngineThread()
	var message *C.char
	return failed(C.zebkit_tray_set_icon(t.tray, (*C.uint8_t)(bytesArg(png)), C.size_t(len(png)), &message), message,
		"the icon was refused")
}

// SetTooltip changes the tooltip.
func (t *Tray) SetTooltip(tooltip string) {
	if t.tray == nil {
		return
	}
	onEngineThread()
	ctooltip := cString(tooltip)
	defer free(ctooltip)
	C.zebkit_tray_set_tooltip(t.tray, ctooltip)
}

// SetMenu takes SetAppMenu's items; function gets a chosen item's id.
func (t *Tray) SetMenu(items any, function func(id string)) error {
	if t.tray == nil {
		return ErrClosed
	}
	onEngineThread()
	text, err := jsonArg(items)
	if err != nil {
		return err
	}
	defer free(text)
	h := menuFunction(function)
	var message *C.char
	if !C.zebkit_tray_set_menu(t.tray, text, C.zebkit_menu_func(C.zk_text), data(h), &message) {
		release(h)
		return takeError(message, "the menu was refused")
	}
	t.menu.replace(h)
	return nil
}

// OnClick calls function when the icon is clicked; nil stops.
func (t *Tray) OnClick(function func()) {
	if t.tray == nil {
		return
	}
	onEngineThread()
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
	onEngineThread()
	C.zebkit_tray_close(t.tray)
	t.tray = nil
	t.menu.clear()
	t.click.clear()
}

// SchemeFlags say what an application's URL scheme is (RegisterScheme).
type SchemeFlags uint

const (
	SchemeStandard  SchemeFlags = 1 << 0
	SchemeSecure    SchemeFlags = 1 << 1
	SchemeFetch     SchemeFlags = 1 << 2
	SchemeBypassCSP SchemeFlags = 1 << 3
)

// RegisterScheme makes scheme ("app") one of the application's, before Main
// or Init.
func RegisterScheme(scheme string, flags SchemeFlags) error {
	onEngineThread()
	cscheme := cString(scheme)
	defer free(cscheme)
	return check(C.zebkit_scheme_register(cscheme, C.uint(flags)), "the scheme is the engine's already, or it is too late")
}

// schemeSlots, as every slot of the package, is the engine thread's: the
// functions that change it check that thread first.
var schemeSlots = map[string]*slot{}

// HandleScheme answers the scheme's requests with function, on the engine's
// thread; nil stops (requests then fail as not found).
func HandleScheme(scheme string, function func(*Request)) {
	onEngineThread()
	cscheme := cString(scheme)
	defer free(cscheme)
	s, ok := schemeSlots[scheme]
	if !ok {
		s = &slot{}
		schemeSlots[scheme] = s
	}
	if function == nil {
		C.zebkit_scheme_handle(cscheme, nil, nil)
		s.clear()
		delete(schemeSlots, scheme)
		return
	}
	h := s.set(function)
	C.zebkit_scheme_handle(cscheme, C.zebkit_scheme_func(C.zk_scheme), data(h))
}

// Request is a request to an application's scheme. Answer it once, with
// Respond or RespondFile, from any goroutine; its methods do nothing once it
// is answered. A Request the program lets go unanswered is answered 404 once
// the garbage collector finds it, as Rust's on Drop and Python's when it is
// collected, through Post on the engine's thread; that can be late, or never
// (no collection, or the engine stopped first), so answer every request.
type Request struct {
	request unsafe.Pointer // *C.zebkit_request, atomically
}

func newRequest(request *C.zebkit_request) *Request {
	r := &Request{}
	atomic.StorePointer(&r.request, unsafe.Pointer(request))
	runtime.SetFinalizer(r, (*Request).dropped)
	return r
}

// dropped answers a Request let go unanswered: not found.
func (r *Request) dropped() {
	request := (*C.zebkit_request)(atomic.SwapPointer(&r.request, nil))
	if request == nil {
		return
	}
	// Post fails only once the engine has stopped, and the request with it.
	_ = Post(func() { C.zebkit_request_respond(request, 404, nil, nil, 0) })
}

func (r *Request) load() *C.zebkit_request {
	return (*C.zebkit_request)(atomic.LoadPointer(&r.request))
}

// URL is what was asked for.
func (r *Request) URL() string {
	if request := r.load(); request != nil {
		return goString(C.zebkit_request_url(request))
	}
	return ""
}

// Method is the request's method.
func (r *Request) Method() string {
	if request := r.load(); request != nil {
		return goString(C.zebkit_request_method(request))
	}
	return ""
}

// Headers are the request's headers: "Name: value" lines separated by "\r\n".
func (r *Request) Headers() string {
	if request := r.load(); request != nil {
		return goString(C.zebkit_request_headers(request))
	}
	return ""
}

// Body is the body sent with the request, or nil.
func (r *Request) Body() []byte {
	request := r.load()
	if request == nil {
		return nil
	}
	var size C.size_t
	body := C.zebkit_request_body(request, &size)
	return goBytes(unsafe.Pointer(body), size)
}

// Respond answers with status, headers ("Name: value" lines separated by
// "\r\n", or "") and body; ErrClosed once answered.
func (r *Request) Respond(status int, headers string, body []byte) error {
	request := (*C.zebkit_request)(atomic.SwapPointer(&r.request, nil))
	if request == nil {
		return ErrClosed
	}
	cheaders := optionalString(headers)
	defer free(cheaders)
	C.zebkit_request_respond(request, C.int(status), cheaders, bytesArg(body), C.size_t(len(body)))
	return nil
}

// RespondFile answers with the file at path, in ranges for media; ErrClosed
// once answered.
func (r *Request) RespondFile(path, headers string) error {
	request := (*C.zebkit_request)(atomic.SwapPointer(&r.request, nil))
	if request == nil {
		return ErrClosed
	}
	cpath := cString(path)
	defer free(cpath)
	cheaders := optionalString(headers)
	defer free(cheaders)
	C.zebkit_request_respond_file(request, cpath, cheaders)
	return nil
}
