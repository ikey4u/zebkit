package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"unsafe"
)

// PDF is a document of the engine's PDFium (<zebkit/pdf.h>): read, rendered,
// searched, annotated, filled in, changed and saved. It needs no engine
// running; any goroutine may use it, one at a time. Coordinates are PDF page
// space: points, the origin at the bottom left, y up; a rectangle is [left,
// bottom, right, top]; pages count from 0. Structured answers are JSON, as
// <zebkit/pdf.h> describes them; JSON arguments follow the package's rule.
type PDF struct {
	p        *C.zebkit_pdf
	borrowed bool
}

// borrowedPDF is the document a hook is given: not the hook's to close,
// and the hook's only while it runs.
func borrowedPDF(p *C.zebkit_pdf) *PDF {
	if p == nil {
		return nil
	}
	return &PDF{p: p, borrowed: true}
}

// expire ends a hook's loan of the document: it does nothing after.
func (d *PDF) expire() {
	if d != nil {
		d.p = nil
	}
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
	if d == nil {
		return
	}
	if d.p != nil && !d.borrowed {
		C.zebkit_pdf_close(d.p)
	}
	d.p = nil
}

// jsonOrError returns an engine-allocated JSON answer, or the error.
func jsonOrError(out, message *C.char, otherwise string) (json.RawMessage, error) {
	if out == nil {
		return nil, takeError(message, otherwise)
	}
	return json.RawMessage(takeString(out)), nil
}

// textOrError returns an engine-allocated text answer, or the error.
func textOrError(out, message *C.char, otherwise string) (string, error) {
	if out == nil {
		return "", takeError(message, otherwise)
	}
	return takeString(out), nil
}

// withJSON runs call with value as JSON (the package's rule).
func withJSON[T any](value any, call func(*C.char) T) (T, error) {
	text, err := jsonArg(value)
	if err != nil {
		var zero T
		return zero, err
	}
	defer free(text)
	return call(text), nil
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
	out := C.zebkit_pdf_save(d.raw(), C.uint(flags), &size, &message)
	if out == nil {
		return nil, takeError(message, "not saved")
	}
	return takeBytes(out, size), nil
}

// Info is what the document is: pages, version, encryption, permissions,
// form, metadata.
func (d *PDF) Info() (json.RawMessage, error) {
	return takeRaw(C.zebkit_pdf_info(d.raw()), "no document")
}

// SetMetadata sets the document information from a JSON object of
// Info's "metadata" keys: a string sets one, null removes it.
func (d *PDF) SetMetadata(metadata any) error {
	var message *C.char
	ok, err := withJSON(metadata, func(text *C.char) C.bool { return C.zebkit_pdf_set_metadata(d.raw(), text, &message) })
	if err != nil {
		return err
	}
	return boolOrError(bool(ok), message, "metadata refused")
}

// PageCount is the number of pages, -1 for no document.
func (d *PDF) PageCount() int { return int(C.zebkit_pdf_page_count(d.raw())) }

// Page is a page's size, rotation, label and boxes.
func (d *PDF) Page(page int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_page(d.raw(), C.int(page), &message), message, "no such page")
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
	out := C.zebkit_pdf_render(d.raw(), C.int(page), o, &width, &height, &message)
	if out == nil {
		return nil, takeError(message, "not rendered")
	}
	return &Pixels{Width: int(width), Height: int(height), RGBA: takeBytes(out, rgbaSize(width, height))}, nil
}

// RenderAnnotation renders appearance mode (0 normal, 1 rollover, 2 down) of
// annotation index alone, on a transparent background when options is nil.
func (d *PDF) RenderAnnotation(page, index, mode int, options *PDFRenderOptions) (*Pixels, error) {
	o, done := renderOptions(options)
	defer done()
	var width, height C.int
	var message *C.char
	out := C.zebkit_pdf_render_annotation(d.raw(), C.int(page), C.int(index), C.int(mode), o, &width, &height, &message)
	if out == nil {
		return nil, takeError(message, "not rendered")
	}
	return &Pixels{Width: int(width), Height: int(height), RGBA: takeBytes(out, rgbaSize(width, height))}, nil
}

