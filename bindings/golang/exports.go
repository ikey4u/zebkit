package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"time"
	"unsafe"
)

// The engine calls these through bridge.c with the handle of the Go function
// to run: on its thread, but for the PDF hooks (the thread of the call that
// needs them) and SetWakeup's function (any thread). A handle let go (its view
// closed, its hook removed) finds nothing, and the call does nothing; a frame
// is still given back.

//export goZkTask
func goZkTask(h C.uintptr_t) {
	if f, ok := lookup[func()](h); ok {
		f()
	}
}

//export goZkText
func goZkText(text *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(string)](h); ok {
		f(goString(text))
	}
}

//export goZkViewClosed
func goZkViewClosed(view *C.zebkit_view) { viewClosed(view) }

//export goZkLoad
func goZkLoad(event C.int, url *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(LoadEvent, string)](h); ok {
		f(LoadEvent(event), goString(url))
	}
}

//export goZkResult
func goZkResult(result, failure *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(string, string, bool)](h); ok {
		f(goString(result), goString(failure), failure != nil)
	}
}

//export goZkBytes
func goZkBytes(bytes unsafe.Pointer, size C.size_t, failure *C.char, h C.uintptr_t) {
	if f, ok := lookup[func([]byte, string, bool)](h); ok {
		f(goBytes(bytes, size), goString(failure), failure != nil)
	}
}

//export goZkBind
func goZkBind(origin, args *C.char, call *C.zebkit_call, h C.uintptr_t) {
	f, ok := lookup[func(*Call)](h)
	if !ok {
		// Nobody to answer: the page's promise is rejected, not left waiting.
		reason := cString("the function is gone")
		defer free(reason)
		C.zebkit_call_reject(call, reason)
		return
	}
	f(newCall(call, goString(origin), json.RawMessage(goString(args))))
}

//export goZkScheme
func goZkScheme(request *C.zebkit_request, h C.uintptr_t) {
	if f, ok := lookup[func(*Request)](h); ok {
		f(newRequest(request))
		return
	}
	C.zebkit_request_respond(request, 404, nil, nil, 0)
}

//export goZkAsked
func goZkAsked(request unsafe.Pointer, text *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(unsafe.Pointer, string)](h); ok {
		f(request, goString(text))
	}
}

//export goZkWakeup
func goZkWakeup(delay C.int64_t, h C.uintptr_t) {
	if delay < 0 {
		delay = 0
	}
	if f, ok := lookup[func(time.Duration)](h); ok {
		f(time.Duration(delay) * time.Millisecond)
	}
}

//export goZkWindowOpen
func goZkWindowOpen(opened *C.zebkit_view, url *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(*C.zebkit_view, string)](h); ok {
		f(opened, goString(url))
	}
}

//export goZkExport
func goZkExport(text *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(string, bool)](h); ok {
		f(goString(text), text != nil)
	}
}

//export goZkBody
func goZkBody(id *C.char, bytes unsafe.Pointer, size C.size_t, end, truncated C.bool, h C.uintptr_t) {
	if f, ok := lookup[func(string, []byte, bool, bool)](h); ok {
		f(goString(id), goBytes(bytes, size), bool(end), bool(truncated))
	}
}

//export goZkPixels
func goZkPixels(pixels unsafe.Pointer, width, height, stride C.int, failure *C.char, h C.uintptr_t) {
	f, ok := lookup[func([]byte, int, int, int, string, bool)](h)
	if !ok {
		return
	}
	var copied []byte
	if height > 0 && stride > 0 {
		copied = goBytes(pixels, C.size_t(height)*C.size_t(stride))
	}
	f(copied, int(width), int(height), int(stride), goString(failure), failure != nil)
}

//export goZkEvent
func goZkEvent(event, text *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(string, json.RawMessage)](h); ok {
		f(goString(event), json.RawMessage(goString(text)))
	}
}

//export goZkFrame
func goZkFrame(frame *C.zebkit_frame, h C.uintptr_t) {
	if f, ok := lookup[func(*C.zebkit_frame)](h); ok {
		f(frame)
		return
	}
	// Every frame goes back to the engine, also after its view has closed.
	C.zebkit_frame_release(frame)
}

