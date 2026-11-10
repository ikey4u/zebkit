package zebkit

/*
#include "bridge.h"
#include "bridge_c.h"
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// What a page asks of its window (<zebkit/page.h>): JavaScript dialogs,
// closing as a tab closes, printing to PDF, and drag and drop.

// DialogType is the kind of a page's dialog.
type DialogType int

const (
	DialogAlert        DialogType = 0
	DialogConfirm      DialogType = 1
	DialogPrompt       DialogType = 2
	DialogBeforeUnload DialogType = 3 // "Leave this page?"; accepting leaves
	DialogPrint        DialogType = 4 // window.print(); answering only closes it
)

// Name is the type as the engine's JSON names it.
func (t DialogType) Name() string {
	switch t {
	case DialogAlert:
		return "alert"
	case DialogConfirm:
		return "confirm"
	case DialogPrompt:
		return "prompt"
	case DialogBeforeUnload:
		return "beforeunload"
	case DialogPrint:
		return "print"
	}
	return "unknown"
}

// DialogPolicy is what becomes of a view's dialogs.
type DialogPolicy int

const (
	// To OnDialog's function when there is one, held until answered;
	// else a CDP client's; else the engine's own.
	DialogPolicyDefault DialogPolicy = 0
	DialogPolicyHold    DialogPolicy = 1 // held until answered
	DialogPolicyAccept  DialogPolicy = 2 // answered OK at once
	DialogPolicyDismiss DialogPolicy = 3 // answered Cancel at once
)

// Dialog is a dialog a page opened.
type Dialog struct {
	ID            uint64
	Type          DialogType
	Message       string
	DefaultPrompt string
	URL           string // of the frame that opened it
	IsReload      bool   // a beforeunload of a reload
	Held          bool   // waiting for an answer
}

// OnDialog calls function for every dialog the view's pages open, held or
// not; nil stops. With a function set, the default policy holds them for it.
func (v *View) OnDialog(function func(*Dialog)) {
	if function == nil {
		C.zebkit_view_on_dialog(v.view, nil, nil)
		v.slot("dialog").clear()
		return
	}
	h := v.slot("dialog").set(func(d *C.zebkit_dialog) {
		function(&Dialog{
			ID: uint64(d.id), Type: DialogType(d._type), Message: goString(d.message),
			DefaultPrompt: goString(d.default_prompt), URL: goString(d.url),
			IsReload: bool(d.is_reload), Held: bool(d.held),
		})
	})
	C.zebkit_view_on_dialog(v.view, C.zebkit_dialog_func(C.zkc_dialog), data(h))
}

// OnDialogClosed calls function when a dialog closes, however it was
// answered; userInput is a prompt's text.
func (v *View) OnDialogClosed(function func(id uint64, accepted bool, userInput string)) {
	if function == nil {
		C.zebkit_view_on_dialog_closed(v.view, nil, nil)
		v.slot("dialog-closed").clear()
		return
	}
	h := v.slot("dialog-closed").set(function)
	C.zebkit_view_on_dialog_closed(v.view, C.zebkit_dialog_closed_func(C.zkc_dialog_closed), data(h))
}

func (v *View) SetDialogPolicy(policy DialogPolicy) {
	C.zebkit_view_set_dialog_policy(v.view, C.zebkit_dialog_policy(policy))
}

func (v *View) DialogPolicy() DialogPolicy {
	return DialogPolicy(C.zebkit_view_get_dialog_policy(v.view))
}

// RespondDialog answers dialog id: OK (accept) or Cancel; a prompt with
// promptText ("" for its default). False when no such dialog is open.
func (v *View) RespondDialog(id uint64, accept bool, promptText string) bool {
	text := optionalString(promptText)
	defer free(text)
	return bool(C.zebkit_view_dialog_respond(v.view, C.uint64_t(id), C.bool(accept), text))
}

// DialogsJSON is the view's open dialogs as a JSON array, oldest first.
func (v *View) DialogsJSON() string { return takeString(C.zebkit_view_dialogs(v.view)) }

// RequestClose closes the view as a browser closes a tab: the page's
// beforeunload handlers first.
func (v *View) RequestClose() { C.zebkit_view_request_close(v.view) }

// PrintToPDF prints the page to PDF with CDP's Page.printToPDF parameters as
// JSON ("" for the defaults).
func (v *View) PrintToPDF(options string, done func(pdf []byte, err error)) {
	var h C.uintptr_t
	h = handle(func(pdf []byte, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if failed {
			done(nil, &Error{failure})
		} else {
			done(pdf, nil)
		}
	})
	coptions := optionalString(options)
	defer free(coptions)
	C.zebkit_view_print_to_pdf(v.view, coptions, C.zebkit_pdf_func(C.zk_capture), data(h))
}

// Drag operations, as masks of what is allowed and as what was done.
const (
	DragNone uint32 = 0
	DragCopy uint32 = 1
	DragLink uint32 = 2
	DragMove uint32 = 16
	DragAll  uint32 = DragCopy | DragLink | DragMove
)

// DragImage is the picture a page gives its drag: BGRA premultiplied.
type DragImage struct {
	Pixels                []byte
	Width, Height, Stride int
	Scale                 float32 // device pixels per CSS pixel
	OffsetX, OffsetY      float32 // the cursor in the image, CSS pixels
}

// Drag is a drag the page started.
type Drag struct {
	DataJSON   string  // what it carries (<zebkit/page.h>)
	Operations uint32  // what the page allows
	X, Y       float32 // where it started, CSS pixels
	Image      *DragImage
}

// OnDragStart calls function when the page starts dragging: return false to
// let the engine run the drag in the page, true to run it yourself
// (DragEnter and the rest, then DragSourceEnd).
func (v *View) OnDragStart(function func(*Drag) bool) {
	if function == nil {
		C.zebkit_view_on_drag_start(v.view, nil, nil)
		v.slot("drag-start").clear()
		return
	}
	h := v.slot("drag-start").set(func(d *C.zebkit_drag) bool {
		drag := &Drag{DataJSON: goString(d.data_json), Operations: uint32(d.operations), X: float32(d.x), Y: float32(d.y)}
		if d.image != nil && d.image_height > 0 && d.image_stride > 0 {
			drag.Image = &DragImage{
				Pixels: C.GoBytes(unsafe.Pointer(d.image), d.image_height*d.image_stride),
				Width:  int(d.image_width), Height: int(d.image_height), Stride: int(d.image_stride),
				Scale: float32(d.image_scale), OffsetX: float32(d.image_offset_x), OffsetY: float32(d.image_offset_y),
			}
		}
		return function(drag)
	})
	C.zebkit_view_on_drag_start(v.view, C.zebkit_drag_func(C.zkc_drag), data(h))
}

// OnDragEnd calls function when the page's drag ended, with what was done.
func (v *View) OnDragEnd(function func(operation uint32)) {
	if function == nil {
		C.zebkit_view_on_drag_end(v.view, nil, nil)
		v.slot("drag-end").clear()
		return
	}
	h := v.slot("drag-end").set(function)
	C.zebkit_view_on_drag_end(v.view, C.zebkit_drag_end_func(C.zkc_drag_operation), data(h))
}

func (v *View) IsDragging() bool { return bool(C.zebkit_view_is_dragging(v.view)) }

// DragEnter starts a drag into the page from outside it, at (x, y).
func (v *View) DragEnter(dataJSON string, x, y float32, operations uint32, modifiers Modifiers) {
	d := cString(dataJSON)
	defer free(d)
	C.zebkit_view_drag_enter(v.view, d, C.float(x), C.float(y), C.uint(operations), C.uint(modifiers))
}

// operationDone is the handle of a once-called operation function.
func operationDone(done func(uint32)) C.uintptr_t {
	var h C.uintptr_t
	h = handle(func(operation uint32) {
		cgo.Handle(h).Delete()
		if done != nil {
			done(operation)
		}
	})
	return h
}

// DragOver moves the drag; done gets the operation the page chose.
func (v *View) DragOver(x, y float32, operations uint32, modifiers Modifiers, done func(operation uint32)) {
	C.zebkit_view_drag_over(v.view, C.float(x), C.float(y), C.uint(operations), C.uint(modifiers),
		C.zebkit_drag_operation_func(C.zkc_drag_operation), data(operationDone(done)))
}

func (v *View) DragLeave() { C.zebkit_view_drag_leave(v.view) }

// Drop drops the drag at (x, y); done gets the operation done.
func (v *View) Drop(x, y float32, modifiers Modifiers, done func(operation uint32)) {
	C.zebkit_view_drop(v.view, C.float(x), C.float(y), C.uint(modifiers),
		C.zebkit_drag_operation_func(C.zkc_drag_operation), data(operationDone(done)))
}

// DragSourceEnd ends the drag the page started, with what was done.
func (v *View) DragSourceEnd(x, y float32, operation uint32) {
	C.zebkit_view_drag_source_end(v.view, C.float(x), C.float(y), C.uint(operation))
}