// Text is the page's text, in reading order.
func (d *PDF) Text(page int) (string, error) {
	var message *C.char
	return textOrError(C.zebkit_pdf_text(d.raw(), C.int(page), &message), message, "no text")
}

// TextLayout is the page's characters with their boxes, fonts and sizes.
func (d *PDF) TextLayout(page int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text_layout(d.raw(), C.int(page), &message), message, "no text")
}

// TextRects are the rectangles covering count characters from start.
func (d *PDF) TextRects(page, start, count int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text_rects(d.raw(), C.int(page), C.int(start), C.int(count), &message), message, "no text")
}

// TextLines is the page's text as lines with their boxes.
func (d *PDF) TextLines(page int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_text_lines(d.raw(), C.int(page), &message), message, "no text")
}

// CharAt is the index of the character at (x, y), within tolerance points,
// or -1.
func (d *PDF) CharAt(page int, x, y, tolerance float64) int {
	return int(C.zebkit_pdf_char_at(d.raw(), C.int(page), C.double(x), C.double(y), C.double(tolerance)))
}

// PDFSearchFlags say how Search matches.
type PDFSearchFlags uint

const (
	PDFSearchCase      PDFSearchFlags = 1 << 0
	PDFSearchWholeWord PDFSearchFlags = 1 << 1
)

// Search finds query from page first on, at most limit matches (0 for all):
// [{"page", "start", "count", "rects", "context"}].
func (d *PDF) Search(query string, flags PDFSearchFlags, first, limit int) (json.RawMessage, error) {
	cquery := cString(query)
	defer free(cquery)
	var message *C.char
	return jsonOrError(C.zebkit_pdf_search(d.raw(), cquery, C.uint(flags), C.int(first), C.int(limit), &message), message, "not searched")
}

// Images are the images the page draws: [{"width", "height", "rect", "jpeg"}].
func (d *PDF) Images(page int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_images(d.raw(), C.int(page), &message), message, "no images")
}

// ImageData is image index of the page: a JPEG file as stored (jpeg true),
// or its pixels as PNG.
func (d *PDF) ImageData(page, image int) (data []byte, jpeg bool, err error) {
	var size C.size_t
	var isJPEG C.bool
	var message *C.char
	out := C.zebkit_pdf_image_data(d.raw(), C.int(page), C.int(image), &size, &isJPEG, &message)
	if out == nil {
		return nil, false, takeError(message, "no such image")
	}
	return takeBytes(out, size), bool(isJPEG), nil
}

// Signatures are the document's digital signatures, each checked against
// the document as it was opened.
func (d *PDF) Signatures() (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_signatures(d.raw(), &message), message, "no signatures")
}

// SignatureContents is signature index's DER-encoded PKCS#7 or time stamp.
func (d *PDF) SignatureContents(index int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_signature_contents(d.raw(), C.int(index), &size, &message)
	if out == nil {
		return nil, takeError(message, "no such signature")
	}
	return takeBytes(out, size), nil
}

// Outline is the bookmarks: [{"title", "page", "x", "y", "zoom", "url",
// "open", "children"}].
func (d *PDF) Outline() (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_outline(d.raw(), &message), message, "no outline")
}

// SetOutline replaces the outline with an array in Outline's format, JSON;
// an empty array removes it.
func (d *PDF) SetOutline(outline any) error {
	var message *C.char
	ok, err := withJSON(outline, func(text *C.char) C.bool { return C.zebkit_pdf_set_outline(d.raw(), text, &message) })
	if err != nil {
		return err
	}
	return boolOrError(bool(ok), message, "outline refused")
}

// Links are the page's link annotations and the addresses in its text.
func (d *PDF) Links(page int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_links(d.raw(), C.int(page), &message), message, "no links")
}

// Annotations are the page's annotations, in their order on the page.
func (d *PDF) Annotations(page int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_annotations(d.raw(), C.int(page), &message), message, "no annotations")
}

