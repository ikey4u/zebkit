package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"strings"
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

var dialogNames = []string{"alert", "confirm", "prompt", "beforeunload", "print"}

// Name is the type as the engine's JSON names it.
func (t DialogType) Name() string {
	if t >= 0 && int(t) < len(dialogNames) {
		return dialogNames[t]
	}
	return "unknown"
}

// UnmarshalJSON reads the type's name.
func (t *DialogType) UnmarshalJSON(text []byte) error {
	var name string
	if err := json.Unmarshal(text, &name); err != nil {
		return err
	}
	*t = -1
	for i, known := range dialogNames {
		if known == name {
			*t = DialogType(i)
		}
	}
	return nil
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
	var value func(*C.zebkit_dialog)
	if function != nil {
		value = func(d *C.zebkit_dialog) {
			function(&Dialog{
				ID: uint64(d.id), Type: DialogType(d._type), Message: goString(d.message),
				DefaultPrompt: goString(d.default_prompt), URL: goString(d.url),
				IsReload: bool(d.is_reload), Held: bool(d.held),
			})
		}
	}
	v.hook("dialog", function, value, unsafe.Pointer(C.zk_dialog), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_dialog(raw, C.zebkit_dialog_func(f), d)
	})
}

// OnDialogClosed calls function when a dialog closes, however it was
// answered; userInput is a prompt's text. nil stops.
func (v *View) OnDialogClosed(function func(id uint64, accepted bool, userInput string)) {
	v.hook("dialog-closed", function, function, unsafe.Pointer(C.zk_dialog_closed), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_dialog_closed(raw, C.zebkit_dialog_closed_func(f), d)
	})
}

// SetDialogPolicy says what becomes of the view's dialogs.
func (v *View) SetDialogPolicy(policy DialogPolicy) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_dialog_policy(raw, C.zebkit_dialog_policy(policy))
	}
}

// DialogPolicy is the view's dialog policy.
func (v *View) DialogPolicy() DialogPolicy {
	if raw, ok := v.raw(); ok {
		return DialogPolicy(C.zebkit_view_get_dialog_policy(raw))
	}
	return DialogPolicyDefault
}

// RespondDialog answers dialog id: OK (accept) or Cancel; a prompt with
// promptText ("" for its default).
func (v *View) RespondDialog(id uint64, accept bool, promptText string) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	text := optionalString(promptText)
	defer free(text)
	return check(C.zebkit_view_dialog_respond(raw, C.uint64_t(id), C.bool(accept), text), "no such dialog is open")
}

// Dialogs are the view's open dialogs, oldest first; all of them are held.
func (v *View) Dialogs() ([]Dialog, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	// The engine writes the ids as JSON numbers that may have a fraction.
	var open []struct {
		ID            float64    `json:"id"`
		Type          DialogType `json:"type"`
		Message       string     `json:"message"`
		DefaultPrompt string     `json:"defaultPrompt"`
		URL           string     `json:"url"`
		IsReload      bool       `json:"isReload"`
	}
	if err := takeJSON(C.zebkit_view_dialogs(raw), &open, "no dialogs"); err != nil {
		return nil, err
	}
	dialogs := make([]Dialog, len(open))
	for i, d := range open {
		dialogs[i] = Dialog{ID: uint64(d.ID), Type: d.Type, Message: d.Message, DefaultPrompt: d.DefaultPrompt, URL: d.URL,
			IsReload: d.IsReload, Held: true}
	}
	return dialogs, nil
}

// FindFlags is how Find looks: FindMatchCase, FindBackward and
// FindHighlightOnly, combined.
type FindFlags uint

const (
	// FindMatchCase tells upper from lower case.
	FindMatchCase FindFlags = 1 << 0
	// FindBackward goes to the previous match rather than the next.
	FindBackward FindFlags = 1 << 1
	// FindHighlightOnly counts and highlights the matches, none made active
	// and nothing scrolled.
	FindHighlightOnly FindFlags = 1 << 2
)

// FindStop is what becomes of the active match when a search stops.
type FindStop int

const (
	// FindStopClear: the highlights go and the active match is not selected.
	FindStopClear FindStop = 0
	// FindStopKeep: the highlights go; the active match stays selected.
	FindStopKeep FindStop = 1
	// FindStopActivate: the highlights go and the active match's element is
	// activated (a link in it followed).
	FindStopActivate FindStop = 2
)

