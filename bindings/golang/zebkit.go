// Package zebkit is Chromium's engine as a library, from Go: web views in
// windows or off-screen, hooks for what pages ask and what the engine would
// decide, the system's services, and the engine's own APIs. It is a binding
// over the C API (<zebkit/zebkit.h>, docs/API.md) through cgo.
//
// The engine is a separate download: unpack the release archive for your
// system, of the same version (BindingVersion), and build with its headers
// and library:
//
//	export ZEBKIT_DIR=/path/to/zebkit-<version>-<target>
//	export CGO_CFLAGS="-I$ZEBKIT_DIR/include"
//	export CGO_LDFLAGS="-L$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"            # Linux
//	export CGO_LDFLAGS="-F$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"            # macOS
//
// Windows is cross-built with cmd/zebkit-cc (README.md).
//
// # The engine's thread
//
// One goroutine runs the engine: the one that calls Init or Main, which this
// package locks to its thread (the main thread: the package locks the main
// goroutine at init, as macOS requires). Call views and the engine from it,
// and from the functions the engine calls back. One rule holds for every
// function and method that reaches the engine: called from another thread it
// panics (before Init or Main, the goroutine that will start the engine is
// the one to call the functions that configure it). The exceptions are the
// ones the C API lets any thread call: Post and PostDelayed, a Call's
// Resolve, Reject and Close, a Request's Respond, and an off-screen Frame's
// Release. From other goroutines, hand work over with Post. The package's
// own state for what the engine holds (the functions of OnPowerEvent,
// SetAppMenu, HandleScheme and the like) is the engine thread's too, and is
// kept by that rule. Main runs the engine's loop; Init and Pump fit it into
// a loop of the program's own. The PDF, canvas, text and services APIs
// (except Fetch) need no engine thread.
//
// # Conventions
//
// JSON going in: an argument documented as JSON is a string, a []byte or a
// json.RawMessage, taken as JSON text as it is, or any other value, encoded
// with encoding/json. Where the C API has a default for NULL, nil (and "")
// gives it; elsewhere nil is JSON's null.
//
// JSON coming out: small answers of a fixed shape are decoded (a struct, a
// map[string]any, an int or a bool); documents whose shape is open, as the
// C headers describe them, are json.RawMessage.
//
// Errors: what the engine can refuse returns an error, an *Error with the
// engine's message (errors.Is(err, ErrRefused) holds for all of them);
// constructors return (value, error); a bool is only a predicate.
//
// Callbacks: a nil function removes the one set before. One-shot callbacks
// (done) may be nil.
//
// Closing: Close lets a thing go; a view also closes when the user closes
// its window, its page calls window.close(), its context closes, or the
// engine quits. A closed view's methods do nothing; those that return an
// error return ErrClosed, and a done function is called with ErrClosed.
//
// # Playwright's API
//
// The automation package (github.com/ikey4u/zebkit/bindings/golang/automation)
// is Playwright's API, typed: pages, locators, events, routes, handles,
// contexts and expect. This package has its low level: CallAutomation,
// CallAutomationPixels, View.OnAutomationEvent, ViewByPageID and
// ListenAutomation.
package zebkit

