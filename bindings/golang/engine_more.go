package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"bytes"
	"encoding/json"
	"os"
	"strings"
	"time"
	"unsafe"
)

var wakeupSlot slot

// SetWakeup has the engine ask for a Pump when work is due, for a program
// that pumps from its own loop: function gets how long until then. It may run
// on any thread: only schedule a Pump there, never call the engine. Before
// Init; nil removes it.
func SetWakeup(function func(delay time.Duration)) error {
	onEngineThread()
	if function == nil {
		return ClearWakeup()
	}
	h := handle(function)
	if !C.zebkit_set_wakeup(C.zebkit_wakeup_func(C.zk_wakeup), data(h)) {
		release(h)
		return refused("the wakeup is set before the engine starts")
	}
	wakeupSlot.replace(h)
	return nil
}

// ClearWakeup removes SetWakeup's function, before Init.
func ClearWakeup() error {
	onEngineThread()
	if !C.zebkit_set_wakeup(nil, nil) {
		return refused("the wakeup is set before the engine starts")
	}
	wakeupSlot.clear()
	return nil
}

// InitWithSettings is Init with the engine's startup settings (docs/API.md),
// JSON; nil for none.
func InitWithSettings(settings any, args ...string) error {
	if err := checkVersion(); err != nil {
		return err
	}
	text, err := optionalJSON(settings)
	if err != nil {
		return err
	}
	defer free(text)
	becomeEngineThread()
	argc, argv, done := cStrings(append([]string{os.Args[0]}, args...))
	defer done()
	var message *C.char
	if C.zebkit_init_with_settings(argc, argv, text, &message) != 0 {
		return takeError(message, "the engine did not start")
	}
	if message != nil {
		C.zebkit_free(unsafe.Pointer(message))
	}
	started.Store(true)
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
func SetCookieStorage(storage CookieStorage, keyDirectory string) error {
	onEngineThread()
	directory := optionalString(keyDirectory)
	defer free(directory)
	return check(C.zebkit_set_cookie_storage(C.zebkit_cookie_storage(storage), directory),
		"the cookie storage is chosen before the engine starts")
}

// SetCookieEncryptionKey makes the application keep the cookies' key: 32
// random bytes it stores and gives again each run; nil restores the system's
// key store. Before starting.
func SetCookieEncryptionKey(key []byte) error {
	onEngineThread()
	return check(C.zebkit_set_cookie_encryption_key((*C.uint8_t)(bytesArg(key)), C.size_t(len(key))),
		"the cookie key is 32 bytes, set before the engine starts")
}

var openFilesSlot, reopenSlot, quitRequestSlot slot

// OnAppOpenFiles calls function with the files the system asks the running
// application to open (macOS: the Finder and the Dock); nil stops.
func OnAppOpenFiles(function func(paths []string)) {
	onEngineThread()
	if function == nil {
		C.zebkit_app_on_open_files(nil, nil)
		openFilesSlot.clear()
		return
	}
	h := openFilesSlot.set(jsonFunc(function))
	C.zebkit_app_on_open_files(C.zebkit_paths_func(C.zk_text), data(h))
}

// OnAppReopen calls function when the Dock's icon is clicked with no window
// shown (macOS); nil stops.
func OnAppReopen(function func()) {
	onEngineThread()
	if function == nil {
		C.zebkit_app_on_reopen(nil, nil)
		reopenSlot.clear()
		return
	}
	h := reopenSlot.set(function)
	C.zebkit_app_on_reopen(C.zebkit_app_func(C.zk_task), data(h))
}

// OnAppQuitRequest calls function when the system asks the application to
// quit (macOS: the menu, the Dock, logging out); the application decides.
func OnAppQuitRequest(function func()) {
	onEngineThread()
	if function == nil {
		C.zebkit_app_on_quit_request(nil, nil)
		quitRequestSlot.clear()
		return
	}
	h := quitRequestSlot.set(function)
	C.zebkit_app_on_quit_request(C.zebkit_app_func(C.zk_task), data(h))
}

// Context is a browser context of its own: cookies, storage and cache kept
// apart from the default one's, in memory.
type Context struct{ context *C.zebkit_context }

// NewContext makes a context.
func NewContext() (*Context, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	context := C.zebkit_context_new()
	if context == nil {
		return nil, refused("no context")
	}
	return &Context{context: context}, nil
}

func (c *Context) raw() (*C.zebkit_context, bool) {
	if c == nil || c.context == nil || !started.Load() {
		return nil, false
	}
	onEngineThread()
	return c.context, true
}

// Close closes the context; its views are closed first.
func (c *Context) Close() {
	if raw, ok := c.raw(); ok {
		C.zebkit_context_close(raw)
		c.context = nil
	}
}

// NewView makes a view in the context.
func (c *Context) NewView(url string, width, height int) (*View, error) {
	raw, ok := c.raw()
	if !ok {
		return nil, ErrClosed
	}
	curl := optionalString(url)
	defer free(curl)
	view := newView(C.zebkit_view_new_in(raw, curl, C.int(width), C.int(height)))
	if view == nil {
		return nil, refused("no view")
	}
	return view, nil
}

// NewViewWithOptions makes a view in the context with its window's options,
// JSON.
func (c *Context) NewViewWithOptions(url string, options any) (*View, error) {
	raw, ok := c.raw()
	if !ok {
		return nil, ErrClosed
	}
	return newViewWithOptions(raw, url, options)
}

// TargetID is the view's page as CDP names it (Target.targetId).
func (v *View) TargetID() string {
	if raw, ok := v.raw(); ok {
		return takeString(C.zebkit_view_get_target_id(raw))
	}
	return ""
}

// SetSizeLimits sets the window's smallest size and whether it can be resized.
func (v *View) SetSizeLimits(minWidth, minHeight int, resizable bool) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_size_limits(raw, C.int(minWidth), C.int(minHeight), C.bool(resizable))
	}
}

