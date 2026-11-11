package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"sync/atomic"
	"unsafe"
)

// LoadEvent is a step of a page's load.
type LoadEvent int

const (
	LoadStarted   LoadEvent = 0
	LoadCommitted LoadEvent = 1
	LoadFinished  LoadEvent = 2
	LoadFailed    LoadEvent = 3
)

// Allow is what a page of a granted origin may call (View.Grant).
type Allow uint

const (
	AllowApp      Allow = 1 << 0
	AllowFetch    Allow = 1 << 1
	AllowFiles    Allow = 1 << 2
	AllowCompress Allow = 1 << 3
	AllowCrypto   Allow = 1 << 4
	AllowImage    Allow = 1 << 5
	AllowURL      Allow = 1 << 6
	AllowPDF      Allow = 1 << 7
	AllowAll      Allow = 0xff
)

// View is a web page, in a window or off-screen. There is one View for each
// of the engine's views, however it was got (made, opened by a page, found
// by its page id). It lives until it is closed: by Close, by the user closing
// its window, by its page's window.close(), by its context closing, or by
// Quit; OnClose's function is told, and the view's methods do nothing from
// then on (those that return an error return ErrClosed).
//
// The View learns that its view closed through the engine's on_close
// callback, which a view has one of and the binding takes; a Script's views
// (zebkit_script, <zebkit/zebkit.h>) have it too. The two do not share a
// view: a View got for a Script's view (ViewByPageID) takes the callback
// from the Script, and a Script that takes a view of the binding's as its
// own (by its page id) takes it from the View, which is then not told when
// the view closes. The binding checks a View's page id each time it hands one out, so
// that it never gives a View of a view gone for another made at the same
// address; a View kept by the application across that is the application's
// to stop using.
type View struct {
	view    *C.zebkit_view
	pageID  string
	closed  atomic.Bool
	onClose func()
	slots   map[string]*slot
	binds   map[string]*slot
}

// views are the engine's views that have a View, on the engine's thread.
var views = map[*C.zebkit_view]*View{}

// pageIDOf is the engine's id of raw's page ("p2"), unique to that view and
// never given to another: what tells a View's view from a later one at the
// same address.
func pageIDOf(raw *C.zebkit_view) string {
	return takeString(C.zebkit_automation_page_id(raw))
}

// newView is the View of an engine view: the one it has, or a new one,
// which learns when the engine closes it.
func newView(raw *C.zebkit_view) *View {
	if raw == nil {
		return nil
	}
	id := pageIDOf(raw)
	if v, ok := views[raw]; ok {
		if v.pageID == id {
			// The same view (closing, after Close, or not).
			return v
		}
		// The View of a view that closed without the binding being told
		// (its on_close taken over) and whose address the engine has given
		// to this one: never handed out again.
		v.forget(false)
	}
	v := &View{view: raw, pageID: id, slots: map[string]*slot{}, binds: map[string]*slot{}}
	views[raw] = v
	// The binding's own: the last use of the view, whoever closes it.
	C.zebkit_view_on_close(raw, C.zebkit_view_func(C.zk_view_closed), nil)
	return v
}

// viewClosed is the engine's last word on a view: its View is closed, and
// the functions the engine held for it are let go.
func viewClosed(raw *C.zebkit_view) {
	if v, ok := views[raw]; ok {
		v.forget(true)
	}
}

// forget takes the View out of the registry, closes it and lets its
// functions go; tell calls OnClose's function, when the engine said the view
// closed.
func (v *View) forget(tell bool) {
	if views[v.view] == v {
		delete(views, v.view)
	}
	v.closed.Store(true)
	onClose := v.onClose
	v.onClose = nil
	if tell && onClose != nil {
		onClose()
	}
	v.view = nil
	slots, binds := v.slots, v.binds
	v.slots, v.binds = map[string]*slot{}, map[string]*slot{}
	letGo := func() {
		for _, s := range slots {
			s.clear()
		}
		for _, s := range binds {
			s.clear()
		}
	}
	// The page's last automation events can come after this, as the view
	// goes (<zebkit/automation.h>): its functions go after the task that
	// closes it.
	if Post(letGo) != nil {
		letGo()
	}
}

// raw is the engine's view, or false once it is closed; it panics off the
// engine's thread.
func (v *View) raw() (*C.zebkit_view, bool) {
	if v == nil || v.closed.Load() || !started.Load() {
		return nil, false
	}
	onEngineThread()
	if v.view == nil {
		return nil, false
	}
	return v.view, true
}

// IsClosed says whether the view is closed (or closing, after Close); any
// goroutine may ask.
func (v *View) IsClosed() bool { return v == nil || v.closed.Load() }

// NewView makes a view loading url, width by height; "" loads nothing.
func NewView(url string, width, height int) (*View, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	curl := optionalString(url)
	defer free(curl)
	view := newView(C.zebkit_view_new(curl, C.int(width), C.int(height)))
	if view == nil {
		return nil, refused("no view")
	}
	return view, nil
}

