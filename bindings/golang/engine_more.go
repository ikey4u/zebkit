package zebkit

/*
#include <stdlib.h>
#include "bridge_a.h"

static void *zka_data(uintptr_t handle) { return (void *)handle; }
*/
import "C"

import (
	"encoding/json"
	"os"
	"runtime/cgo"
	"time"
	"unsafe"
)

func dataA(h C.uintptr_t) unsafe.Pointer { return C.zka_data(h) }

func jsonText(value any) (*C.char, error) {
	if text, ok := value.(string); ok {
		return cString(text), nil
	}
	text, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}
	return cString(string(text)), nil
}

var wakeupSlot slot

// SetWakeup has the engine ask for a Pump when work is due, for a program
// that pumps from its own loop: function gets how long until then. It may run
// on any thread: only schedule a Pump there, never call the engine. Before
// Init.
func SetWakeup(function func(delay time.Duration)) bool {
	if function == nil {
		return ClearWakeup()
	}
	h := handle(function)
	if !C.zebkit_set_wakeup(C.zebkit_wakeup_func(C.zka_wakeup), dataA(h)) {
		cgo.Handle(h).Delete()
		return false
	}
	wakeupSlot.clear()
	wakeupSlot.h = h
	return true
}

// ClearWakeup removes SetWakeup's function, before Init.
func ClearWakeup() bool {
	if !C.zebkit_set_wakeup(nil, nil) {
		return false
	}
	wakeupSlot.clear()
	return true
}

// InitWithSettings is Init with the engine's startup settings (docs/API.md),
// as JSON or a value that encodes to it.
func InitWithSettings(settings any, args ...string) error {
	text, err := jsonText(settings)
	if err != nil {
		return err
	}
	defer free(text)
	argc, argv, done := argv(args)
	defer done()
	var message *C.char
	if C.zebkit_init_with_settings(argc, argv, text, &message) != 0 {
		return takeError(message, "the engine did not start")
	}
	return nil
}

// CookieStorage is where the engine keeps the key of its cookies.
type CookieStorage int

const (
	// CookieStorageOS is the system's key store, the default.
	CookieStorageOS CookieStorage = 0
	// CookieStorageFile is a private key file of the engine's.
	CookieStorageFile CookieStorage = 1
)

// SetCookieStorage chooses where the cookies' key is kept; keyDirectory is
// for CookieStorageFile ("" for the profile's). Before starting.
func SetCookieStorage(storage CookieStorage, keyDirectory string) bool {
	directory := optionalString(keyDirectory)
	defer free(directory)
	return bool(C.zebkit_set_cookie_storage(C.zebkit_cookie_storage(storage), directory))
}

// SetCookieEncryptionKey makes the application keep the cookies' key: 32
// random bytes it stores and gives again each run; nil restores the system's
// key store. Before starting.
func SetCookieEncryptionKey(key []byte) bool {
	if key == nil {
		return bool(C.zebkit_set_cookie_encryption_key(nil, 0))
	}
	p, n, done := bytesPointer(key)
	defer done()
	return bool(C.zebkit_set_cookie_encryption_key(p, n))
}

var openFilesSlot, reopenSlot, quitRequestSlot slot

// AppOnOpenFiles calls function with the files the system asks the running
// application to open (macOS: the Finder and the Dock).
func AppOnOpenFiles(function func(paths []string)) {
	if function == nil {
		C.zebkit_app_on_open_files(nil, nil)
		openFilesSlot.clear()
		return
	}
	h := openFilesSlot.set(func(text string) {
		var paths []string
		_ = json.Unmarshal([]byte(text), &paths)
		function(paths)
	})
	C.zebkit_app_on_open_files(C.zebkit_paths_func(C.zk_json), dataA(h))
}

// AppOnReopen calls function when the Dock's icon is clicked with no window
// shown (macOS).
func AppOnReopen(function func()) {
	if function == nil {
		C.zebkit_app_on_reopen(nil, nil)
		reopenSlot.clear()
		return
	}
	h := reopenSlot.set(function)
	C.zebkit_app_on_reopen(C.zebkit_app_func(C.zk_task), dataA(h))
}

// AppOnQuitRequest calls function when the system asks the application to
// quit (macOS: the menu, the Dock, logging out); the application decides.
func AppOnQuitRequest(function func()) {
	if function == nil {
		C.zebkit_app_on_quit_request(nil, nil)
		quitRequestSlot.clear()
		return
	}
	h := quitRequestSlot.set(function)
	C.zebkit_app_on_quit_request(C.zebkit_app_func(C.zk_task), dataA(h))
}

// Context is a browser context of its own: cookies, storage and cache kept
// apart from the default one's, in memory.
type Context struct{ context *C.zebkit_context }

