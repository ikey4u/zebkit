package zebkit

/*
#include "bridge_b.h"
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// The engine calls these through bridge_b.c with the handle of the Go
// function to run: on its thread for Fetch, and for the PDF hooks on the
// thread that made the call that needs them.

//export goZkBDestroy
func goZkBDestroy(h C.uintptr_t) {
	cgo.Handle(h).Delete()
}

//export goZkBFetch
func goZkBFetch(status C.int, url, headers *C.char, body unsafe.Pointer, size C.size_t, failure *C.char, h C.uintptr_t) {
	response := &FetchResponse{Status: int(status), URL: goString(url), Headers: goString(headers)}
	if body != nil && size > 0 {
		response.Body = C.GoBytes(body, C.int(size))
	}
	if failure != nil {
		response.Err = &Error{C.GoString(failure)}
	}
	function := cgo.Handle(h).Value().(func(*FetchResponse))
	cgo.Handle(h).Delete()
	function(response)
}

//export goZkBBefore
func goZkBBefore(pdf *C.zebkit_pdf, operation *C.char, change, reason **C.char, h C.uintptr_t) C.int {
	decision := cgo.Handle(h).Value().(func(*PDF, string) PDFDecision)(borrowedPDF(pdf), goString(operation))
	switch decision.kind {
	case decisionChange:
		*change = pdfString(decision.text)
	case decisionRefuse:
		if decision.text != "" {
			*reason = pdfString(decision.text)
		}
		return C.ZEBKIT_PDF_REFUSE
	}
	return C.ZEBKIT_PDF_CONTINUE
}

//export goZkBAfter
func goZkBAfter(pdf *C.zebkit_pdf, operation *C.char, h C.uintptr_t) {
	cgo.Handle(h).Value().(func(*PDF, string))(borrowedPDF(pdf), goString(operation))
}

//export goZkBAppearance
func goZkBAppearance(canvas *C.zebkit_canvas, pdf *C.zebkit_pdf, page, index C.int, annotation *C.char,
	width, height C.double, h C.uintptr_t) C.bool {
	var draw AppearanceFunc
	switch value := cgo.Handle(h).Value().(type) {
	case AppearanceFunc:
		draw = value
	case *pdfKind:
		draw = value.draw
	}
	if draw == nil {
		return false
	}
	return C.bool(draw(borrowedCanvas(canvas), goString(annotation), float64(width), float64(height)))
}

//export goZkBCheck
func goZkBCheck(dataJSON *C.char, h C.uintptr_t) *C.char {
	kind := cgo.Handle(h).Value().(*pdfKind)
	if kind.check == nil {
		return nil
	}
	if problem := kind.check(goString(dataJSON)); problem != "" {
		return pdfString(problem)
	}
	return nil
}

//export goZkBHit
func goZkBHit(pdf *C.zebkit_pdf, page, index C.int, annotation *C.char, x, y C.double, h C.uintptr_t) C.bool {
	kind := cgo.Handle(h).Value().(*pdfKind)
	return C.bool(kind.hitTest(borrowedPDF(pdf), int(page), int(index), goString(annotation), float64(x), float64(y)))
}

//export goZkBLayer
func goZkBLayer(canvas *C.zebkit_canvas, pdf *C.zebkit_pdf, page C.int, info *C.zebkit_pdf_layer_info, h C.uintptr_t) {
	layer := PDFLayerInfo{Width: int(info.width), Height: int(info.height), Scale: float64(info.scale),
		Rotation: int(info.rotation)}
	for i := range layer.Matrix {
		layer.Matrix[i] = float64(info.matrix[i])
	}
	cgo.Handle(h).Value().(func(*Canvas, int, PDFLayerInfo))(borrowedCanvas(canvas), int(page), layer)
}

//export goZkBOcr
func goZkBOcr(pdf *C.zebkit_pdf, page C.int, rgba unsafe.Pointer, width, height C.int, scale C.double, h C.uintptr_t) *C.char {
	pixels := &Pixels{Width: int(width), Height: int(height)}
	if rgba != nil && width > 0 && height > 0 {
		pixels.RGBA = C.GoBytes(rgba, width*height*4)
	}
	text, ok := cgo.Handle(h).Value().(func(int, *Pixels, float64) (string, bool))(int(page), pixels, float64(scale))
	if !ok {
		return nil
	}
	return pdfString(text)
}

//export goZkBFont
func goZkBFont(pdf *C.zebkit_pdf, family *C.char, size *C.size_t, h C.uintptr_t) *C.uint8_t {
	font := cgo.Handle(h).Value().(func(string) []byte)(goString(family))
	if len(font) == 0 {
		return nil
	}
	*size = C.size_t(len(font))
	return C.zebkit_pdf_bytes(unsafe.Pointer(&font[0]), C.size_t(len(font)))
}

//export goZkBTrust
func goZkBTrust(pdf *C.zebkit_pdf, signature *C.char, contents unsafe.Pointer, size C.size_t, h C.uintptr_t) C.int {
	var bytes []byte
	if contents != nil && size > 0 {
		bytes = C.GoBytes(contents, C.int(size))
	}
	return C.int(cgo.Handle(h).Value().(func(string, []byte) PDFTrust)(goString(signature), bytes))
}

//export goZkBAction
func goZkBAction(pdf *C.zebkit_pdf, action *C.char, h C.uintptr_t) C.bool {
	return C.bool(cgo.Handle(h).Value().(func(string) bool)(goString(action)))
}

//export goZkBInput
func goZkBInput(pdf *C.zebkit_pdf, tool, points *C.char, h C.uintptr_t) *C.char {
	text, ok := cgo.Handle(h).Value().(func(string, string) (string, bool))(goString(tool), goString(points))
	if !ok {
		return nil
	}
	return pdfString(text)
}