// Find looks for text in the page as Chrome's find bar does: in every frame
// and shadow tree, all matches highlighted, the active one distinctly. The
// same text again goes to the next match (FindBackward: the previous). It
// returns the request's number, its results' requestId (OnFind). Empty text
// stops the search, as StopFinding(FindStopClear), and returns 0 and nil; a
// closed view returns ErrClosed; text that is not UTF-8, or holds a zero
// byte, is refused.
func (v *View) Find(text string, flags FindFlags) (int, error) {
	raw, ok := v.raw()
	if !ok {
		return 0, ErrClosed
	}
	if strings.IndexByte(text, 0) >= 0 {
		return 0, refused("Find: the text holds a zero byte")
	}
	t := C.CString(text)
	defer free(t)
	request := int(C.zebkit_view_find(raw, t, C.uint(flags)))
	if request == 0 && text != "" {
		return 0, refused("Find: the text is not UTF-8, or the view has gone")
	}
	return request, nil
}

// StopFinding stops the search and clears its highlights; OnFind is told a
// result with stopped true when one was on.
func (v *View) StopFinding(action FindStop) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_stop_finding(raw, C.zebkit_find_stop_action(action))
	}
}

// FindResult is a result of Find, as OnFind and View.FindResult give it.
type FindResult struct {
	// RequestID is the Find it answers.
	RequestID int
	Text      string
	// Matches is the number of matches so far.
	Matches int
	// ActiveMatch is the active one, from 1; 0 for none.
	ActiveMatch int
	// Final: the last result of its request, the counting done.
	Final bool
	// Rect is the active match, in the view's CSS pixels; nil for none.
	Rect *Rect
	// Stopped: the search has stopped (StopFinding, or its document went).
	Stopped bool
	// JSON is all of it, as the engine gave it.
	JSON json.RawMessage
}

func findResultOf(text string) FindResult {
	var value struct {
		RequestID   float64 `json:"requestId"`
		Text        string  `json:"text"`
		Matches     float64 `json:"matches"`
		ActiveMatch float64 `json:"activeMatch"`
		Final       bool    `json:"final"`
		Rect        *struct{ X, Y, Width, Height float64 }
		Stopped     bool `json:"stopped"`
	}
	_ = json.Unmarshal([]byte(text), &value)
	result := FindResult{RequestID: int(value.RequestID), Text: value.Text, Matches: int(value.Matches),
		ActiveMatch: int(value.ActiveMatch), Final: value.Final, Stopped: value.Stopped, JSON: json.RawMessage(text)}
	if r := value.Rect; r != nil {
		result.Rect = &Rect{int32(r.X), int32(r.Y), int32(r.Width), int32(r.Height)}
	}
	return result
}

// OnFind tells function each result of Find; nil stops.
func (v *View) OnFind(function func(FindResult)) {
	var value func(string)
	if function != nil {
		value = func(text string) { function(findResultOf(text)) }
	}
	v.hook("find", function, value, unsafe.Pointer(C.zk_view_text), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_find(raw, C.zebkit_find_func(f), d)
	})
}

// FindResult is the last result of the search on, as OnFind has them; with
// none on, Text "", Matches 0 and Stopped true.
func (v *View) FindResult() (FindResult, error) {
	raw, ok := v.raw()
	if !ok {
		return FindResult{}, ErrClosed
	}
	text, err := takeRaw(C.zebkit_view_find_result(raw), "no find result")
	if err != nil {
		return FindResult{}, err
	}
	return findResultOf(string(text)), nil
}

// RequestClose closes the view as a browser closes a tab: the page's
// beforeunload handlers first.
func (v *View) RequestClose() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_request_close(raw)
	}
}

// PrintToPDF prints the page to PDF with CDP's Page.printToPDF parameters,
// JSON (nil for the defaults).
func (v *View) PrintToPDF(options any, done func(pdf []byte, err error)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	coptions, err := optionalJSON(options)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	defer free(coptions)
	C.zebkit_view_print_to_pdf(raw, coptions, C.zebkit_pdf_func(C.zk_bytes), data(bytesOnce(done)))
}