// NewViewWithOptions makes a view whose window is made with options, JSON
// (<zebkit/window.h>): {"width", "height", "frame": "overlay", ...}; nil for
// the defaults.
func NewViewWithOptions(url string, options any) (*View, error) {
	return newViewWithOptions(nil, url, options)
}

func newViewWithOptions(context *C.zebkit_context, url string, options any) (*View, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	text, err := optionalJSON(options)
	if err != nil {
		return nil, err
	}
	defer free(text)
	curl := optionalString(url)
	defer free(curl)
	var message *C.char
	view := C.zebkit_view_new_with_options(context, curl, text, &message)
	if view == nil {
		return nil, takeError(message, "the window options were refused")
	}
	return newView(view), nil
}

func (v *View) slot(name string) *slot {
	if s, ok := v.slots[name]; ok {
		return s
	}
	s := &slot{}
	v.slots[name] = s
	return s
}

// hook installs value, made from function, as the view's callback name,
// through set with the C trampoline and the handle; a nil function removes
// it.
func (v *View) hook(name string, function, value any, trampoline unsafe.Pointer,
	set func(raw *C.zebkit_view, f, d unsafe.Pointer)) {
	raw, ok := v.raw()
	if !ok {
		return
	}
	if isNil(function) {
		set(raw, nil, nil)
		v.slot(name).clear()
		return
	}
	h := v.slot(name).set(value)
	set(raw, trampoline, data(h))
}

// told installs function as a hook that tells the application something as
// JSON; nil removes it.
func (v *View) told(name string, function func(map[string]any), set func(raw *C.zebkit_view, f, d unsafe.Pointer)) {
	var value func(string)
	if function != nil {
		value = jsonFunc(function)
	}
	v.hook(name, function, value, unsafe.Pointer(C.zk_view_text), set)
}

// asked installs function as a hook the application answers; nil removes it.
func (v *View) asked(name string, function func(request unsafe.Pointer, json string),
	set func(raw *C.zebkit_view, f, d unsafe.Pointer)) {
	v.hook(name, function, function, unsafe.Pointer(C.zk_asked), set)
}

// Load loads url.
func (v *View) Load(url string) {
	if raw, ok := v.raw(); ok {
		curl := cString(url)
		defer free(curl)
		C.zebkit_view_load_url(raw, curl)
	}
}

// Reload loads the page again.
func (v *View) Reload() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_reload(raw)
	}
}

// Back goes back in the view's history.
func (v *View) Back() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_go_back(raw)
	}
}

// Forward goes forward in the view's history.
func (v *View) Forward() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_go_forward(raw)
	}
}

// Stop stops the page's load.
func (v *View) Stop() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_stop(raw)
	}
}

// Focus gives the view's window, and the page, the keyboard's focus.
func (v *View) Focus() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_focus(raw)
	}
}

// IsLoading says whether the page is loading.
func (v *View) IsLoading() bool {
	raw, ok := v.raw()
	return ok && bool(C.zebkit_view_is_loading(raw))
}

// Resize resizes the view's content to width by height.
func (v *View) Resize(width, height int) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_resize(raw, C.int(width), C.int(height))
	}
}

// URL is the page's committed URL.
func (v *View) URL() string {
	if raw, ok := v.raw(); ok {
		return takeString(C.zebkit_view_get_url(raw))
	}
	return ""
}

// Title is the page's title.
func (v *View) Title() string {
	if raw, ok := v.raw(); ok {
		return takeString(C.zebkit_view_get_title(raw))
	}
	return ""
}

// SetTitle sets the window's title.
func (v *View) SetTitle(title string) {
	if raw, ok := v.raw(); ok {
		t := cString(title)
		defer free(t)
		C.zebkit_view_set_title(raw, t)
	}
}

// AddScript runs source in every document the view loads from now on,
// before the page's own scripts.
func (v *View) AddScript(source string) {
	if raw, ok := v.raw(); ok {
		s := cString(source)
		defer free(s)
		C.zebkit_view_add_script(raw, s)
	}
}

// SetUserAgent sets the user agent the view's requests and pages see.
func (v *View) SetUserAgent(agent string) {
	if raw, ok := v.raw(); ok {
		a := cString(agent)
		defer free(a)
		C.zebkit_view_set_user_agent(raw, a)
	}
}

// Close closes the view; OnClose's function follows, from the engine's loop,
// and the view's functions are let go then.
func (v *View) Close() {
	raw, ok := v.raw()
	if !ok {
		return
	}
	v.closed.Store(true)
	C.zebkit_view_close(raw)
}

// Evaluate runs script in the page; done gets its result as JSON, or the
// error the page threw.
func (v *View) Evaluate(script string, done func(result json.RawMessage, err error)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	cscript := cString(script)
	defer free(cscript)
	C.zebkit_view_evaluate(raw, cscript, C.zebkit_result_func(C.zk_result), data(resultOnce(done)))
}

// Capture takes a PNG of what the view shows.
func (v *View) Capture(done func(png []byte, err error)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	C.zebkit_view_capture(raw, C.zebkit_capture_func(C.zk_bytes), data(bytesOnce(done)))
}