// Attach puts the view inside parent's window at x, y, width by height. It
// is refused for the view itself as parent, and for a parent without a
// window to be in.
func (v *View) Attach(parent *View, x, y, width, height int) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	praw, ok := parent.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_attach(raw, praw, C.int(x), C.int(y), C.int(width), C.int(height)),
		"the view was not attached: it is the parent, or the parent has no window to be in")
}

// SetBounds places an attached view in its parent's window.
func (v *View) SetBounds(x, y, width, height int) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_bounds(raw, C.int(x), C.int(y), C.int(width), C.int(height))
	}
}

// SetVisible shows or hides the view.
func (v *View) SetVisible(visible bool) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_visible(raw, C.bool(visible))
	}
}

// AttachNative puts the view inside a window of the application's: an X11
// window id, or an NSView* on macOS, or an HWND on Windows.
func (v *View) AttachNative(parent uintptr, x, y, width, height int) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	var message *C.char
	return failed(C.zebkit_view_attach_native(raw, C.uintptr_t(parent), C.int(x), C.int(y), C.int(width), C.int(height), &message),
		message, "the view was not attached")
}

// DetachNative takes the view out of the application's window.
func (v *View) DetachNative() error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_detach_native(raw), "the view is not attached to a window of the application's")
}

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
	raw, ok := v.raw()
	if !ok {
		return NativeWindowNone, 0, false
	}
	var window C.zebkit_native_window
	if !C.zebkit_view_get_native_window(raw, &window) {
		return NativeWindowNone, 0, false
	}
	return NativeWindowKind(window.kind), uintptr(window.handle), true
}

// ExportNativeWindow gives done the window as the system's portals name a
// parent window, "x11:<XID>" or "wayland:<handle>", or ok false when there is
// none (headless, off-screen, a compositor without xdg-foreign; on Windows
// and macOS always, where NativeWindow is the handle). done is called once,
// later, on the engine's thread; a view that closes first calls it with ok
// false before OnClose's function. An error, and no call, for a closed view.
func (v *View) ExportNativeWindow(done func(handle string, ok bool)) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	h := onceHandle(func(text string, ok bool) {
		if done != nil {
			done(text, ok)
		}
	})
	if !C.zebkit_view_export_native_window(raw, C.zebkit_native_window_export_func(C.zk_export), data(h)) {
		release(h)
		return refused("the window cannot be exported")
	}
	return nil
}

// SetUserAgentOverride sets the user agent, its client hints and platform
// at once, JSON (docs/API.md); done gets the state applied ({}), or why not.
// It returns an error, and done is never called, only when the engine cannot
// take it at all: a closed view (ErrClosed), or the engine's refusal. Options
// it cannot read (not JSON, a zero byte, too long, no userAgent) are told
// once, to done.
func (v *View) SetUserAgentOverride(options any, done func(result json.RawMessage, err error)) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	var text *C.char
	err := refused("the user agent options hold a zero byte")
	if !holdsZero(options) {
		text, err = jsonArg(options)
	}
	if err != nil {
		// Told after this returns, as the engine tells its own answers.
		if done != nil {
			told := err
			if Post(func() { done(nil, told) }) != nil {
				done(nil, told)
			}
		}
		return nil
	}
	defer free(text)
	h := resultOnce(done)
	if !C.zebkit_view_set_user_agent_override(raw, text, C.zebkit_result_func(C.zk_result), data(h)) {
		release(h)
		return refused("the user agent override was refused")
	}
	return nil
}

// holdsZero says whether JSON text given as it is holds a zero byte, which
// would cut it short.
func holdsZero(value any) bool {
	switch v := value.(type) {
	case string:
		return strings.IndexByte(v, 0) >= 0
	case []byte:
		return bytes.IndexByte(v, 0) >= 0
	case json.RawMessage:
		return bytes.IndexByte(v, 0) >= 0
	}
	return false
}