// DragOperation is a drag's operations: a mask of what is allowed, or what
// was done.
type DragOperation uint32

const (
	DragNone DragOperation = 0
	DragCopy DragOperation = 1
	DragLink DragOperation = 2
	DragMove DragOperation = 16
	DragAll  DragOperation = DragCopy | DragLink | DragMove
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
	Data       json.RawMessage // what it carries (<zebkit/page.h>)
	Operations DragOperation   // what the page allows
	X, Y       float32         // where it started, CSS pixels
	Image      *DragImage
}

// OnDragStart calls function when the page starts dragging: return false to
// let the engine run the drag in the page, true to run it yourself
// (DragEnter and the rest, then DragSourceEnd). nil stops.
func (v *View) OnDragStart(function func(*Drag) bool) {
	var value func(*C.zebkit_drag) bool
	if function != nil {
		value = func(d *C.zebkit_drag) bool {
			drag := &Drag{Data: json.RawMessage(goString(d.data_json)), Operations: DragOperation(d.operations),
				X: float32(d.x), Y: float32(d.y)}
			if d.image != nil && d.image_height > 0 && d.image_stride > 0 {
				drag.Image = &DragImage{
					Pixels: goBytes(unsafe.Pointer(d.image), C.size_t(d.image_height)*C.size_t(d.image_stride)),
					Width:  int(d.image_width), Height: int(d.image_height), Stride: int(d.image_stride),
					Scale: float32(d.image_scale), OffsetX: float32(d.image_offset_x), OffsetY: float32(d.image_offset_y),
				}
			}
			return function(drag)
		}
	}
	v.hook("drag-start", function, value, unsafe.Pointer(C.zk_drag), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_drag_start(raw, C.zebkit_drag_func(f), d)
	})
}

// OnDragEnd calls function when the page's drag ended, with what was done;
// nil stops.
func (v *View) OnDragEnd(function func(operation DragOperation)) {
	v.hook("drag-end", function, function, unsafe.Pointer(C.zk_drag_operation), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_drag_end(raw, C.zebkit_drag_end_func(f), d)
	})
}

// IsDragging says whether a drag is over the page.
func (v *View) IsDragging() bool {
	raw, ok := v.raw()
	return ok && bool(C.zebkit_view_is_dragging(raw))
}

// DragEnter starts a drag into the page from outside it, at (x, y), carrying
// data, JSON (<zebkit/page.h>).
func (v *View) DragEnter(data any, x, y float32, operations DragOperation, modifiers Modifiers) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	d, err := jsonArg(data)
	if err != nil {
		return err
	}
	defer free(d)
	C.zebkit_view_drag_enter(raw, d, C.float(x), C.float(y), C.uint(operations), C.uint(modifiers))
	return nil
}

// operationOnce is the handle of a once-called operation function.
func operationOnce(done func(DragOperation)) C.uintptr_t {
	return onceHandle(func(operation DragOperation) {
		if done != nil {
			done(operation)
		}
	})
}

// DragOver moves the drag; done gets the operation the page chose.
func (v *View) DragOver(x, y float32, operations DragOperation, modifiers Modifiers, done func(operation DragOperation)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(DragNone)
		}
		return
	}
	C.zebkit_view_drag_over(raw, C.float(x), C.float(y), C.uint(operations), C.uint(modifiers),
		C.zebkit_drag_operation_func(C.zk_drag_operation), data(operationOnce(done)))
}

// DragLeave takes the drag out of the page.
func (v *View) DragLeave() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_drag_leave(raw)
	}
}

// Drop drops the drag at (x, y); done gets the operation done.
func (v *View) Drop(x, y float32, modifiers Modifiers, done func(operation DragOperation)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(DragNone)
		}
		return
	}
	C.zebkit_view_drop(raw, C.float(x), C.float(y), C.uint(modifiers),
		C.zebkit_drag_operation_func(C.zk_drag_operation), data(operationOnce(done)))
}

// DragSourceEnd ends the drag the page started, with what was done.
func (v *View) DragSourceEnd(x, y float32, operation DragOperation) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_drag_source_end(raw, C.float(x), C.float(y), C.uint(operation))
	}
}