/*
#cgo linux LDFLAGS: -l:libzebkit.so.1
#cgo darwin LDFLAGS: -framework Zebkit
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"reflect"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"unsafe"
)

// BindingVersion is the version of zebkit this binding is made for. Init and
// Main compare it with the engine's (Version): a different major or minor
// version fails, a different patch level only warns.
const BindingVersion = "0.5.0"

func init() {
	// The engine's thread is the one that starts it: the main thread, which
	// macOS requires. Init locks the goroutine that calls it as well.
	runtime.LockOSThread()
}

// ErrRefused is what every error of the engine is: errors.Is(err, ErrRefused).
var ErrRefused = errors.New("zebkit: refused")

// ErrClosed is what a closed view, or another thing closed, answers.
var ErrClosed = errors.New("zebkit: closed")

// ErrNotStarted is Post's answer before the engine has started or after it
// has stopped.
var ErrNotStarted = errors.New("zebkit: the engine is not running")

// Error is what the engine said when it refused something.
type Error struct{ Message string }

func (e *Error) Error() string { return e.Message }

// Unwrap makes every engine error ErrRefused.
func (e *Error) Unwrap() error { return ErrRefused }

// refused is the error of a C call that said no without a message.
func refused(what string) error { return &Error{what} }

// check turns a C call's false into refused(what).
func check(ok C.bool, what string) error {
	if !ok {
		return refused(what)
	}
	return nil
}

// takeError turns an engine-allocated message into an error, and frees it.
func takeError(message *C.char, otherwise string) error {
	if message == nil {
		return &Error{otherwise}
	}
	defer C.zebkit_free(unsafe.Pointer(message))
	return &Error{C.GoString(message)}
}

// failed is takeError when ok is false, and frees a message that came anyway.
func failed(ok C.bool, message *C.char, otherwise string) error {
	if !ok {
		return takeError(message, otherwise)
	}
	if message != nil {
		C.zebkit_free(unsafe.Pointer(message))
	}
	return nil
}

// takeString returns an engine-allocated string, and frees it.
func takeString(text *C.char) string {
	if text == nil {
		return ""
	}
	defer C.zebkit_free(unsafe.Pointer(text))
	return C.GoString(text)
}

// takeRaw returns an engine-allocated JSON string, and frees it; NULL is
// an error.
func takeRaw(text *C.char, otherwise string) (json.RawMessage, error) {
	if text == nil {
		return nil, refused(otherwise)
	}
	return json.RawMessage(takeString(text)), nil
}

// takeJSON decodes an engine-allocated JSON string into into, and frees it.
func takeJSON(text *C.char, into any, otherwise string) error {
	if text == nil {
		return refused(otherwise)
	}
	value := takeString(text)
	if err := json.Unmarshal([]byte(value), into); err != nil {
		return fmt.Errorf("zebkit: %s: %w", otherwise, err)
	}
	return nil
}

func goString(text *C.char) string {
	if text == nil {
		return ""
	}
	return C.GoString(text)
}

// goBytes copies size bytes at p; size may be over 2 GiB.
func goBytes(p unsafe.Pointer, size C.size_t) []byte {
	if p == nil || size == 0 {
		return nil
	}
	return append([]byte(nil), unsafe.Slice((*byte)(p), int(size))...)
}

// takeBytes copies an engine-allocated buffer, and frees it.
func takeBytes(p *C.uint8_t, size C.size_t) []byte {
	if p == nil {
		return nil
	}
	defer C.zebkit_free(unsafe.Pointer(p))
	if size == 0 {
		return []byte{}
	}
	return goBytes(unsafe.Pointer(p), size)
}

// rgbaSize is width * height * 4 without overflowing a C int.
func rgbaSize(width, height C.int) C.size_t {
	if width <= 0 || height <= 0 {
		return 0
	}
	return C.size_t(width) * C.size_t(height) * 4
}

func cString(text string) *C.char { return C.CString(text) }

func free(text *C.char) { C.free(unsafe.Pointer(text)) }

// optionalString is NULL for "", else the string.
func optionalString(text string) *C.char {
	if text == "" {
		return nil
	}
	return cString(text)
}

// jsonArg is value as JSON text for the engine (the package's rule): a
// string, []byte or json.RawMessage as it is, anything else encoded; nil is
// null. Free it.
func jsonArg(value any) (*C.char, error) {
	switch v := value.(type) {
	case nil:
		return cString("null"), nil
	case string:
		return cString(v), nil
	case []byte:
		return cString(string(v)), nil
	case json.RawMessage:
		return cString(string(v)), nil
	}
	text, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}
	return cString(string(text)), nil
}

// optionalJSON is jsonArg, but NULL (the C API's default) for nil and "".
func optionalJSON(value any) (*C.char, error) {
	switch v := value.(type) {
	case nil:
		return nil, nil
	case string:
		if v == "" {
			return nil, nil
		}
	case []byte:
		if len(v) == 0 {
			return nil, nil
		}
	case json.RawMessage:
		if len(v) == 0 {
			return nil, nil
		}
	}
	return jsonArg(value)
}

// cStrings is a NULL-terminated C array of args; done frees it.
func cStrings(args []string) (C.int, **C.char, func()) {
	array := C.malloc(C.size_t(len(args)+1) * C.size_t(unsafe.Sizeof(uintptr(0))))
	items := unsafe.Slice((**C.char)(array), len(args)+1)
	for i, arg := range args {
		items[i] = C.CString(arg)
	}
	items[len(args)] = nil
	return C.int(len(args)), (**C.char)(array), func() {
		for i := range args {
			C.free(unsafe.Pointer(items[i]))
		}
		C.free(array)
	}
}

// bytesArg points at b's bytes for the length of a call, or is nil.
func bytesArg(b []byte) unsafe.Pointer {
	if len(b) == 0 {
		return nil
	}
	return unsafe.Pointer(unsafe.SliceData(b))
}

// ---- Handles ---------------------------------------------------------------

// The Go values the engine holds are named by handles: numbers it keeps as
// its callbacks' data and gives back. A handle let go is never used again,
// so a late call with one finds nothing and does nothing.

type entry struct {
	value any
	once  bool
}

var handles sync.Map // uintptr -> entry
var lastHandle atomic.Uintptr

// isNil says whether value is nil, or a nil function.
func isNil(value any) bool {
	if value == nil {
		return true
	}
	v := reflect.ValueOf(value)
	return v.Kind() == reflect.Func && v.IsNil()
}

func newHandle(value any, once bool) C.uintptr_t {
	if isNil(value) {
		return 0
	}
	h := lastHandle.Add(1)
	handles.Store(h, entry{value, once})
	return C.uintptr_t(h)
}

// handle names value for as long as the engine holds it: release it after.
func handle(value any) C.uintptr_t { return newHandle(value, false) }

// onceHandle names value for one call, after which it is let go.
func onceHandle(value any) C.uintptr_t { return newHandle(value, true) }

// release lets a handle go.
func release(h C.uintptr_t) {
	if h != 0 {
		handles.Delete(uintptr(h))
	}
}

// lookup is the function a handle names, if it still does and is a T; a
// one-shot handle is let go as it is found.
func lookup[T any](h C.uintptr_t) (T, bool) {
	var zero T
	if h == 0 {
		return zero, false
	}
	found, ok := handles.Load(uintptr(h))
	if !ok {
		return zero, false
	}
	e := found.(entry)
	if e.once {
		if _, loaded := handles.LoadAndDelete(uintptr(h)); !loaded {
			return zero, false
		}
	}
	value, ok := e.value.(T)
	if !ok || isNil(value) {
		return zero, false
	}
	return value, true
}

func data(h C.uintptr_t) unsafe.Pointer { return C.zk_data(h) }

// A slot keeps the one Go function the engine holds for something, and lets
// it go when replaced.
type slot struct{ h C.uintptr_t }

// set names value (0 for a nil function) and lets the one before go.
func (s *slot) set(value any) C.uintptr_t {
	s.clear()
	s.h = handle(value)
	return s.h
}

// replace keeps h, a handle the engine has accepted, and lets the one before go.
func (s *slot) replace(h C.uintptr_t) {
	s.clear()
	s.h = h
}

func (s *slot) clear() {
	release(s.h)
	s.h = 0
}

// ---- One-shot answers --------------------------------------------------------

// resultOnce is the handle of a done function given JSON or an error, once.
func resultOnce(done func(json.RawMessage, error)) C.uintptr_t {
	return onceHandle(func(result, failure string, isError bool) {
		if done == nil {
			return
		}
		if isError {
			done(nil, &Error{failure})
		} else {
			done(json.RawMessage(result), nil)
		}
	})
}

// bytesOnce is the handle of a done function given bytes or an error, once.
func bytesOnce(done func([]byte, error)) C.uintptr_t {
	return onceHandle(func(bytes []byte, failure string, isError bool) {
		if done == nil {
			return
		}
		if isError {
			done(nil, &Error{failure})
		} else {
			done(bytes, nil)
		}
	})
}

// errorOnce is the handle of a done function given only an error, once.
func errorOnce(done func(error)) C.uintptr_t {
	return resultOnce(func(_ json.RawMessage, err error) {
		if done != nil {
			done(err)
		}
	})
}

// decodeOnce adapts done to a JSON answer decoded into a T.
func decodeOnce[T any](done func(T, error)) func(json.RawMessage, error) {
	return func(result json.RawMessage, err error) {
		var value T
		if err == nil {
			err = json.Unmarshal(result, &value)
		}
		if done != nil {
			done(value, err)
		}
	}
}

// ---- The engine --------------------------------------------------------------

var started atomic.Bool

// Version is zebkit's version; EngineVersion is Chromium's.
func Version() string       { return C.GoString(C.zebkit_version()) }
func EngineVersion() string { return C.GoString(C.zebkit_engine_version()) }

func versionParts(version string) (major, minor, patch int, ok bool) {
	parts := strings.SplitN(strings.SplitN(version, "-", 2)[0], ".", 3)
	if len(parts) != 3 {
		return 0, 0, 0, false
	}
	var err [3]error
	major, err[0] = strconv.Atoi(parts[0])
	minor, err[1] = strconv.Atoi(parts[1])
	patch, err[2] = strconv.Atoi(parts[2])
	return major, minor, patch, err[0] == nil && err[1] == nil && err[2] == nil
}

// checkVersion fails for an engine of another major or minor version than
// BindingVersion, and warns for another patch level.
func checkVersion() error {
	engine := Version()
	a, b, c, ok1 := versionParts(BindingVersion)
	x, y, z, ok2 := versionParts(engine)
	if !ok1 || !ok2 {
		return nil
	}
	if a != x || b != y {
		return &Error{fmt.Sprintf("zebkit: the binding is for zebkit %s, the engine is %s", BindingVersion, engine)}
	}
	if c != z {
		fmt.Fprintf(os.Stderr, "zebkit: warning: the binding is for zebkit %s, the engine is %s\n", BindingVersion, engine)
	}
	return nil
}

// becomeEngineThread locks the calling goroutine to its thread and makes it
// the engine's.
func becomeEngineThread() {
	runtime.LockOSThread()
	C.zk_mark_engine_thread()
}

// onEngineThread panics when called off the engine's thread: the engine's
// calls there are undefined behaviour.
func onEngineThread() {
	if !C.zk_on_engine_thread() {
		panic("zebkit: called off the engine's thread; hand the work over with zebkit.Post")
	}
}

// IsEngineThread says whether the calling goroutine runs on the engine's
// thread (before Init or Main, any goroutine does); any goroutine may ask.
func IsEngineThread() bool { return bool(C.zk_on_engine_thread()) }

// Init starts the engine inside the program's own loop; args are the
// engine's switches, such as "--headless". Pump it from then on, and Shutdown
// at the end. The calling goroutine becomes the engine's.
func Init(args ...string) error {
	if err := checkVersion(); err != nil {
		return err
	}
	becomeEngineThread()
	argc, argv, done := cStrings(append([]string{os.Args[0]}, args...))
	defer done()
	if code := C.zebkit_init(argc, argv); code != 0 {
		return &Error{fmt.Sprintf("the engine did not start (%d)", int(code))}
	}
	started.Store(true)
	return nil
}

// Pump runs what is due on the engine's thread and returns; false once the
// engine has quit, before Init, and inside a callback (a nested pump).
func Pump() bool {
	onEngineThread()
	return bool(C.zebkit_pump())
}

// Shutdown ends an engine started with Init: its views close first. It is
// refused, and the engine keeps running, when it is not running here or when
// called from inside a callback or a nested pump.
func Shutdown() error {
	onEngineThread()
	if !C.zebkit_shutdown() {
		return refused("the engine was not running here, or Shutdown was called from inside a callback")
	}
	started.Store(false)
	return nil
}

// Main runs the engine's loop until Quit, calling ready once it can make
// views; args are the engine's switches. It returns the exit code (1 when
// the engine is of another version than BindingVersion).
func Main(ready func(), args ...string) int {
	if err := checkVersion(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	becomeEngineThread()
	argc, argv, done := cStrings(append([]string{os.Args[0]}, args...))
	defer done()
	h := handle(func() {
		started.Store(true)
		if ready != nil {
			ready()
		}
	})
	defer release(h)
	code := int(C.zebkit_main(argc, argv, C.zebkit_task_func(C.zk_task), data(h)))
	started.Store(false)
	return code
}

// Quit closes every view and ends Main's loop.
func Quit() {
	onEngineThread()
	C.zebkit_quit()
}

// IsHeadless says whether the engine runs without a display.
func IsHeadless() bool {
	onEngineThread()
	return bool(C.zebkit_is_headless())
}

// Post runs function on the engine's thread, from any goroutine; ErrNotStarted
// before the engine has started (Init, or Main's ready) or after it stopped,
// when it would never run.
func Post(function func()) error { return PostDelayed(function, 0) }

// PostDelayed runs function on the engine's thread after delayMs.
func PostDelayed(function func(), delayMs uint32) error {
	if function == nil {
		return nil
	}
	if !started.Load() {
		return ErrNotStarted
	}
	h := onceHandle(function)
	var posted C.bool
	if delayMs == 0 {
		posted = C.zebkit_post_task(C.zebkit_task_func(C.zk_task), data(h))
	} else {
		posted = C.zebkit_post_delayed_task(C.zebkit_task_func(C.zk_task), data(h), C.uint32_t(delayMs))
	}
	if !posted {
		// The engine has stopped (or is stopping): function never runs.
		release(h)
		return ErrNotStarted
	}
	return nil
}

// Environment is what the system says of how things should look:
// colorScheme, contrast, forcedColors, locale, accessibility, following.
func Environment() (map[string]any, error) {
	onEngineThread()
	var value map[string]any
	return value, takeJSON(C.zebkit_get_environment(), &value, "no environment")
}

// Capabilities is what this build can do on this system and display.
func Capabilities() (map[string]any, error) {
	onEngineThread()
	var value map[string]any
	return value, takeJSON(C.zebkit_get_capabilities(), &value, "no capabilities")
}

// jsonFunc wraps function(event) for an engine that calls with JSON.
func jsonFunc[T any](function func(T)) func(string) {
	return func(text string) {
		var value T
		_ = json.Unmarshal([]byte(text), &value)
		function(value)
	}
}

var environmentSlot slot

// OnEnvironmentChanged calls function with the environment when it changes;
// nil stops.
func OnEnvironmentChanged(function func(map[string]any)) {
	onEngineThread()
	if function == nil {
		C.zebkit_on_environment_changed(nil, nil)
		environmentSlot.clear()
		return
	}
	h := environmentSlot.set(jsonFunc(function))
	C.zebkit_on_environment_changed(C.zebkit_environment_func(C.zk_text), data(h))
}