// AddAnnotation adds an annotation from a JSON object ("type" and "rect" at
// least) and returns its index.
func (d *PDF) AddAnnotation(page int, annotation any) (int, error) {
	var message *C.char
	index, err := withJSON(annotation, func(text *C.char) int {
		return int(C.zebkit_pdf_annotation_add(d.raw(), C.int(page), text, &message))
	})
	if err != nil {
		return -1, err
	}
	if index < 0 {
		return -1, takeError(message, "annotation refused")
	}
	return index, nil
}

// UpdateAnnotation sets the keys changes (a JSON object) has on annotation
// index; null removes one.
func (d *PDF) UpdateAnnotation(page, index int, changes any) error {
	var message *C.char
	ok, err := withJSON(changes, func(text *C.char) C.bool {
		return C.zebkit_pdf_annotation_update(d.raw(), C.int(page), C.int(index), text, &message)
	})
	if err != nil {
		return err
	}
	return boolOrError(bool(ok), message, "annotation refused")
}

// RemoveAnnotation removes annotation index, with its popup and replies.
func (d *PDF) RemoveAnnotation(page, index int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_remove(d.raw(), C.int(page), C.int(index), &message)), message, "not removed")
}

// SetAnnotationImage makes image the appearance of stamp annotation index.
func (d *PDF) SetAnnotationImage(page, index int, image *Pixels) error {
	if !image.complete() {
		return refused("fewer pixels than the size says")
	}
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_set_image(d.raw(), C.int(page), C.int(index), (*C.uint8_t)(bytesArg(image.RGBA)),
		C.int(image.Width), C.int(image.Height), &message)), message, "image refused")
}

// Flatten makes the page's annotations and form fields part of its
// content, as shown or (forPrint) as printed.
func (d *PDF) Flatten(page int, forPrint bool) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_flatten(d.raw(), C.int(page), C.bool(forPrint), &message)), message, "not flattened")
}

// AnnotationAt is the top annotation at (x, y) within tolerance points, or -1.
func (d *PDF) AnnotationAt(page int, x, y, tolerance float64) (int, error) {
	var message *C.char
	index := int(C.zebkit_pdf_annotation_at(d.raw(), C.int(page), C.double(x), C.double(y), C.double(tolerance), &message))
	if index < 0 && message != nil {
		return -1, takeError(message, "")
	}
	return index, nil
}

// MoveAnnotation moves annotation index to index to in the page's order: later is on top.
func (d *PDF) MoveAnnotation(page, index, to int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_move(d.raw(), C.int(page), C.int(index), C.int(to), &message)), message, "not moved")
}

// CopyAnnotation copies annotation index to page targetPage of target
// (which may be d) and returns its index there.
func (d *PDF) CopyAnnotation(page, index int, target *PDF, targetPage int) (int, error) {
	var message *C.char
	copied := int(C.zebkit_pdf_annotation_copy(d.raw(), C.int(page), C.int(index), target.raw(), C.int(targetPage), &message))
	if copied < 0 {
		return -1, takeError(message, "not copied")
	}
	return copied, nil
}

// FlattenAnnotation makes annotation index's appearance part of the page, and removes it.
func (d *PDF) FlattenAnnotation(page, index int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_flatten(d.raw(), C.int(page), C.int(index), &message)), message, "not flattened")
}

// StampFromPage makes page sourcePage of source, as vectors, the appearance of stamp index.
func (d *PDF) StampFromPage(page, index int, source *PDF, sourcePage int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_stamp_page(d.raw(), C.int(page), C.int(index), source.raw(), C.int(sourcePage), &message)),
		message, "not stamped")
}

// AttachFile embeds data, named name, as the file of attachment annotation index.
func (d *PDF) AttachFile(page, index int, name string, data []byte) error {
	cname := cString(name)
	defer free(cname)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_annotation_attach_file(d.raw(), C.int(page), C.int(index), cname, bytesArg(data),
		C.size_t(len(data)), &message)), message, "not attached")
}