// OnLoad calls function at each step of a page's load; nil stops.
func (v *View) OnLoad(function func(event LoadEvent, url string)) {
	v.hook("load", function, function, unsafe.Pointer(C.zk_load), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_load(raw, C.zebkit_load_func(f), d)
	})
}

// OnTitle calls function with the page's title when it changes; nil stops.
func (v *View) OnTitle(function func(title string)) {
	v.hook("title", function, function, unsafe.Pointer(C.zk_view_text), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_title(raw, C.zebkit_title_func(f), d)
	})
}

// OnClose calls function when the view closes, however it closes: the last
// call of the view; nil stops.
func (v *View) OnClose(function func()) {
	if v.IsClosed() {
		return
	}
	onEngineThread()
	v.onClose = function
}

// Grant gives pages of origin what allow says: the zebkit object and its
// APIs. filesRoot is the directory zebkit.fs works in, or "".
func (v *View) Grant(origin string, allow Allow, filesRoot string) {
	raw, ok := v.raw()
	if !ok {
		return
	}
	corigin := cString(origin)
	defer free(corigin)
	root := optionalString(filesRoot)
	defer free(root)
	C.zebkit_view_grant(raw, corigin, C.uint(allow), root)
}

// Call is a page's call of a function the application bound: answer it
// once, now or later, from any goroutine (the answer reaches the page
// through the engine's thread). A call never answered stays in the engine's
// memory until it stops.
type Call struct {
	call   unsafe.Pointer // *C.zebkit_call, atomically
	Origin string
	Args   json.RawMessage // the arguments, a JSON array
}

func newCall(call *C.zebkit_call, origin string, args json.RawMessage) *Call {
	c := &Call{Origin: origin, Args: args}
	atomic.StorePointer(&c.call, unsafe.Pointer(call))
	return c
}

// ArgsInto decodes the arguments into values, pointers in their order;
// arguments past them are ignored, and missing ones leave theirs as they are.
func (c *Call) ArgsInto(values ...any) error {
	var args []json.RawMessage
	if err := json.Unmarshal(c.Args, &args); err != nil {
		return err
	}
	for i, value := range values {
		if i >= len(args) {
			break
		}
		if err := json.Unmarshal(args[i], value); err != nil {
			return fmt.Errorf("argument %d: %w", i, err)
		}
	}
	return nil
}

// take is the engine's call, once: ErrClosed once it is answered.
func (c *Call) take() (*C.zebkit_call, error) {
	if c == nil {
		return nil, ErrClosed
	}
	call := atomic.SwapPointer(&c.call, nil)
	if call == nil {
		return nil, ErrClosed
	}
	return (*C.zebkit_call)(call), nil
}

// ResolveValue answers with value, encoded with encoding/json: a Go string
// is a JavaScript string.
func (c *Call) ResolveValue(value any) error {
	text, err := json.Marshal(value)
	if err != nil {
		return err
	}
	return c.ResolveJSON(text)
}

// ResolveJSON answers with JSON text as it was written, its numbers and all;
// empty is undefined.
func (c *Call) ResolveJSON(value []byte) error {
	call, err := c.take()
	if err != nil {
		return err
	}
	if len(value) == 0 {
		C.zebkit_call_resolve(call, nil)
		return nil
	}
	p := C.CBytes(value)
	defer C.free(p)
	C.zebkit_call_resolve_json(call, (*C.char)(p), C.size_t(len(value)))
	return nil
}

// Reject answers with an error of message.
func (c *Call) Reject(message string) error {
	call, err := c.take()
	if err != nil {
		return err
	}
	value := cString(message)
	defer free(value)
	C.zebkit_call_reject(call, value)
	return nil
}

// Bind makes zebkit.app.<name>(...) in granted pages call function; nil
// removes it.
func (v *View) Bind(name string, function func(call *Call)) {
	raw, ok := v.raw()
	if !ok {
		return
	}
	cname := cString(name)
	defer free(cname)
	s, ok := v.binds[name]
	if !ok {
		s = &slot{}
		v.binds[name] = s
	}
	if function == nil {
		C.zebkit_view_bind(raw, cname, nil, nil)
		s.clear()
		delete(v.binds, name)
		return
	}
	h := s.set(function)
	C.zebkit_view_bind(raw, cname, C.zebkit_bind_func(C.zk_bind), data(h))
}

// Window is the state of the view's window (<zebkit/window.h>).
func (v *View) Window() (map[string]any, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	var value map[string]any
	return value, takeJSON(C.zebkit_view_get_window(raw), &value, "no window")
}

// SetWindow changes the window's options, JSON.
func (v *View) SetWindow(options any) error {
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
	return failed(C.zebkit_view_set_window(raw, text, &message), message, "the window options were refused")
}

// WindowAction asks for "minimize", "maximize", "restore", "fullscreen", "close", ...
func (v *View) WindowAction(action string) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	caction := cString(action)
	defer free(caction)
	var message *C.char
	return failed(C.zebkit_view_window_action(raw, caction, &message), message, "the window action was refused")
}

// OnWindowState calls function with every change of the window's state.
func (v *View) OnWindowState(function func(state map[string]any)) {
	v.told("window-state", function, func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_window_state(raw, C.zebkit_window_state_func(f), d)
	})
}
