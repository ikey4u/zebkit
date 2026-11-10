package zebkit

/*
#include "bridge_a.h"
*/
import "C"

import (
	"time"
	"unsafe"
)

//export goZkAWakeup
func goZkAWakeup(delay C.int64_t, h C.uintptr_t) {
	if delay < 0 {
		delay = 0
	}
	if f, ok := callback(h).(func(time.Duration)); ok {
		f(time.Duration(delay) * time.Millisecond)
	}
}

//export goZkAWindowOpen
func goZkAWindowOpen(opened *C.zebkit_view, url *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(*C.zebkit_view, string)); ok {
		f(opened, goString(url))
	}
}

//export goZkAExport
func goZkAExport(text *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(string, bool)); ok {
		f(goString(text), text != nil)
	}
}

//export goZkABody
func goZkABody(id *C.char, bytes unsafe.Pointer, size C.size_t, end, truncated C.bool, h C.uintptr_t) {
	var body []byte
	if bytes != nil && size > 0 {
		body = C.GoBytes(bytes, C.int(size))
	}
	if f, ok := callback(h).(func(string, []byte, bool, bool)); ok {
		f(goString(id), body, bool(end), bool(truncated))
	}
}

//export goZkAItem
func goZkAItem(id *C.char, h C.uintptr_t) {
	if f, ok := callback(h).(func(string)); ok {
		f(goString(id))
	}
}