// NewContext makes a context.
func NewContext() *Context { return &Context{context: C.zebkit_context_new()} }

// Close closes the context; its views are closed first.
func (c *Context) Close() {
	if c.context != nil {
		C.zebkit_context_close(c.context)
		c.context = nil
	}
}

// NewView makes a view in the context.
func (c *Context) NewView(url string, width, height int) *View {
	var curl *C.char
	if url != "" {
		curl = cString(url)
		defer free(curl)
	}
	return newView(C.zebkit_view_new_in(c.context, curl, C.int(width), C.int(height)))
}

// NewViewWithOptions makes a view in the context with its window's options.
func (c *Context) NewViewWithOptions(url string, options any) (*View, error) {
	text, err := jsonText(options)
	if err != nil {
		return nil, err
	}
	defer free(text)
	curl := cString(url)
	defer free(curl)
	var message *C.char
	view := C.zebkit_view_new_with_options(c.context, curl, text, &message)
	if view == nil {
		return nil, takeError(message, "the window options were refused")
	}
	return newView(view), nil
}

// TargetID is the view's page as CDP names it (Target.targetId).
func (v *View) TargetID() string { return takeString(C.zebkit_view_get_target_id(v.view)) }

// SetSizeLimits sets the window's smallest size and whether it can be resized.
func (v *View) SetSizeLimits(minWidth, minHeight int, resizable bool) {
	C.zebkit_view_set_size_limits(v.view, C.int(minWidth), C.int(minHeight), C.bool(resizable))
}

// Attach puts the view inside parent's window at x, y, width by height.
func (v *View) Attach(parent *View, x, y, width, height int) {
	C.zebkit_view_attach(v.view, parent.view, C.int(x), C.int(y), C.int(width), C.int(height))
}

func (v *View) SetBounds(x, y, width, height int) {
	C.zebkit_view_set_bounds(v.view, C.int(x), C.int(y), C.int(width), C.int(height))
}

func (v *View) SetVisible(visible bool) { C.zebkit_view_set_visible(v.view, C.bool(visible)) }

// AttachNative puts the view inside a window of the application's: an X11
// window id, or an NSView* on macOS, or an HWND on Windows.
func (v *View) AttachNative(parent uintptr, x, y, width, height int) error {
	var message *C.char
	if !C.zebkit_view_attach_native(v.view, C.uintptr_t(parent), C.int(x), C.int(y), C.int(width), C.int(height), &message) {
		return takeError(message, "the view was not attached")
	}
	return nil
}

// DetachNative takes the view out of the application's window.
func (v *View) DetachNative() bool { return bool(C.zebkit_view_detach_native(v.view)) }

// NativeWindowKind is the kind of a system window's handle.
type NativeWindowKind int

const (
	NativeWindowNone   NativeWindowKind = 0
	NativeWindowX11    NativeWindowKind = 1
	NativeWindowWin32  NativeWindowKind = 2
	NativeWindowAppKit NativeWindowKind = 3
)

// NativeWindow is the system window the page is in, borrowed: an X11 window
// id, an HWND or an NSWindow*. false without one (headless, off-screen,
// Wayland).
func (v *View) NativeWindow() (NativeWindowKind, uintptr, bool) {
	var window C.zebkit_native_window
	if !C.zebkit_view_get_native_window(v.view, &window) {
		return NativeWindowNone, 0, false
	}
	return NativeWindowKind(window.kind), uintptr(window.handle), true
}

// ExportNativeWindow gives done the window as the system's portals name a
// parent window, "x11:<XID>" or "wayland:<handle>", or ok false.
func (v *View) ExportNativeWindow(done func(handle string, ok bool)) bool {
	var h C.uintptr_t
	h = handle(func(text string, ok bool) {
		cgo.Handle(h).Delete()
		done(text, ok)
	})
	if !C.zebkit_view_export_native_window(v.view, C.zebkit_native_window_export_func(C.zka_export), dataA(h)) {
		cgo.Handle(h).Delete()
		return false
	}
	return true
}

// SetUserAgentOverride sets the user agent, its client hints and platform
// at once (docs/API.md); done gets the state applied, or why not.
func (v *View) SetUserAgentOverride(options any, done func(result string, err error)) bool {
	text, err := jsonText(options)
	if err != nil {
		if done != nil {
			done("", err)
		}
		return false
	}
	defer free(text)
	var h C.uintptr_t
	h = handle(func(result, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if done == nil {
			return
		}
		if failed {
			done("", &Error{failure})
		} else {
			done(result, nil)
		}
	})
	if !C.zebkit_view_set_user_agent_override(v.view, text, C.zebkit_result_func(C.zk_result), dataA(h)) {
		cgo.Handle(h).Delete()
		return false
	}
	return true
}

