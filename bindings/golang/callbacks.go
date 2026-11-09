package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// The engine calls these through bridge.c, on its thread, with the handle of
// the Go function to run.

//export goZkTask
func goZkTask(h C.uintptr_t) {
	cgo.Handle(h).Value().(func())()
}

//export goZkLoad
func goZkLoad(event C.int, url *C.char, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(LoadEvent, string))(LoadEvent(event), C.GoString(url))
}

//export goZkText
func goZkText(text *C.char, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(string))(goString(text))
}

//export goZkResult
func goZkResult(json, failure *C.char, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(string, string, bool))(goString(json), goString(failure), failure != nil)
}

//export goZkCapture
func goZkCapture(png unsafe.Pointer, size C.size_t, failure *C.char, h C.uintptr_t) {
	var bytes []byte
	if png != nil && size > 0 {
		bytes = C.GoBytes(png, C.int(size))
	}
	cgo.Handle(h).Value().(func([]byte, string, bool))(bytes, goString(failure), failure != nil)
}

//export goZkBind
func goZkBind(origin, args *C.char, call *C.zebkit_call, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(*Call))(&Call{call: call, Origin: goString(origin), Args: goString(args)})
}

//export goZkScheme
func goZkScheme(request *C.zebkit_request, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(*Request))(&Request{request: request})
}

//export goZkAsked
func goZkAsked(request unsafe.Pointer, json *C.char, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(unsafe.Pointer, string))(request, goString(json))
}

func goString(text *C.char) string {
	if text == nil {
		return ""
	}
	return C.GoString(text)
}