//export goZkRect
func goZkRect(present C.bool, rect *C.zebkit_rect, h C.uintptr_t) {
	if f, ok := lookup[func(*C.zebkit_rect)](h); ok {
		if !present {
			rect = nil
		}
		f(rect)
	}
}

//export goZkCursor
func goZkCursor(cursor *C.char, image *C.zebkit_cursor_image, h C.uintptr_t) {
	if f, ok := lookup[func(string, *C.zebkit_cursor_image)](h); ok {
		f(goString(cursor), image)
	}
}

//export goZkDialog
func goZkDialog(dialog *C.zebkit_dialog, h C.uintptr_t) {
	if f, ok := lookup[func(*C.zebkit_dialog)](h); ok {
		f(dialog)
	}
}

//export goZkDialogClosed
func goZkDialogClosed(id C.uint64_t, accepted C.bool, input *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(uint64, bool, string)](h); ok {
		f(uint64(id), bool(accepted), goString(input))
	}
}

//export goZkDrag
func goZkDrag(drag *C.zebkit_drag, h C.uintptr_t) C.bool {
	if f, ok := lookup[func(*C.zebkit_drag) bool](h); ok {
		return C.bool(f(drag))
	}
	return false
}

//export goZkOperation
func goZkOperation(operation C.uint, h C.uintptr_t) {
	if f, ok := lookup[func(DragOperation)](h); ok {
		f(DragOperation(operation))
	}
}

//export goZkTap
func goZkTap(text *C.char, bytes unsafe.Pointer, size C.size_t, h C.uintptr_t) {
	if f, ok := lookup[func(json.RawMessage, []byte)](h); ok {
		f(json.RawMessage(goString(text)), goBytes(bytes, size))
	}
}

//export goZkProgress
func goZkProgress(packets, timeUs C.int64_t, h C.uintptr_t) C.int {
	if f, ok := lookup[func(int64, int64) bool](h); ok && f(int64(packets), int64(timeUs)) {
		return 1
	}
	return 0
}

//export goZkRelease
func goZkRelease(h C.uintptr_t) { release(h) }

//export goZkFetch
func goZkFetch(status C.int, url, headers *C.char, body unsafe.Pointer, size C.size_t, failure *C.char, h C.uintptr_t) {
	f, ok := lookup[func(*FetchResponse)](h)
	if !ok {
		return
	}
	response := &FetchResponse{Status: int(status), URL: goString(url), Headers: goString(headers), Body: goBytes(body, size)}
	if failure != nil {
		response.Err = &Error{C.GoString(failure)}
	}
	f(response)
}

// ---- PDF hooks: the document and canvas they are given are theirs only
// while they run.

//export goZkBefore
func goZkBefore(pdf *C.zebkit_pdf, operation *C.char, change, reason **C.char, h C.uintptr_t) C.int {
	f, ok := lookup[func(*PDF, json.RawMessage) PDFDecision](h)
	if !ok {
		return C.ZEBKIT_PDF_CONTINUE
	}
	doc := borrowedPDF(pdf)
	defer doc.expire()
	decision := f(doc, json.RawMessage(goString(operation)))
	switch decision.kind {
	case decisionChange:
		text, err := jsonArg(decision.change)
		if err != nil {
			*reason = pdfString("the hook's change is not JSON: " + err.Error())
			return C.ZEBKIT_PDF_REFUSE
		}
		defer free(text)
		*change = C.zebkit_pdf_string(text)
	case decisionRefuse:
		if decision.reason != "" {
			*reason = pdfString(decision.reason)
		}
		return C.ZEBKIT_PDF_REFUSE
	}
	return C.ZEBKIT_PDF_CONTINUE
}

//export goZkAfter
func goZkAfter(pdf *C.zebkit_pdf, operation *C.char, h C.uintptr_t) {
	if f, ok := lookup[func(*PDF, json.RawMessage)](h); ok {
		doc := borrowedPDF(pdf)
		defer doc.expire()
		f(doc, json.RawMessage(goString(operation)))
	}
}

