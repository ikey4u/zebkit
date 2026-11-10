package zebkit

/*
#include "bridge_c.h"
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// The engine calls these through bridge_c.c with the handle of the Go
// function to run: automation, off-screen views, dialogs, drag and drop
// and media. A function let go (its view closed) may still be called by
// the engine on its way out: that call does nothing, but a frame is still
// given back.

// valueOf is the Go value of handle h, or false for a handle let go.
func valueOf(h C.uintptr_t) (value any, ok bool) {
	defer func() {
		if recover() != nil {
			value, ok = nil, false
		}
	}()
	return cgo.Handle(h).Value(), true
}

//export goZkCPixels
func goZkCPixels(pixels unsafe.Pointer, width, height, stride C.int, failure *C.char, h C.uintptr_t) {
	var copied []byte
	if pixels != nil && height > 0 && stride > 0 {
		copied = C.GoBytes(pixels, height*stride)
	}
	if f, ok := valueOf(h); ok {
		f.(func([]byte, int, int, int, string, bool))(copied, int(width), int(height), int(stride),
			goString(failure), failure != nil)
	}
}

//export goZkCEvent
func goZkCEvent(event, json *C.char, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(string, string))(goString(event), goString(json))
	}
}

//export goZkCListen
func goZkCListen(url, failure *C.char, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(string, string, bool))(goString(url), goString(failure), url == nil)
	}
}

//export goZkCFrame
func goZkCFrame(frame *C.zebkit_frame, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(*C.zebkit_frame))(frame)
		return
	}
	// Every frame goes back to the engine, also after its view has closed.
	C.zebkit_frame_release(frame)
}

//export goZkCRect
func goZkCRect(present C.bool, rect *C.zebkit_rect, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		if !present {
			rect = nil
		}
		f.(func(*C.zebkit_rect))(rect)
	}
}

//export goZkCCursor
func goZkCCursor(cursor *C.char, image *C.zebkit_cursor_image, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(string, *C.zebkit_cursor_image))(goString(cursor), image)
	}
}

//export goZkCDialog
func goZkCDialog(dialog *C.zebkit_dialog, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(*C.zebkit_dialog))(dialog)
	}
}

//export goZkCDialogClosed
func goZkCDialogClosed(id C.uint64_t, accepted C.bool, input *C.char, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(uint64, bool, string))(uint64(id), bool(accepted), goString(input))
	}
}

//export goZkCDrag
func goZkCDrag(drag *C.zebkit_drag, h C.uintptr_t) C.bool {
	if f, ok := valueOf(h); ok {
		return C.bool(f.(func(*C.zebkit_drag) bool)(drag))
	}
	return false
}

//export goZkCOperation
func goZkCOperation(operation C.uint, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(uint32))(uint32(operation))
	}
}

//export goZkCJSON
func goZkCJSON(json *C.char, h C.uintptr_t) {
	if f, ok := valueOf(h); ok {
		f.(func(string))(goString(json))
	}
}

//export goZkCTap
func goZkCTap(json *C.char, bytes unsafe.Pointer, size C.size_t, h C.uintptr_t) {
	var copied []byte
	if bytes != nil && size > 0 {
		copied = C.GoBytes(bytes, C.int(size))
	}
	if f, ok := valueOf(h); ok {
		f.(func(string, []byte))(goString(json), copied)
	}
}

//export goZkCProgress
func goZkCProgress(packets, timeUs C.int64_t, h C.uintptr_t) C.int {
	if f, ok := valueOf(h); ok && f.(func(int64, int64) bool)(int64(packets), int64(timeUs)) {
		return 1
	}
	return 0
}
