package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"unsafe"
)

// The engine calls these through bridge.c, on its thread, with the handle of
// the Go function to run.

//export goZkTask
func goZkTask(h C.uintptr_t) {
	if f, ok := callback(h).(func()); ok {
		f()
	}
}

//export goZkLoad
func goZkLoad(event C.int, url *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(LoadEvent, string)); ok {
		f(LoadEvent(event), C.GoString(url))
	}
}

//export goZkText
func goZkText(text *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(string)); ok {
		f(goString(text))
	}
}

//export goZkResult
func goZkResult(json, failure *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(string, string, bool)); ok {
		f(goString(json), goString(failure), failure != nil)
	}
}

//export goZkCapture
func goZkCapture(png unsafe.Pointer, size C.size_t, failure *C.char, h C.uintptr_t) {
	var bytes []byte
	if png != nil && size > 0 {
		bytes = C.GoBytes(png, C.int(size))
	}
	if f, ok := callback(h).(func([]byte, string, bool)); ok {
		f(bytes, goString(failure), failure != nil)
	}
}

//export goZkBind
func goZkBind(origin, args *C.char, call *C.zebkit_call, h C.uintptr_t) {
	if f, ok := callback(h).(func(*Call)); ok {
		f(&Call{call: call, Origin: goString(origin), Args: goString(args)})
	}
}

//export goZkScheme
func goZkScheme(request *C.zebkit_request, h C.uintptr_t) {
	if f, ok := callback(h).(func(*Request)); ok {
		f(&Request{request: request})
	}
}

//export goZkAsked
func goZkAsked(request unsafe.Pointer, json *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(unsafe.Pointer, string)); ok {
		f(request, goString(json))
	}
}

// callback is the Go function a handle names, or nil once it has been let go:
// the engine can still call a view's function as the view closes or the
// engine shuts down, after the view has let it go.
func callback(h C.uintptr_t) any {
	value, _ := valueOf(h)
	return value
}

func goString(text *C.char) string {
	if text == nil {
		return ""
	}
	return C.GoString(text)
}