// OnNewWindow takes the URL of a window a page opens instead of opening it;
// nil stops.
func (v *View) OnNewWindow(function func(url string)) {
	v.hook("new-window", function, function, unsafe.Pointer(C.zk_view_text), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_new_window(raw, C.zebkit_url_func(f), d)
	})
}

// OnWindowOpen gets a window a page opens as a view of its own, still linked
// to its opener; the application shows it and closes it. nil stops.
func (v *View) OnWindowOpen(function func(opened *View, url string)) {
	var value func(*C.zebkit_view, string)
	if function != nil {
		value = func(opened *C.zebkit_view, url string) { function(newView(opened), url) }
	}
	v.hook("window-open", function, value, unsafe.Pointer(C.zk_window_open), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_window_open(raw, C.zebkit_window_open_func(f), d)
	})
}

// OnDrop gets the paths of files dropped on the view, before the page; nil
// stops.
func (v *View) OnDrop(function func(paths []string)) {
	var value func(string)
	if function != nil {
		value = jsonFunc(function)
	}
	v.hook("drop", function, value, unsafe.Pointer(C.zk_view_text), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_drop(raw, C.zebkit_drop_func(f), d)
	})
}

// OnAuthentication shows WebAuthn's requests in the application's own UI:
// function gets each event (docs/notes/passkeys.md); nil gives the engine's UI back.
func (v *View) OnAuthentication(function func(event map[string]any)) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	if function == nil {
		ok := C.zebkit_view_on_authentication(raw, nil, nil)
		v.slot("authentication").clear()
		return check(ok, "the authentication hook was refused")
	}
	h := handle(jsonFunc(function))
	if !C.zebkit_view_on_authentication(raw, C.zebkit_authentication_func(C.zk_view_text), data(h)) {
		release(h)
		return refused("the authentication hook was refused")
	}
	v.slot("authentication").replace(h)
	return nil
}

// AuthenticationAction answers the current WebAuthn request, JSON.
func (v *View) AuthenticationAction(action any) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := jsonArg(action)
	if err != nil {
		return err
	}
	defer free(text)
	return check(C.zebkit_view_authentication_action(raw, text), "the authentication action was refused")
}

// SetDragRegions sets the regions the window is dragged from, for the
// "host" drag source: [{x, y, width, height, drag}] in content DIPs, JSON.
func (v *View) SetDragRegions(regions any) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := optionalJSON(regions)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_view_set_drag_regions(raw, text, &message), message, "the drag regions were refused")
}

// SetPopupWindowOptions sets the options of the windows the view's pages
// open, JSON; nil for the system's frame.
func (v *View) SetPopupWindowOptions(options any) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text, err := optionalJSON(options)
	if err != nil {
		return err
	}
	defer free(text)
	var message *C.char
	return failed(C.zebkit_view_set_popup_window_options(raw, text, &message), message, "the popup window options were refused")
}

// ListenCDP serves the Chrome DevTools Protocol at address ("" for
// 127.0.0.1) and port (0 picks one); it returns the port. It waits, on the
// engine's thread, for the server to bind (up to ten seconds; ListenCDPAsync
// does not). Served already, it returns the port; asking then for another
// address or port is an error.
func ListenCDP(address string, port uint) (uint, error) {
	onEngineThread()
	caddress := optionalString(address)
	defer free(caddress)
	var message *C.char
	got := uint(C.zebkit_cdp_listen(caddress, C.uint(port), &message))
	if got == 0 {
		return 0, takeError(message, "CDP did not listen")
	}
	return got, nil
}

// ListenCDPAsync is ListenCDP without waiting for the server to bind: done
// gets the port and the browser target's WebSocket address (CDPURL), or the
// error, once, on the engine's thread after it returns. Calls while it
// starts are answered together; StopCDP answers them with an error. Asking
// for another address or port than the one served is an error. done may be
// nil.
func ListenCDPAsync(address string, port uint, done func(port uint, url string, err error)) {
	onEngineThread()
	caddress := optionalString(address)
	defer free(caddress)
	h := onceHandle(func(port uint, url string, err error) {
		if done != nil {
			done(port, url, err)
		}
	})
	C.zebkit_cdp_listen_async(caddress, C.uint(port), C.zebkit_cdp_listen_func(C.zk_cdp_listen), data(h))
}

// CDPPort is the port CDP is served on, 0 when it is not.
func CDPPort() uint {
	onEngineThread()
	return uint(C.zebkit_cdp_get_port())
}

// CDPURL is CDP's browser WebSocket URL, "" when it is not served.
func CDPURL() string {
	onEngineThread()
	return takeString(C.zebkit_cdp_get_url())
}

// StopCDP stops serving CDP.
func StopCDP() {
	onEngineThread()
	C.zebkit_cdp_stop()
}

// HelperMain runs the engine's child process when this program is one of
// them (a program that is its own helper); it returns its exit code. Call it
// first in main with the program's arguments.
func HelperMain() int {
	argc, argv, done := cStrings(os.Args)
	defer done()
	return int(C.zebkit_helper_main(argc, argv))
}