// OnNewWindow takes the URL of a window a page opens instead of opening it.
func (v *View) OnNewWindow(function func(url string)) {
	if function == nil {
		C.zebkit_view_on_new_window(v.view, nil, nil)
		v.slot("new-window").clear()
		return
	}
	h := v.slot("new-window").set(function)
	C.zebkit_view_on_new_window(v.view, C.zebkit_url_func(C.zk_title), dataA(h))
}

// OnWindowOpen gets a window a page opens as a view of its own, still linked
// to its opener; the application shows it and closes it.
func (v *View) OnWindowOpen(function func(opened *View, url string)) {
	if function == nil {
		C.zebkit_view_on_window_open(v.view, nil, nil)
		v.slot("window-open").clear()
		return
	}
	h := v.slot("window-open").set(func(opened *C.zebkit_view, url string) { function(newView(opened), url) })
	C.zebkit_view_on_window_open(v.view, C.zebkit_window_open_func(C.zka_window_open), dataA(h))
}

// OnDrop gets the paths of files dropped on the view, before the page.
func (v *View) OnDrop(function func(paths []string)) {
	if function == nil {
		C.zebkit_view_on_drop(v.view, nil, nil)
		v.slot("drop").clear()
		return
	}
	h := v.slot("drop").set(func(text string) {
		var paths []string
		_ = json.Unmarshal([]byte(text), &paths)
		function(paths)
	})
	C.zebkit_view_on_drop(v.view, C.zebkit_drop_func(C.zk_title), dataA(h))
}

// OnAuthentication shows WebAuthn's requests in the application's own UI:
// function gets each event (docs/PASSKEYS.md); nil gives the engine's UI back.
func (v *View) OnAuthentication(function func(event map[string]any)) bool {
	if function == nil {
		ok := C.zebkit_view_on_authentication(v.view, nil, nil)
		v.slot("authentication").clear()
		return bool(ok)
	}
	h := v.slot("authentication").set(jsonFunc(function))
	return bool(C.zebkit_view_on_authentication(v.view, C.zebkit_authentication_func(C.zk_title), dataA(h)))
}

// AuthenticationAction answers the current WebAuthn request.
func (v *View) AuthenticationAction(action any) bool {
	text, err := jsonText(action)
	if err != nil {
		return false
	}
	defer free(text)
	return bool(C.zebkit_view_authentication_action(v.view, text))
}

// SetDragRegions sets the regions the window is dragged from, for the
// "host" drag source: [{x, y, width, height, drag}] in content DIPs.
func (v *View) SetDragRegions(regions any) error {
	text, err := jsonText(regions)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	if !C.zebkit_view_set_drag_regions(v.view, text, &message) {
		return takeError(message, "the drag regions were refused")
	}
	return nil
}

// SetPopupWindowOptions sets the options of the windows the view's pages
// open; nil for the system's frame.
func (v *View) SetPopupWindowOptions(options any) error {
	var text *C.char
	if options != nil {
		var err error
		if text, err = jsonText(options); err != nil {
			return err
		}
		defer free(text)
	}
	var message *C.char
	if !C.zebkit_view_set_popup_window_options(v.view, text, &message) {
		return takeError(message, "the popup window options were refused")
	}
	return nil
}

// ResolveJSON answers with the page's JSON as it was written, bytes and all.
func (c *Call) ResolveJSON(value []byte) {
	if len(value) == 0 {
		c.Resolve("null")
		return
	}
	p := C.CBytes(value)
	defer C.free(p)
	C.zebkit_call_resolve_json(c.call, (*C.char)(p), C.size_t(len(value)))
}

// CDPListen serves the Chrome DevTools Protocol at address ("" for
// 127.0.0.1) and port (0 picks one); it returns the port.
func CDPListen(address string, port uint) (uint, error) {
	caddress := optionalString(address)
	defer free(caddress)
	var message *C.char
	got := uint(C.zebkit_cdp_listen(caddress, C.uint(port), &message))
	if got == 0 {
		return 0, takeError(message, "CDP did not listen")
	}
	return got, nil
}

// CDPPort is the port CDP is served on, 0 when it is not.
func CDPPort() uint { return uint(C.zebkit_cdp_get_port()) }

// CDPURL is CDP's browser WebSocket URL, "" when it is not served.
func CDPURL() string { return takeString(C.zebkit_cdp_get_url()) }

// CDPStop stops serving CDP.
func CDPStop() { C.zebkit_cdp_stop() }

// HelperMain runs the engine's child process when this program is one of
// them (a program that is its own helper); it returns its exit code. Call it
// first in main with the program's arguments.
func HelperMain() int {
	argc, argv, done := argv(os.Args[1:])
	defer done()
	return int(C.zebkit_helper_main(argc, argv))
}