//export goZkAppearance
func goZkAppearance(canvas *C.zebkit_canvas, pdf *C.zebkit_pdf, page, index C.int, annotation *C.char,
	width, height C.double, h C.uintptr_t) C.bool {
	var draw AppearanceFunc
	if f, ok := lookup[AppearanceFunc](h); ok {
		draw = f
	} else if kind, ok := lookup[*pdfKind](h); ok {
		draw = kind.draw
	}
	if draw == nil {
		return false
	}
	c := borrowedCanvas(canvas)
	defer c.expire()
	return C.bool(draw(c, json.RawMessage(goString(annotation)), float64(width), float64(height)))
}

//export goZkCheck
func goZkCheck(dataJSON *C.char, h C.uintptr_t) *C.char {
	kind, ok := lookup[*pdfKind](h)
	if !ok || kind.check == nil {
		return nil
	}
	if problem := kind.check(json.RawMessage(goString(dataJSON))); problem != "" {
		return pdfString(problem)
	}
	return nil
}

//export goZkHit
func goZkHit(pdf *C.zebkit_pdf, page, index C.int, annotation *C.char, x, y C.double, h C.uintptr_t) C.bool {
	kind, ok := lookup[*pdfKind](h)
	if !ok || kind.hitTest == nil {
		return false
	}
	doc := borrowedPDF(pdf)
	defer doc.expire()
	return C.bool(kind.hitTest(doc, int(page), int(index), json.RawMessage(goString(annotation)), float64(x), float64(y)))
}

//export goZkLayer
func goZkLayer(canvas *C.zebkit_canvas, pdf *C.zebkit_pdf, page C.int, info *C.zebkit_pdf_layer_info, h C.uintptr_t) {
	f, ok := lookup[func(*Canvas, int, PDFLayerInfo)](h)
	if !ok {
		return
	}
	layer := PDFLayerInfo{Width: int(info.width), Height: int(info.height), Scale: float64(info.scale),
		Rotation: int(info.rotation)}
	for i := range layer.Matrix {
		layer.Matrix[i] = float64(info.matrix[i])
	}
	c := borrowedCanvas(canvas)
	defer c.expire()
	f(c, int(page), layer)
}

//export goZkOcr
func goZkOcr(pdf *C.zebkit_pdf, page C.int, rgba unsafe.Pointer, width, height C.int, scale C.double, h C.uintptr_t) *C.char {
	f, ok := lookup[func(int, *Pixels, float64) (any, bool)](h)
	if !ok {
		return nil
	}
	pixels := &Pixels{Width: int(width), Height: int(height), RGBA: goBytes(rgba, rgbaSize(width, height))}
	text, found := f(int(page), pixels, float64(scale))
	if !found {
		return nil
	}
	return pdfJSON(text)
}

//export goZkFont
func goZkFont(pdf *C.zebkit_pdf, family *C.char, size *C.size_t, h C.uintptr_t) *C.uint8_t {
	f, ok := lookup[func(string) []byte](h)
	if !ok {
		return nil
	}
	font := f(goString(family))
	if len(font) == 0 {
		return nil
	}
	*size = C.size_t(len(font))
	return C.zebkit_pdf_bytes(unsafe.Pointer(&font[0]), C.size_t(len(font)))
}

//export goZkTrust
func goZkTrust(pdf *C.zebkit_pdf, signature *C.char, contents unsafe.Pointer, size C.size_t, h C.uintptr_t) C.int {
	f, ok := lookup[func(json.RawMessage, []byte) PDFTrust](h)
	if !ok {
		return C.int(PDFTrustUnknown)
	}
	return C.int(f(json.RawMessage(goString(signature)), goBytes(contents, size)))
}

//export goZkAction
func goZkAction(pdf *C.zebkit_pdf, action *C.char, h C.uintptr_t) C.bool {
	if f, ok := lookup[func(json.RawMessage) bool](h); ok {
		return C.bool(f(json.RawMessage(goString(action))))
	}
	return false
}

//export goZkInput
func goZkInput(pdf *C.zebkit_pdf, tool, points *C.char, h C.uintptr_t) *C.char {
	f, ok := lookup[func(string, json.RawMessage) (any, bool)](h)
	if !ok {
		return nil
	}
	changed, ok := f(goString(tool), json.RawMessage(goString(points)))
	if !ok {
		return nil
	}
	return pdfJSON(changed)
}
