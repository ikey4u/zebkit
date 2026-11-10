package zebkit

/*
#include "bridge_b.h"
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// PDF is a document of the engine's PDFium (<zebkit/pdf.h>): read, rendered,
// searched, annotated, filled in, changed and saved. It needs no engine
// running; any goroutine may use it, one at a time. Coordinates are PDF page
// space: points, the origin at the bottom left, y up; a rectangle is [left,
// bottom, right, top]; pages count from 0. Structured answers are JSON
// strings, as <zebkit/pdf.h> describes them.
type PDF struct {
	p        *C.zebkit_pdf
	borrowed bool
}

// borrowedPDF is the document a hook is given: not the hook's to close.
func borrowedPDF(p *C.zebkit_pdf) *PDF {
	if p == nil {
		return nil
	}
	return &PDF{p: p, borrowed: true}
}

func (d *PDF) raw() *C.zebkit_pdf {
	if d == nil {
		return nil
	}
	return d.p
}

// OpenPDF opens a PDF from data, which is copied; password "" for none. It
// fails with "password" when one is needed or wrong.
func OpenPDF(data []byte, password string) (*PDF, error) {
	cpassword := optionalString(password)
	defer free(cpassword)
	var message *C.char
	p := C.zebkit_pdf_open(bytesArg(data), C.size_t(len(data)), cpassword, &message)
	if p == nil {
		return nil, takeError(message, "not a PDF")
	}
	return &PDF{p: p}, nil
}

// OpenPDFFile opens a PDF file, read as it is needed: it must stay.
func OpenPDFFile(path, password string) (*PDF, error) {
	cpath := cString(path)
	defer free(cpath)
	cpassword := optionalString(password)
	defer free(cpassword)
	var message *C.char
	p := C.zebkit_pdf_open_file(cpath, cpassword, &message)
	if p == nil {
		return nil, takeError(message, "not a PDF")
	}
	return &PDF{p: p}, nil
}

// NewPDF is a new, empty document, to build from others' pages.
func NewPDF() (*PDF, error) {
	var message *C.char
	p := C.zebkit_pdf_new(&message)
	if p == nil {
		return nil, takeError(message, "no document")
	}
	return &PDF{p: p}, nil
}

// Close frees the document; one a hook was given is not the hook's to close.
func (d *PDF) Close() {
	if d.p != nil && !d.borrowed {
		C.zebkit_pdf_close(d.p)
	}
	d.p = nil
}

// jsonOrError returns an engine-allocated JSON (or text) answer, or the error.
func jsonOrError(out, message *C.char, otherwise string) (string, error) {
	if out == nil {
		return "", takeError(message, otherwise)
	}
	return takeString(out), nil
}

func boolOrError(ok bool, message *C.char, otherwise string) error {
	if !ok {
		return takeError(message, otherwise)
	}
	return nil
}

// PDFSaveFlags say how Save writes the file.
type PDFSaveFlags uint

const (
	PDFSaveIncremental PDFSaveFlags = 1 << 0 // append the changes; keeps signatures valid
	PDFSaveNoSecurity  PDFSaveFlags = 1 << 1 // write it without its encryption
)

// Save is the document as a file, with the changes made to it.
func (d *PDF) Save(flags PDFSaveFlags) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_save(d.p, C.uint(flags), &size, &message)
	if out == nil {
		return nil, takeError(message, "not saved")
	}
	return takeBytes(out, size), nil
}

// Info is what the document is: pages, version, encryption, permissions,
// form, metadata.
func (d *PDF) Info() string { return takeString(C.zebkit_pdf_info(d.p)) }

// SetMetadata sets the document information from a JSON object of
// Info's "metadata" keys: a string sets one, null removes it.
func (d *PDF) SetMetadata(json string) error {
	cjson := cString(json)
	defer free(cjson)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_set_metadata(d.p, cjson, &message)), message, "metadata refused")
}

func (d *PDF) PageCount() int { return int(C.zebkit_pdf_page_count(d.p)) }

// Page is a page's size, rotation, label and boxes.
func (d *PDF) Page(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_page(d.p, C.int(page), &message), message, "no such page")
}

// PDFRenderFlags say what is drawn and how.
type PDFRenderFlags uint

const (
	PDFRenderAnnotations PDFRenderFlags = 1 << 0
	PDFRenderForms       PDFRenderFlags = 1 << 1
	PDFRenderLCDText     PDFRenderFlags = 1 << 2
	PDFRenderGrayscale   PDFRenderFlags = 1 << 3
	PDFRenderPrinting    PDFRenderFlags = 1 << 4
	PDFRenderNoSmoothing PDFRenderFlags = 1 << 5
)

// PDFRenderOptions: Scale is pixels per point (0 is 1), Rotation 0, 90, 180
// or 270 added to the page's own, Background 0xRRGGBBAA (0 transparent);
// X, Y, Width, Height a tile of the rendered page's pixels (0 Width or
// Height: the whole page).
type PDFRenderOptions struct {
	Scale               float32
	Rotation            int
	Flags               PDFRenderFlags
	Background          uint32
	X, Y, Width, Height int
}

func renderOptions(o *PDFRenderOptions) (*C.zebkit_pdf_render_options, func()) {
	if o == nil {
		return nil, func() {}
	}
	c := (*C.zebkit_pdf_render_options)(C.calloc(1, C.size_t(unsafe.Sizeof(C.zebkit_pdf_render_options{}))))
	c.scale = C.float(o.Scale)
	c.rotation = C.int(o.Rotation)
	c.flags = C.uint(o.Flags)
	c.background = C.uint32_t(o.Background)
	c.x, c.y, c.width, c.height = C.int(o.X), C.int(o.Y), C.int(o.Width), C.int(o.Height)
	return c, func() { C.free(unsafe.Pointer(c)) }
}

// Render renders a page; options nil is the whole page at scale 1 with
// annotations, on white.
func (d *PDF) Render(page int, options *PDFRenderOptions) (*Pixels, error) {
	o, done := renderOptions(options)
	defer done()
	var width, height C.int
	var message *C.char
	out := C.zebkit_pdf_render(d.p, C.int(page), o, &width, &height, &message)
	if out == nil {
		return nil, takeError(message, "not rendered")
	}
	return &Pixels{Width: int(width), Height: int(height), RGBA: takeBytes(out, C.size_t(width*height*4))}, nil
}

// RenderAnnotation renders appearance mode (0 normal, 1 rollover, 2 down) of
// annotation index alone, on a transparent background when options is nil.
func (d *PDF) RenderAnnotation(page, index, mode int, options *PDFRenderOptions) (*Pixels, error) {
	o, done := renderOptions(options)
	defer done()
	var width, height C.int
	var message *C.char
	out := C.zebkit_pdf_render_annotation(d.p, C.int(page), C.int(index), C.int(mode), o, &width, &height, &message)
	if out == nil {
		return nil, takeError(message, "not rendered")
	}
	return &Pixels{Width: int(width), Height: int(height), RGBA: takeBytes(out, C.size_t(width*height*4))}, nil
}

// Text is the page's text, in reading order.
func (d *PDF) Text(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text(d.p, C.int(page), &message), message, "no text")
}

// TextLayout is the page's characters with their boxes, fonts and sizes.
func (d *PDF) TextLayout(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text_layout(d.p, C.int(page), &message), message, "no text")
}

// TextRects are the rectangles covering count characters from start.
func (d *PDF) TextRects(page, start, count int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text_rects(d.p, C.int(page), C.int(start), C.int(count), &message), message, "no text")
}

// TextLines is the page's text as lines with their boxes.
func (d *PDF) TextLines(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text_lines(d.p, C.int(page), &message), message, "no text")
}

// CharAt is the index of the character at (x, y), within tolerance points,
// or -1.
func (d *PDF) CharAt(page int, x, y, tolerance float64) int {
	return int(C.zebkit_pdf_char_at(d.p, C.int(page), C.double(x), C.double(y), C.double(tolerance)))
}

// PDFSearchFlags say how Search matches.
type PDFSearchFlags uint

const (
	PDFSearchCase      PDFSearchFlags = 1 << 0
	PDFSearchWholeWord PDFSearchFlags = 1 << 1
)

// Search finds query from page first on, at most limit matches (0 for all):
// [{"page", "start", "count", "rects", "context"}].
func (d *PDF) Search(query string, flags PDFSearchFlags, first, limit int) (string, error) {
	cquery := cString(query)
	defer free(cquery)
	var message *C.char
	return jsonOrError(C.zebkit_pdf_search(d.p, cquery, C.uint(flags), C.int(first), C.int(limit), &message), message, "not searched")
}

// Images are the images the page draws: [{"width", "height", "rect", "jpeg"}].
func (d *PDF) Images(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_images(d.p, C.int(page), &message), message, "no images")
}

// ImageData is image index of the page: a JPEG file as stored (jpeg true),
// or its pixels as PNG.
func (d *PDF) ImageData(page, image int) (data []byte, jpeg bool, err error) {
	var size C.size_t
	var isJPEG C.bool
	var message *C.char
	out := C.zebkit_pdf_image_data(d.p, C.int(page), C.int(image), &size, &isJPEG, &message)
	if out == nil {
		return nil, false, takeError(message, "no such image")
	}
	return takeBytes(out, size), bool(isJPEG), nil
}

// Signatures are the document's digital signatures, each checked against
// the document as it was opened.
func (d *PDF) Signatures() (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_signatures(d.p, &message), message, "no signatures")
}

// SignatureContents is signature index's DER-encoded PKCS#7 or time stamp.
func (d *PDF) SignatureContents(index int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_signature_contents(d.p, C.int(index), &size, &message)
	if out == nil {
		return nil, takeError(message, "no such signature")
	}
	return takeBytes(out, size), nil
}

// Outline is the bookmarks: [{"title", "page", "x", "y", "zoom", "url",
// "open", "children"}].
func (d *PDF) Outline() (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_outline(d.p, &message), message, "no outline")
}

// SetOutline replaces the outline with an array in Outline's format; "[]" removes it.
func (d *PDF) SetOutline(json string) error {
	cjson := cString(json)
	defer free(cjson)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_set_outline(d.p, cjson, &message)), message, "outline refused")
}

// Links are the page's link annotations and the addresses in its text.
func (d *PDF) Links(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_links(d.p, C.int(page), &message), message, "no links")
}

// Annotations are the page's annotations, in their order on the page.
func (d *PDF) Annotations(page int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_annotations(d.p, C.int(page), &message), message, "no annotations")
}

// AddAnnotation adds an annotation from a JSON object ("type" and "rect" at
// least) and returns its index.
func (d *PDF) AddAnnotation(page int, json string) (int, error) {
	cjson := cString(json)
	defer free(cjson)
	var message *C.char
	index := int(C.zebkit_pdf_annotation_add(d.p, C.int(page), cjson, &message))
	if index < 0 {
		return -1, takeError(message, "annotation refused")
	}
	return index, nil
}

// UpdateAnnotation sets the keys json has on annotation index; null removes one.
func (d *PDF) UpdateAnnotation(page, index int, json string) error {
	cjson := cString(json)
	defer free(cjson)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_update(d.p, C.int(page), C.int(index), cjson, &message)), message, "annotation refused")
}

// RemoveAnnotation removes annotation index, with its popup and replies.
func (d *PDF) RemoveAnnotation(page, index int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_remove(d.p, C.int(page), C.int(index), &message)), message, "not removed")
}

// SetAnnotationImage makes image the appearance of stamp annotation index.
func (d *PDF) SetAnnotationImage(page, index int, image *Pixels) error {
	if image == nil || len(image.RGBA) < image.Width*image.Height*4 {
		return &Error{"fewer pixels than the size says"}
	}
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_set_image(d.p, C.int(page), C.int(index), (*C.uint8_t)(bytesArg(image.RGBA)),
		C.int(image.Width), C.int(image.Height), &message)), message, "image refused")
}

// Flatten makes the page's annotations and form fields part of its
// content, as shown or (forPrint) as printed.
func (d *PDF) Flatten(page int, forPrint bool) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_flatten(d.p, C.int(page), C.bool(forPrint), &message)), message, "not flattened")
}

// AnnotationAt is the top annotation at (x, y) within tolerance points, or -1.
func (d *PDF) AnnotationAt(page int, x, y, tolerance float64) (int, error) {
	var message *C.char
	index := int(C.zebkit_pdf_annotation_at(d.p, C.int(page), C.double(x), C.double(y), C.double(tolerance), &message))
	if index < 0 && message != nil {
		return -1, takeError(message, "")
	}
	return index, nil
}

// MoveAnnotation moves annotation index to index to in the page's order: later is on top.
func (d *PDF) MoveAnnotation(page, index, to int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_move(d.p, C.int(page), C.int(index), C.int(to), &message)), message, "not moved")
}

// CopyAnnotation copies annotation index to page targetPage of target
// (which may be d) and returns its index there.
func (d *PDF) CopyAnnotation(page, index int, target *PDF, targetPage int) (int, error) {
	var message *C.char
	copied := int(C.zebkit_pdf_annotation_copy(d.p, C.int(page), C.int(index), target.raw(), C.int(targetPage), &message))
	if copied < 0 {
		return -1, takeError(message, "not copied")
	}
	return copied, nil
}

// FlattenAnnotation makes annotation index's appearance part of the page, and removes it.
func (d *PDF) FlattenAnnotation(page, index int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_flatten(d.p, C.int(page), C.int(index), &message)), message, "not flattened")
}

// StampFromPage makes page sourcePage of source, as vectors, the appearance of stamp index.
func (d *PDF) StampFromPage(page, index int, source *PDF, sourcePage int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_stamp_page(d.p, C.int(page), C.int(index), source.raw(), C.int(sourcePage), &message)),
		message, "not stamped")
}

// AttachFile embeds data, named name, as the file of attachment annotation index.
func (d *PDF) AttachFile(page, index int, name string, data []byte) error {
	cname := cString(name)
	defer free(cname)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_attach_file(d.p, C.int(page), C.int(index), cname, bytesArg(data),
		C.size_t(len(data)), &message)), message, "not attached")
}

// AnnotationFile is the file of attachment annotation index, and its name.
func (d *PDF) AnnotationFile(page, index int) (name string, data []byte, err error) {
	var cname *C.char
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_annotation_file(d.p, C.int(page), C.int(index), &cname, &size, &message)
	if out == nil {
		return "", nil, takeError(message, "no file")
	}
	return takeString(cname), takeBytes(out, size), nil
}

// DrawAnnotation draws ops (Canvas.Run's) as the appearance of annotation
// index: in points, its origin the top left of the annotation's box.
func (d *PDF) DrawAnnotation(page, index int, ops string) error {
	cops := cString(ops)
	defer free(cops)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_draw(d.p, C.int(page), C.int(index), cops, &message)), message, "not drawn")
}

// AnnotationAction is what activating annotation index means, without doing it.
func (d *PDF) AnnotationAction(page, index int) (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_annotation_action(d.p, C.int(page), C.int(index), &message), message, "no action")
}

// PDFInterchange is a file format for annotations.
type PDFInterchange int

const (
	XFDF PDFInterchange = 0
	FDF  PDFInterchange = 1
)

// ExportAnnotations writes the annotations as XFDF or FDF: of page (-1 for
// all), only annotation index unless it is -1.
func (d *PDF) ExportAnnotations(format PDFInterchange, page, index int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_annotations_export(d.p, C.zebkit_pdf_interchange(format), C.int(page), C.int(index), &size, &message)
	if out == nil {
		return nil, takeError(message, "not exported")
	}
	return takeBytes(out, size), nil
}

// ImportAnnotations adds the annotations of an XFDF or FDF file and returns
// how many; all or none.
func (d *PDF) ImportAnnotations(format PDFInterchange, data []byte) (int, error) {
	var message *C.char
	count := int(C.zebkit_pdf_annotations_import(d.p, C.zebkit_pdf_interchange(format), bytesArg(data), C.size_t(len(data)), &message))
	if count < 0 {
		return -1, takeError(message, "not imported")
	}
	return count, nil
}

// Redact takes what is under areas, a JSON array of [l, b, r, t] ("" for
// the page's redaction marks), out of the page for good, draws fill
// (0xRRGGBBAA; 0 for none) over each, and returns the number of areas.
func (d *PDF) Redact(page int, areasJSON string, fill uint32) (int, error) {
	careas := optionalString(areasJSON)
	defer free(careas)
	var message *C.char
	count := int(C.zebkit_pdf_redact(d.p, C.int(page), careas, C.uint32_t(fill), &message))
	if count < 0 {
		return -1, takeError(message, "not redacted")
	}
	return count, nil
}

// FormFields are the document's form fields.
func (d *PDF) FormFields() (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_form_fields(d.p, &message), message, "no form")
}

// SetField sets field name (its full name) to value; the appearances follow.
func (d *PDF) SetField(name, value string) error {
	cname, cvalue := cString(name), cString(value)
	defer free(cname)
	defer free(cvalue)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_form_set(d.p, cname, cvalue, &message)), message, "field refused")
}

// ResetForm sets every field back to its default value.
func (d *PDF) ResetForm() error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_form_reset(d.p, &message)), message, "not reset")
}

// RotatePage sets the page's rotation: 0, 90, 180 or 270.
func (d *PDF) RotatePage(page, rotation int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_page_rotate(d.p, C.int(page), C.int(rotation), &message)), message, "not rotated")
}

func intArray(values []int) (*C.int, C.int) {
	if len(values) == 0 {
		return nil, 0
	}
	ints := make([]C.int, len(values))
	for i, v := range values {
		ints[i] = C.int(v)
	}
	return &ints[0], C.int(len(ints))
}

// DeletePages deletes the pages at these indexes.
func (d *PDF) DeletePages(pages []int) error {
	p, n := intArray(pages)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_pages_delete(d.p, p, n, &message)), message, "not deleted")
}

// MovePages moves pages, in the order given, to start at index to of the
// document they leave.
func (d *PDF) MovePages(pages []int, to int) error {
	p, n := intArray(pages)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_pages_move(d.p, p, n, C.int(to), &message)), message, "not moved")
}

// InsertPage inserts a blank page width by height points at index at.
func (d *PDF) InsertPage(at int, width, height float64) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_page_insert(d.p, C.int(at), C.double(width), C.double(height), &message)), message, "not inserted")
}

// ImportPages copies pages of from (all when pages is nil) to index at.
func (d *PDF) ImportPages(from *PDF, pages []int, at int) error {
	p, n := intArray(pages)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_pages_import(d.p, from.raw(), p, n, C.int(at), &message)), message, "not imported")
}

// Attachments are the files attached to the document: [{"name", "size"}].
func (d *PDF) Attachments() (string, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_attachments(d.p, &message), message, "no attachments")
}

// Attachment is attachment index's bytes.
func (d *PDF) Attachment(index int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_attachment(d.p, C.int(index), &size, &message)
	if out == nil {
		return nil, takeError(message, "no such attachment")
	}
	return takeBytes(out, size), nil
}

// Attach attaches data as name.
func (d *PDF) Attach(name string, data []byte) error {
	cname := cString(name)
	defer free(cname)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_attach(d.p, cname, bytesArg(data), C.size_t(len(data)), &message)), message, "not attached")
}

// Detach removes attachment index.
func (d *PDF) Detach(index int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_detach(d.p, C.int(index), &message)), message, "not detached")
}

// Apply runs an operation an "after" hook saw, or its "inverse": undo and redo.
func (d *PDF) Apply(operation string) error {
	cop := cString(operation)
	defer free(cop)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_apply(d.p, cop, &message)), message, "not applied")
}

// PageData is what Flatten kept of annotations with entries of prefix.
func (d *PDF) PageData(page int, prefix string) (string, error) {
	cprefix := cString(prefix)
	defer free(cprefix)
	var message *C.char
	return jsonOrError(C.zebkit_pdf_page_data(d.p, C.int(page), cprefix, &message), message, "no data")
}

// AddFont gives family ("" for what the rest lack) as this font file, for free text.
func (d *PDF) AddFont(family string, font []byte) error {
	cfamily := cString(family)
	defer free(cfamily)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_font_add(d.p, cfamily, bytesArg(font), C.size_t(len(font)), &message)), message, "font refused")
}

// FilterInput runs a drawing tool's points (JSON [[x, y], ...]) through the
// built-in filters options names ("smooth", "simplify", "snapGrid",
// "snapAngle", "constrain"; "" for none), then the input hooks for tool.
func (d *PDF) FilterInput(tool, points, options string) (string, error) {
	ctool, cpoints := cString(tool), cString(points)
	defer free(ctool)
	defer free(cpoints)
	coptions := optionalString(options)
	defer free(coptions)
	var message *C.char
	return jsonOrError(C.zebkit_pdf_filter_input(d.raw(), ctool, cpoints, coptions, &message), message, "points refused")
}

// FilterInput is PDF.FilterInput for no document.
func FilterInput(tool, points, options string) (string, error) {
	var none *PDF
	return none.FilterInput(tool, points, options)
}

// ---- Hooks ------------------------------------------------------------

// pdfString is text handed to the library, which frees it.
func pdfString(text string) *C.char {
	ctext := cString(text)
	defer free(ctext)
	return C.zebkit_pdf_string(ctext)
}

// PDFHook is a hook of <zebkit/pdf.h>; it goes with Remove or with its document.
type PDFHook int

// Remove removes the hook; false when it is gone already.
func (h PDFHook) Remove() bool { return bool(C.zebkit_pdf_hook_remove(C.int(h))) }

// hookResult turns an id into a PDFHook, letting the function go when the
// hook was not registered.
func hookResult(id C.int, h C.uintptr_t) (PDFHook, error) {
	if id <= 0 {
		cgo.Handle(h).Delete()
		return 0, &Error{"the hook cannot be registered"}
	}
	return PDFHook(id), nil
}

func eventArg(event string) *C.char { return cString(event) }

const (
	decisionContinue = iota
	decisionChange
	decisionRefuse
)

// PDFDecision is what a "before" hook decides.
type PDFDecision struct {
	kind int
	text string
}

// PDFContinue goes on as it is; PDFChange goes on with this operation
// instead, as JSON; PDFRefuse does nothing and fails the call with reason.
func PDFContinue() PDFDecision { return PDFDecision{kind: decisionContinue} }
func PDFChange(operation string) PDFDecision {
	return PDFDecision{kind: decisionChange, text: operation}
}
func PDFRefuse(reason string) PDFDecision { return PDFDecision{kind: decisionRefuse, text: reason} }

// HookBefore runs function before event ("annotation.add", ..., or "*") on
// document doc (nil for all), given the operation as JSON; doc is nil for
// "open". Hooks run on the goroutine that made the call that needs them.
func HookBefore(doc *PDF, event string, function func(doc *PDF, operation string) PDFDecision) (PDFHook, error) {
	cevent := eventArg(event)
	defer free(cevent)
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_before(doc.raw(), cevent, C.zebkit_pdf_before_func(C.zkb_before), data(h),
		(*[0]byte)(C.zkb_destroy)), h)
}

// HookAfter runs function after event, given what happened as JSON, with
// its "inverse" when it can be undone (Apply).
func HookAfter(doc *PDF, event string, function func(doc *PDF, operation string)) (PDFHook, error) {
	cevent := eventArg(event)
	defer free(cevent)
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_after(doc.raw(), cevent, C.zebkit_pdf_after_func(C.zkb_after), data(h),
		(*[0]byte)(C.zkb_destroy)), h)
}

// AppearanceFunc draws an annotation's appearance on a canvas that writes PDF
// operators: in points, its origin the top left of the annotation's box of
// width by height; annotation is the annotation as Annotations has it. False
// leaves it to the generator.
type AppearanceFunc func(canvas *Canvas, annotation string, width, height float64) bool

// HookAppearance draws type kind's appearance ("stamp", or "*") instead of
// the generator.
func HookAppearance(doc *PDF, kind string, function AppearanceFunc) (PDFHook, error) {
	ckind := cString(kind)
	defer free(ckind)
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_appearance(doc.raw(), ckind, C.zebkit_pdf_appearance_func(C.zkb_appearance), data(h),
		(*[0]byte)(C.zkb_destroy)), h)
}

// PDFKind is a kind of annotation of the application's own (HookKind).
// Subtype "" makes /Subtype Name; Schema "" lets "data" hold anything.
// Check returns what is wrong with data, or ""; Draw draws the appearance;
// HitTest says whether (x, y) is on it (nil: its rectangle).
type PDFKind struct {
	Name, Prefix, Subtype, Schema string
	Check                         func(dataJSON string) string
	Draw                          AppearanceFunc
	HitTest                       func(doc *PDF, page, index int, annotation string, x, y float64) bool
}

type pdfKind struct {
	check   func(string) string
	draw    AppearanceFunc
	hitTest func(*PDF, int, int, string, float64, float64) bool
}

// HookKind registers kind.
func HookKind(doc *PDF, kind PDFKind) (PDFHook, error) {
	name, prefix := cString(kind.Name), cString(kind.Prefix)
	defer free(name)
	defer free(prefix)
	subtype, schema := optionalString(kind.Subtype), optionalString(kind.Schema)
	defer free(subtype)
	defer free(schema)
	h := handle(&pdfKind{check: kind.Check, draw: kind.Draw, hitTest: kind.HitTest})
	raw := (*C.zebkit_pdf_kind)(C.calloc(1, C.size_t(unsafe.Sizeof(C.zebkit_pdf_kind{}))))
	defer C.free(unsafe.Pointer(raw))
	raw.name, raw.prefix, raw.subtype, raw.schema = name, prefix, subtype, schema
	if kind.Check != nil {
		raw.check = (*[0]byte)(C.zkb_check)
	}
	if kind.Draw != nil {
		raw.draw = C.zebkit_pdf_appearance_func(C.zkb_appearance)
	}
	if kind.HitTest != nil {
		raw.hit_test = (*[0]byte)(C.zkb_hit)
	}
	raw.data = data(h)
	raw.destroy = (*[0]byte)(C.zkb_destroy)
	var message *C.char
	id := C.zebkit_pdf_hook_kind(doc.raw(), raw, &message)
	if id <= 0 {
		cgo.Handle(h).Delete()
		return 0, takeError(message, "the kind cannot be registered")
	}
	return PDFHook(id), nil
}

// PageData is per kind's prefix: see PDF.PageData.

// PDFLayerInfo says how a layer's canvas maps to pixels: the bitmap's (or
// tile's) size, pixels per point, the options' rotation, and the matrix
// from page space to the bitmap's pixels.
type PDFLayerInfo struct {
	Width, Height int
	Scale         float64
	Rotation      int
	Matrix        [6]float64
}

// HookLayer draws above (or below) a page's content when it is rendered, on
// a canvas in page space: points, y up.
func HookLayer(doc *PDF, above bool, function func(canvas *Canvas, page int, info PDFLayerInfo)) (PDFHook, error) {
	position := C.zebkit_pdf_layer_position(C.ZEBKIT_PDF_LAYER_BELOW)
	if above {
		position = C.ZEBKIT_PDF_LAYER_ABOVE
	}
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_layer(doc.raw(), position, C.zebkit_pdf_layer_func(C.zkb_layer), data(h),
		(*[0]byte)(C.zkb_destroy)), h)
}

// HookOCR gives text to pages with none of their own: given the page
// rendered at scale pixels per point, it returns [{"text", "box"}] in page
// space as JSON, or false for none.
func HookOCR(doc *PDF, function func(page int, image *Pixels, scale float64) (string, bool)) (PDFHook, error) {
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_ocr(doc.raw(), C.zebkit_pdf_ocr_func(C.zkb_ocr), data(h), (*[0]byte)(C.zkb_destroy)), h)
}

// HookFont gives a TrueType or OpenType file for a family free text asks
// for that is not a standard font ("" for what they lack), or nil.
func HookFont(doc *PDF, function func(family string) []byte) (PDFHook, error) {
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_font(doc.raw(), C.zebkit_pdf_font_func(C.zkb_font), data(h), (*[0]byte)(C.zkb_destroy)), h)
}

// PDFTrust is what a trust hook says of a signer.
type PDFTrust int

const (
	PDFUntrusted    PDFTrust = -1
	PDFTrustUnknown PDFTrust = 0
	PDFTrusted      PDFTrust = 1
)

// HookTrust says whether a signer is trusted, given the signature's entry of
// Signatures and its PKCS#7.
func HookTrust(doc *PDF, function func(signature string, contents []byte) PDFTrust) (PDFHook, error) {
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_trust(doc.raw(), C.zebkit_pdf_trust_func(C.zkb_trust), data(h), (*[0]byte)(C.zkb_destroy)), h)
}

// HookAction takes an annotation's action (AnnotationAction's JSON): true
// when the application has done it.
func HookAction(doc *PDF, function func(action string) bool) (PDFHook, error) {
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_action(doc.raw(), C.zebkit_pdf_action_func(C.zkb_action), data(h), (*[0]byte)(C.zkb_destroy)), h)
}

// HookInput changes tool's points (FilterInput): new points as JSON, or
// false for no change.
func HookInput(doc *PDF, tool string, function func(tool, points string) (string, bool)) (PDFHook, error) {
	ctool := cString(tool)
	defer free(ctool)
	h := handle(function)
	return hookResult(C.zebkit_pdf_hook_input(doc.raw(), ctool, C.zebkit_pdf_input_func(C.zkb_input), data(h),
		(*[0]byte)(C.zkb_destroy)), h)
}
