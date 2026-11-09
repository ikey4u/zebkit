// Package zebkit is Chromium's engine as a library, from Go: web views in
// windows or off-screen, hooks for what pages ask and what the engine would
// decide, the system's services, and the engine's own APIs. It is a binding
// over the C API (<zebkit/zebkit.h>, docs/API.md) through cgo.
//
// The engine is a separate download: unpack the release archive for your
// system, of the same version, and build with its headers and library:
//
//	export ZEBKIT_DIR=/path/to/zebkit-<version>-<target>
//	export CGO_CFLAGS="-I$ZEBKIT_DIR/include"
//	export CGO_LDFLAGS="-L$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"            # Linux
//	export CGO_LDFLAGS="-F$ZEBKIT_DIR -Wl,-rpath,$ZEBKIT_DIR"            # macOS
//
// One goroutine runs the engine: the program's main goroutine, which this
// package locks to the main thread. Call the engine from it, and from the
// functions the engine calls back; from other goroutines, hand work over with
// Post. Main runs the engine's loop; Init and Pump fit it into a loop of the
// program's own.
package zebkit

/*
#cgo linux LDFLAGS: -l:libzebkit.so.1
#cgo darwin LDFLAGS: -framework Zebkit
#include "bridge.h"

static void *zk_data(uintptr_t handle) { return (void *)handle; }
*/
import "C"

import (
	"encoding/json"
	"errors"
	"os"
	"runtime"
	"runtime/cgo"
	"unsafe"
)

func init() {
	// The engine's thread is the one that starts it: the main thread.
	runtime.LockOSThread()
}

// Error is what the engine said when it refused something.
type Error struct{ Message string }

func (e *Error) Error() string { return e.Message }

// takeError turns an engine-allocated message into an error, and frees it.
func takeError(message *C.char, otherwise string) error {
	if message == nil {
		return &Error{otherwise}
	}
	defer C.zebkit_free(unsafe.Pointer(message))
	return &Error{C.GoString(message)}
}

// takeString returns an engine-allocated string, and frees it.
func takeString(text *C.char) string {
	if text == nil {
		return ""
	}
	defer C.zebkit_free(unsafe.Pointer(text))
	return C.GoString(text)
}

// takeJSON decodes an engine-allocated JSON string, and frees it.
func takeJSON(text *C.char, into any) bool {
	value := takeString(text)
	return value != "" && json.Unmarshal([]byte(value), into) == nil
}

func cString(text string) *C.char { return C.CString(text) }

func free(text *C.char) { C.free(unsafe.Pointer(text)) }

// optional is NULL for nil, else the string.
func optional(text *string) *C.char {
	if text == nil {
		return nil
	}
	return C.CString(*text)
}

func handle(value any) C.uintptr_t { return C.uintptr_t(cgo.NewHandle(value)) }

func data(h C.uintptr_t) unsafe.Pointer { return C.zk_data(h) }

// Version is zebkit's version; EngineVersion is Chromium's.
func Version() string       { return C.GoString(C.zebkit_version()) }
func EngineVersion() string { return C.GoString(C.zebkit_engine_version()) }

func argv(args []string) (C.int, **C.char, func()) {
	all := append([]string{os.Args[0]}, args...)
	array := C.malloc(C.size_t(len(all)+1) * C.size_t(unsafe.Sizeof(uintptr(0))))
	items := unsafe.Slice((**C.char)(array), len(all)+1)
	for i, arg := range all {
		items[i] = C.CString(arg)
	}
	items[len(all)] = nil
	return C.int(len(all)), (**C.char)(array), func() {
		for i := range all {
			C.free(unsafe.Pointer(items[i]))
		}
		C.free(array)
	}
}

// Init starts the engine inside the program's own loop; args are the
// engine's switches, such as "--headless". Pump it from then on, and Shutdown
// at the end.
func Init(args ...string) error {
	argc, argv, done := argv(args)
	defer done()
	if code := C.zebkit_init(argc, argv); code != 0 {
		return &Error{"the engine did not start"}
	}
	return nil
}

// Pump runs what is due on the engine's thread and returns; false once the
// engine has quit.
func Pump() bool { return bool(C.zebkit_pump()) }

// Shutdown ends an engine started with Init.
func Shutdown() bool { return bool(C.zebkit_shutdown()) }

// Main runs the engine's loop until Quit, calling ready once it can make
// views; args are the engine's switches. It returns the exit code.
func Main(ready func(), args ...string) int {
	argc, argv, done := argv(args)
	defer done()
	h := handle(ready)
	defer cgo.Handle(h).Delete()
	return int(C.zebkit_main(argc, argv, C.zebkit_task_func(C.zk_task), data(h)))
}

// Quit ends Main's loop.
func Quit() { C.zebkit_quit() }

// IsHeadless says whether the engine runs without a display.
func IsHeadless() bool { return bool(C.zebkit_is_headless()) }

// Post runs function on the engine's thread, from any goroutine.
func Post(function func()) {
	var h C.uintptr_t
	h = handle(func() {
		cgo.Handle(h).Delete()
		function()
	})
	C.zebkit_post_task(C.zebkit_task_func(C.zk_task), data(h))
}

// PostDelayed runs function on the engine's thread after delayMs.
func PostDelayed(function func(), delayMs uint32) {
	var h C.uintptr_t
	h = handle(func() {
		cgo.Handle(h).Delete()
		function()
	})
	C.zebkit_post_delayed_task(C.zebkit_task_func(C.zk_task), data(h), C.uint32_t(delayMs))
}

// Environment is what the system says of how things should look:
// colorScheme, contrast, forcedColors, locale, accessibility, following.
func Environment() map[string]any {
	var value map[string]any
	takeJSON(C.zebkit_get_environment(), &value)
	return value
}

// Capabilities is what this build can do on this system and display.
func Capabilities() map[string]any {
	var value map[string]any
	takeJSON(C.zebkit_get_capabilities(), &value)
	return value
}

// A slot keeps the one Go function the engine holds for something, and lets
// it go when replaced.
type slot struct{ h C.uintptr_t }

func (s *slot) set(value any) C.uintptr_t {
	s.clear()
	if value == nil {
		return 0
	}
	s.h = handle(value)
	return s.h
}

func (s *slot) clear() {
	if s.h != 0 {
		cgo.Handle(s.h).Delete()
		s.h = 0
	}
}

// jsonFunc wraps function(event) for an engine that calls with JSON.
func jsonFunc(function func(map[string]any)) func(string) {
	return func(text string) {
		var value map[string]any
		_ = json.Unmarshal([]byte(text), &value)
		function(value)
	}
}

var environmentSlot slot

// OnEnvironmentChanged calls function with the environment when it changes;
// nil stops.
func OnEnvironmentChanged(function func(map[string]any)) {
	if function == nil {
		C.zebkit_on_environment_changed(nil, nil)
		environmentSlot.clear()
		return
	}
	h := environmentSlot.set(jsonFunc(function))
	C.zebkit_on_environment_changed(C.zebkit_environment_func(C.zk_json), data(h))
}

var errNil = errors.New("zebkit: nil")