// AnnotationFile is the file of attachment annotation index, and its name.
func (d *PDF) AnnotationFile(page, index int) (name string, data []byte, err error) {
	var cname *C.char
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_annotation_file(d.raw(), C.int(page), C.int(index), &cname, &size, &message)
	if out == nil {
		return "", nil, takeError(message, "no file")
	}
	return takeString(cname), takeBytes(out, size), nil
}

// DrawAnnotation draws ops (Canvas.Run's, JSON) as the appearance of
// annotation index: in points, its origin the top left of the annotation's
// box.
func (d *PDF) DrawAnnotation(page, index int, ops any) error {
	var message *C.char
	ok, err := withJSON(ops, func(text *C.char) C.bool {
		return C.zebkit_pdf_annotation_draw(d.raw(), C.int(page), C.int(index), text, &message)
	})
	if err != nil {
		return err
	}
	return boolOrError(bool(ok), message, "not drawn")
}

// AnnotationAction is what activating annotation index means, without doing it.
func (d *PDF) AnnotationAction(page, index int) (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_annotation_action(d.raw(), C.int(page), C.int(index), &message), message, "no action")
}

// PDFInterchange is a file format for annotations.
type PDFInterchange int

const (
	PDFFormatXFDF PDFInterchange = 0
	PDFFormatFDF  PDFInterchange = 1
)

// ExportAnnotations writes the annotations as XFDF or FDF: of page (-1 for
// all), only annotation index unless it is -1.
func (d *PDF) ExportAnnotations(format PDFInterchange, page, index int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_annotations_export(d.raw(), C.zebkit_pdf_interchange(format), C.int(page), C.int(index), &size, &message)
	if out == nil {
		return nil, takeError(message, "not exported")
	}
	return takeBytes(out, size), nil
}

// ImportAnnotations adds the annotations of an XFDF or FDF file and returns
// how many; all or none.
func (d *PDF) ImportAnnotations(format PDFInterchange, data []byte) (int, error) {
	var message *C.char
	count := int(C.zebkit_pdf_annotations_import(d.raw(), C.zebkit_pdf_interchange(format), bytesArg(data), C.size_t(len(data)), &message))
	if count < 0 {
		return -1, takeError(message, "not imported")
	}
	return count, nil
}

// Redact takes what is under areas, a JSON array of [l, b, r, t] (nil for
// the page's redaction marks), out of the page for good, draws fill
// (0xRRGGBBAA; 0 for none) over each, and returns the number of areas.
func (d *PDF) Redact(page int, areas any, fill uint32) (int, error) {
	careas, err := optionalJSON(areas)
	if err != nil {
		return -1, err
	}
	defer free(careas)
	var message *C.char
	count := int(C.zebkit_pdf_redact(d.raw(), C.int(page), careas, C.uint32_t(fill), &message))
	if count < 0 {
		return -1, takeError(message, "not redacted")
	}
	return count, nil
}

// FormFields are the document's form fields.
func (d *PDF) FormFields() (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_form_fields(d.raw(), &message), message, "no form")
}

// SetField sets field name (its full name) to value; the appearances follow.
func (d *PDF) SetField(name, value string) error {
	cname, cvalue := cString(name), cString(value)
	defer free(cname)
	defer free(cvalue)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_form_set(d.raw(), cname, cvalue, &message)), message, "field refused")
}

// ResetForm sets every field back to its default value.
func (d *PDF) ResetForm() error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_form_reset(d.raw(), &message)), message, "not reset")
}

// RotatePage sets the page's rotation: 0, 90, 180 or 270.
func (d *PDF) RotatePage(page, rotation int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_page_rotate(d.raw(), C.int(page), C.int(rotation), &message)), message, "not rotated")
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
	return boolOrError(bool(C.zebkit_pdf_pages_delete(d.raw(), p, n, &message)), message, "not deleted")
}

