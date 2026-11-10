package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"runtime/cgo"
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

// View is a web page, in a window or off-screen.
type View struct {
	view  *C.zebkit_view
	slots map[string]*slot
	binds []slot
}

func newView(view *C.zebkit_view) *View {
	return &View{view: view, slots: map[string]*slot{}}
}

// NewView makes a view loading url, width by height; "" loads nothing.
func NewView(url string, width, height int) *View {
	var curl *C.char
	if url != "" {
		curl = cString(url)
		defer free(curl)
	}
	return newView(C.zebkit_view_new(curl, C.int(width), C.int(height)))
}

// NewViewWithOptions makes a view whose window is made with options
// (<zebkit/window.h>): {"width", "height", "frame": "overlay", ...}.
func NewViewWithOptions(url string, options any) (*View, error) {
	text, err := json.Marshal(options)
	if err != nil {
		return nil, err
	}
	curl, coptions := cString(url), cString(string(text))
	defer free(curl)
	defer free(coptions)
	var message *C.char
	view := C.zebkit_view_new_with_options(nil, curl, coptions, &message)
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

// Load loads url.
func (v *View) Load(url string) {
	curl := cString(url)
	defer free(curl)
	C.zebkit_view_load_url(v.view, curl)
}

func (v *View) Reload()                  { C.zebkit_view_reload(v.view) }
func (v *View) Back()                    { C.zebkit_view_go_back(v.view) }
func (v *View) Forward()                 { C.zebkit_view_go_forward(v.view) }
func (v *View) Stop()                    { C.zebkit_view_stop(v.view) }
func (v *View) Focus()                   { C.zebkit_view_focus(v.view) }
func (v *View) IsLoading() bool          { return bool(C.zebkit_view_is_loading(v.view)) }
func (v *View) Resize(width, height int) { C.zebkit_view_resize(v.view, C.int(width), C.int(height)) }
func (v *View) URL() string              { return takeString(C.zebkit_view_get_url(v.view)) }
func (v *View) Title() string            { return takeString(C.zebkit_view_get_title(v.view)) }
func (v *View) SetTitle(title string) {
	t := cString(title)
	defer free(t)
	C.zebkit_view_set_title(v.view, t)
}
func (v *View) AddScript(source string) {
	s := cString(source)
	defer free(s)
	C.zebkit_view_add_script(v.view, s)
}
func (v *View) SetUserAgent(agent string) {
	a := cString(agent)
	defer free(a)
	C.zebkit_view_set_user_agent(v.view, a)
}

// Close closes the view; its functions are let go.
func (v *View) Close() {
	if v.view == nil {
		return
	}
	C.zebkit_view_close(v.view)
	v.view = nil
	// The engine can still call the view's functions as it closes (OnClose
	// among them): they are let go after that, from the engine's loop. A
	// call that comes later still finds nothing and does nothing (callback).
	slots, binds := v.slots, v.binds
	v.slots, v.binds = map[string]*slot{}, nil
	Post(func() {
		for _, s := range slots {
			s.clear()
		}
		for i := range binds {
			binds[i].clear()
		}
	})
}

// Evaluate runs script in the page; done gets its result as JSON, or the
// error the page threw.
func (v *View) Evaluate(script string, done func(result string, err error)) {
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
	cscript := cString(script)
	defer free(cscript)
	C.zebkit_view_evaluate(v.view, cscript, C.zebkit_result_func(C.zk_result), data(h))
}

// Capture takes a PNG of what the view shows.
func (v *View) Capture(done func(png []byte, err error)) {
	var h C.uintptr_t
	h = handle(func(png []byte, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if failed {
			done(nil, &Error{failure})
		} else {
			done(png, nil)
		}
	})
	C.zebkit_view_capture(v.view, C.zebkit_capture_func(C.zk_capture), data(h))
}

// OnLoad calls function at each step of a page's load; nil stops.
func (v *View) OnLoad(function func(event LoadEvent, url string)) {
	if function == nil {
		C.zebkit_view_on_load(v.view, nil, nil)
		v.slot("load").clear()
		return
	}
	h := v.slot("load").set(function)
	C.zebkit_view_on_load(v.view, C.zebkit_load_func(C.zk_load), data(h))
}

// OnTitle calls function with the page's title when it changes.
func (v *View) OnTitle(function func(title string)) {
	if function == nil {
		C.zebkit_view_on_title(v.view, nil, nil)
		v.slot("title").clear()
		return
	}
	h := v.slot("title").set(function)
	C.zebkit_view_on_title(v.view, C.zebkit_title_func(C.zk_title), data(h))
}

// OnClose calls function when the view's window is closed.
func (v *View) OnClose(function func()) {
	if function == nil {
		C.zebkit_view_on_close(v.view, nil, nil)
		v.slot("close").clear()
		return
	}
	h := v.slot("close").set(function)
	C.zebkit_view_on_close(v.view, C.zebkit_view_func(C.zk_view), data(h))
}

// Grant gives pages of origin what allow says: the zebkit object and its
// APIs. filesRoot is the directory zebkit.fs works in, or "".
func (v *View) Grant(origin string, allow Allow, filesRoot string) {
	corigin := cString(origin)
	defer free(corigin)
	var root *C.char
	if filesRoot != "" {
		root = cString(filesRoot)
		defer free(root)
	}
	C.zebkit_view_grant(v.view, corigin, C.uint(allow), root)
}

// Call is a page's call of a function the application bound; answer it once.
type Call struct {
	call   *C.zebkit_call
	Origin string
	Args   string // the arguments, a JSON array
}

// Resolve answers with a JSON value.
func (c *Call) Resolve(jsonValue string) {
	value := cString(jsonValue)
	defer free(value)
	C.zebkit_call_resolve(c.call, value)
}

// Reject answers with an error.
func (c *Call) Reject(message string) {
	value := cString(message)
	defer free(value)
	C.zebkit_call_reject(c.call, value)
}

// Bind makes zebkit.app.<name>(...) in granted pages call function.
func (v *View) Bind(name string, function func(call *Call)) {
	v.binds = append(v.binds, slot{})
	h := v.binds[len(v.binds)-1].set(function)
	cname := cString(name)
	defer free(cname)
	C.zebkit_view_bind(v.view, cname, C.zebkit_bind_func(C.zk_bind), data(h))
}

// Window is the state of the view's window (<zebkit/window.h>).
func (v *View) Window() map[string]any {
	var value map[string]any
	takeJSON(C.zebkit_view_get_window(v.view), &value)
	return value
}

// SetWindow changes the window's options.
func (v *View) SetWindow(options any) error {
	text, err := json.Marshal(options)
	if err != nil {
		return err
	}
	coptions := cString(string(text))
	defer free(coptions)
	var message *C.char
	if !C.zebkit_view_set_window(v.view, coptions, &message) {
		return takeError(message, "the window options were refused")
	}
	return nil
}

// WindowAction asks for "minimize", "maximize", "restore", "fullscreen", "close", ...
func (v *View) WindowAction(action string) error {
	caction := cString(action)
	defer free(caction)
	var message *C.char
	if !C.zebkit_view_window_action(v.view, caction, &message) {
		return takeError(message, "the window action was refused")
	}
	return nil
}

// OnWindowState calls function with every change of the window's state.
func (v *View) OnWindowState(function func(state map[string]any)) {
	v.told("window-state", function, func(f, d unsafe.Pointer) {
		C.zebkit_view_on_window_state(v.view, C.zebkit_window_state_func(f), d)
	})
}

// told installs function as a hook that tells the application something as
// JSON; nil removes it. set takes the C function and its data.
func (v *View) told(name string, function func(map[string]any), set func(f, d unsafe.Pointer)) {
	if function == nil {
		set(nil, nil)
		v.slot(name).clear()
		return
	}
	h := v.slot(name).set(jsonFunc(function))
	set(unsafe.Pointer(C.zk_told), data(h))
}

// asked installs function as a hook the application answers; nil removes it.
func (v *View) asked(name string, function func(request unsafe.Pointer, json string), set func(f, d unsafe.Pointer)) {
	if function == nil {
		set(nil, nil)
		v.slot(name).clear()
		return
	}
	h := v.slot(name).set(function)
	set(unsafe.Pointer(C.zk_asked), data(h))
}