// MovePages moves pages, in the order given, to start at index to of the
// document they leave.
func (d *PDF) MovePages(pages []int, to int) error {
	p, n := intArray(pages)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_pages_move(d.raw(), p, n, C.int(to), &message)), message, "not moved")
}

// InsertPage inserts a blank page width by height points at index at.
func (d *PDF) InsertPage(at int, width, height float64) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_page_insert(d.raw(), C.int(at), C.double(width), C.double(height), &message)), message, "not inserted")
}

// ImportPages copies pages of from (all when pages is nil) to index at.
func (d *PDF) ImportPages(from *PDF, pages []int, at int) error {
	p, n := intArray(pages)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_pages_import(d.raw(), from.raw(), p, n, C.int(at), &message)), message, "not imported")
}

// Attachments are the files attached to the document: [{"name", "size"}].
func (d *PDF) Attachments() (json.RawMessage, error) {
	var message *C.char
	return jsonOrError(C.zebkit_pdf_attachments(d.raw(), &message), message, "no attachments")
}

// Attachment is attachment index's bytes.
func (d *PDF) Attachment(index int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_pdf_attachment(d.raw(), C.int(index), &size, &message)
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
	return boolOrError(bool(C.zebkit_pdf_attach(d.raw(), cname, bytesArg(data), C.size_t(len(data)), &message)), message, "not attached")
}

// Detach removes attachment index.
func (d *PDF) Detach(index int) error {
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_detach(d.raw(), C.int(index), &message)), message, "not detached")
}

// Apply runs an operation an "after" hook saw, or its "inverse": undo and redo.
func (d *PDF) Apply(operation string) error {
	cop := cString(operation)
	defer free(cop)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_apply(d.raw(), cop, &message)), message, "not applied")
}

// PageData is what Flatten kept of annotations with entries of prefix.
func (d *PDF) PageData(page int, prefix string) (json.RawMessage, error) {
	cprefix := cString(prefix)
	defer free(cprefix)
	var message *C.char
	return jsonOrError(C.zebkit_pdf_page_data(d.raw(), C.int(page), cprefix, &message), message, "no data")
}

// AddFont gives family ("" for what the rest lack) as this font file, for free text.
func (d *PDF) AddFont(family string, font []byte) error {
	cfamily := cString(family)
	defer free(cfamily)
	var message *C.char
	return boolOrError(bool(C.zebkit_pdf_font_add(d.raw(), cfamily, bytesArg(font), C.size_t(len(font)), &message)), message, "font refused")
}

// FilterInput runs a drawing tool's points (JSON [[x, y], ...]) through the
// built-in filters options names, JSON ("smooth", "simplify", "snapGrid",
// "snapAngle", "constrain"; nil for none), then the input hooks for tool.
func (d *PDF) FilterInput(tool string, points, options any) (json.RawMessage, error) {
	cpoints, err := jsonArg(points)
	if err != nil {
		return nil, err
	}
	defer free(cpoints)
	coptions, err := optionalJSON(options)
	if err != nil {
		return nil, err
	}
	defer free(coptions)
	ctool := cString(tool)
	defer free(ctool)
	var message *C.char
	return jsonOrError(C.zebkit_pdf_filter_input(d.raw(), ctool, cpoints, coptions, &message), message, "points refused")
}

// FilterInput is PDF.FilterInput for no document.
func FilterInput(tool string, points, options any) (json.RawMessage, error) {
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

// pdfJSON is value as JSON handed to the library, or NULL when it is not.
func pdfJSON(value any) *C.char {
	text, err := jsonArg(value)
	if err != nil {
		return nil
	}
	defer free(text)
	return C.zebkit_pdf_string(text)
}

// PDFHook is a hook of <zebkit/pdf.h>; it goes with Remove or with its document.
type PDFHook int

// Remove removes the hook; an error when it is gone already.
func (h PDFHook) Remove() error { return check(C.zebkit_pdf_hook_remove(C.int(h)), "no such hook") }

// hookResult turns an id into a PDFHook, letting the function go when the
// hook was not registered.
func hookResult(id C.int, h C.uintptr_t) (PDFHook, error) {
	if id <= 0 {
		release(h)
		return 0, refused("the hook cannot be registered")
	}
	return PDFHook(id), nil
}

// pdfHandle names a hook's function for the engine, which lets it go
// (zk_release) with the hook.
func pdfHandle(function any) (C.uintptr_t, error) {
	h := handle(function)
	if h == 0 {
		return 0, refused("a hook needs a function")
	}
	return h, nil
}

const (
	decisionContinue = iota
	decisionChange
	decisionRefuse
)

// PDFDecision is what a "before" hook decides.
type PDFDecision struct {
	kind   int
	change any
	reason string
}

// PDFContinue goes on as it is.
func PDFContinue() PDFDecision { return PDFDecision{kind: decisionContinue} }

// PDFChange goes on with this operation instead, JSON.
func PDFChange(operation any) PDFDecision {
	return PDFDecision{kind: decisionChange, change: operation}
}

// PDFRefuse does nothing and fails the call with reason.
func PDFRefuse(reason string) PDFDecision { return PDFDecision{kind: decisionRefuse, reason: reason} }

// OnPDFBefore runs function before event ("annotation.add", ..., or "*") on
// document doc (nil for all), given the operation as JSON; doc is nil for
// "open". Hooks run on the goroutine that made the call that needs them.
func OnPDFBefore(doc *PDF, event string, function func(doc *PDF, operation json.RawMessage) PDFDecision) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	cevent := cString(event)
	defer free(cevent)
	return hookResult(C.zebkit_pdf_hook_before(doc.raw(), cevent, C.zebkit_pdf_before_func(C.zk_before), data(h),
		(*[0]byte)(C.zk_release)), h)
}

// OnPDFAfter runs function after event, given what happened as JSON, with
// its "inverse" when it can be undone (Apply).
func OnPDFAfter(doc *PDF, event string, function func(doc *PDF, operation json.RawMessage)) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	cevent := cString(event)
	defer free(cevent)
	return hookResult(C.zebkit_pdf_hook_after(doc.raw(), cevent, C.zebkit_pdf_after_func(C.zk_after), data(h),
		(*[0]byte)(C.zk_release)), h)
}

// AppearanceFunc draws an annotation's appearance on a canvas that writes PDF
// operators: in points, its origin the top left of the annotation's box of
// width by height; annotation is the annotation as Annotations has it. False
// leaves it to the generator.
type AppearanceFunc func(canvas *Canvas, annotation json.RawMessage, width, height float64) bool

// OnPDFAppearance draws type kind's appearance ("stamp", or "*") instead of
// the generator.
func OnPDFAppearance(doc *PDF, kind string, function AppearanceFunc) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	ckind := cString(kind)
	defer free(ckind)
	return hookResult(C.zebkit_pdf_hook_appearance(doc.raw(), ckind, C.zebkit_pdf_appearance_func(C.zk_appearance), data(h),
		(*[0]byte)(C.zk_release)), h)
}

// PDFKind is a kind of annotation of the application's own (RegisterPDFKind).
// Subtype "" makes /Subtype Name; Schema "" lets "data" hold anything.
// Check returns what is wrong with data, or ""; Draw draws the appearance;
// HitTest says whether (x, y) is on it (nil: its rectangle).
type PDFKind struct {
	Name, Prefix, Subtype, Schema string
	Check                         func(data json.RawMessage) string
	Draw                          AppearanceFunc
	HitTest                       func(doc *PDF, page, index int, annotation json.RawMessage, x, y float64) bool
}

type pdfKind struct {
	check   func(json.RawMessage) string
	draw    AppearanceFunc
	hitTest func(*PDF, int, int, json.RawMessage, float64, float64) bool
}

// RegisterPDFKind registers kind, for doc (nil for all documents).
func RegisterPDFKind(doc *PDF, kind PDFKind) (PDFHook, error) {
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
		raw.check = (*[0]byte)(C.zk_check)
	}
	if kind.Draw != nil {
		raw.draw = C.zebkit_pdf_appearance_func(C.zk_appearance)
	}
	if kind.HitTest != nil {
		raw.hit_test = (*[0]byte)(C.zk_hit)
	}
	raw.data = data(h)
	raw.destroy = (*[0]byte)(C.zk_release)
	var message *C.char
	id := C.zebkit_pdf_hook_kind(doc.raw(), raw, &message)
	if id <= 0 {
		release(h)
		return 0, takeError(message, "the kind cannot be registered")
	}
	return PDFHook(id), nil
}

// PDFLayerInfo says how a layer's canvas maps to pixels: the bitmap's (or
// tile's) size, pixels per point, the options' rotation, and the matrix
// from page space to the bitmap's pixels.
type PDFLayerInfo struct {
	Width, Height int
	Scale         float64
	Rotation      int
	Matrix        [6]float64
}

// OnPDFLayer draws above (or below) a page's content when it is rendered, on
// a canvas in page space: points, y up.
func OnPDFLayer(doc *PDF, above bool, function func(canvas *Canvas, page int, info PDFLayerInfo)) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	position := C.zebkit_pdf_layer_position(C.ZEBKIT_PDF_LAYER_BELOW)
	if above {
		position = C.ZEBKIT_PDF_LAYER_ABOVE
	}
	return hookResult(C.zebkit_pdf_hook_layer(doc.raw(), position, C.zebkit_pdf_layer_func(C.zk_layer), data(h),
		(*[0]byte)(C.zk_release)), h)
}

// OnPDFOCR gives text to pages with none of their own: given the page
// rendered at scale pixels per point, it returns [{"text", "box"}] in page
// space, JSON, or false for none.
func OnPDFOCR(doc *PDF, function func(page int, image *Pixels, scale float64) (any, bool)) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	return hookResult(C.zebkit_pdf_hook_ocr(doc.raw(), C.zebkit_pdf_ocr_func(C.zk_ocr), data(h), (*[0]byte)(C.zk_release)), h)
}

// OnPDFFont gives a TrueType or OpenType file for a family free text asks
// for that is not a standard font ("" for what they lack), or nil.
func OnPDFFont(doc *PDF, function func(family string) []byte) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	return hookResult(C.zebkit_pdf_hook_font(doc.raw(), C.zebkit_pdf_font_func(C.zk_font), data(h), (*[0]byte)(C.zk_release)), h)
}

// PDFTrust is what a trust hook says of a signer.
type PDFTrust int

const (
	PDFUntrusted    PDFTrust = -1
	PDFTrustUnknown PDFTrust = 0
	PDFTrusted      PDFTrust = 1
)

// OnPDFTrust says whether a signer is trusted, given the signature's entry of
// Signatures and its PKCS#7.
func OnPDFTrust(doc *PDF, function func(signature json.RawMessage, contents []byte) PDFTrust) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	return hookResult(C.zebkit_pdf_hook_trust(doc.raw(), C.zebkit_pdf_trust_func(C.zk_trust), data(h), (*[0]byte)(C.zk_release)), h)
}

// OnPDFAction takes an annotation's action (AnnotationAction's JSON): true
// when the application has done it.
func OnPDFAction(doc *PDF, function func(action json.RawMessage) bool) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	return hookResult(C.zebkit_pdf_hook_action(doc.raw(), C.zebkit_pdf_action_func(C.zk_action), data(h), (*[0]byte)(C.zk_release)), h)
}

// OnPDFInput changes tool's points (FilterInput): new points, JSON, or false
// for no change.
func OnPDFInput(doc *PDF, tool string, function func(tool string, points json.RawMessage) (any, bool)) (PDFHook, error) {
	h, err := pdfHandle(function)
	if err != nil {
		return 0, err
	}
	ctool := cString(tool)
	defer free(ctool)
	return hookResult(C.zebkit_pdf_hook_input(doc.raw(), ctool, C.zebkit_pdf_input_func(C.zk_input), data(h),
		(*[0]byte)(C.zk_release)), h)
}
