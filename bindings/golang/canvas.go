package zebkit

/*
#include "bridge_b.h"
*/
import "C"

import "unsafe"

// Canvas is 2D drawing on the engine's Skia (<zebkit/canvas.h>), as the web's
// canvas works: a current path put through the matrix as it is made, and a
// stack of state kept by Save and Restore. The origin is the top left, y
// down, units are pixels. A canvas a PDF hook is given writes PDF operators
// instead, in points, and its Pixels and PNG fail. One goroutine at a time.
type Canvas struct {
	c        *C.zebkit_canvas
	borrowed bool
}

func borrowedCanvas(c *C.zebkit_canvas) *Canvas { return &Canvas{c: c, borrowed: true} }

// NewCanvas is a canvas over a transparent bitmap of its own, at most 32768
// on a side and 1 << 28 pixels.
func NewCanvas(width, height int) (*Canvas, error) {
	var message *C.char
	c := C.zebkit_canvas_new(C.int(width), C.int(height), &message)
	if c == nil {
		return nil, takeError(message, "no canvas")
	}
	return &Canvas{c: c}, nil
}

// CanvasFlags say what the pixels OnPixels draws on are.
type CanvasFlags uint

// CanvasUnpremultiplied: the pixels are not premultiplied (opaque pixels
// are premultiplied already).
const CanvasUnpremultiplied CanvasFlags = 1 << 0

// OnPixels runs draw on a canvas over rgba, width by height pixels of R, G,
// B, A, with stride bytes from row to row (0 for width * 4), and writes what
// was drawn back into rgba.
func OnPixels(rgba []byte, width, height, stride int, flags CanvasFlags, draw func(*Canvas)) error {
	if stride == 0 {
		stride = width * 4
	}
	if width <= 0 || height <= 0 || stride < width*4 || len(rgba) < stride*(height-1)+width*4 {
		return &Error{"fewer pixels than the size says"}
	}
	// The engine keeps the pixels until the canvas closes: C's, not Go's.
	pixels := C.CBytes(rgba)
	defer C.free(pixels)
	var message *C.char
	c := C.zebkit_canvas_wrap((*C.uint8_t)(pixels), C.int(width), C.int(height), C.size_t(stride), C.uint(flags), &message)
	if c == nil {
		return takeError(message, "no canvas")
	}
	canvas := &Canvas{c: c}
	draw(canvas)
	canvas.Close()
	copy(rgba, unsafe.Slice((*byte)(pixels), len(rgba)))
	return nil
}

// Close frees the canvas; a canvas a hook was given is not the caller's to close.
func (c *Canvas) Close() {
	if c.c != nil && !c.borrowed {
		C.zebkit_canvas_close(c.c)
	}
	c.c = nil
}

func (c *Canvas) Width() int  { return int(C.zebkit_canvas_width(c.c)) }
func (c *Canvas) Height() int { return int(C.zebkit_canvas_height(c.c)) }

// Pixels is a copy of the pixels, R, G, B, A not premultiplied.
func (c *Canvas) Pixels() (*Pixels, error) {
	var message *C.char
	out := C.zebkit_canvas_pixels(c.c, &message)
	if out == nil {
		return nil, takeError(message, "no pixels")
	}
	width, height := c.Width(), c.Height()
	return &Pixels{Width: width, Height: height, RGBA: takeBytes(out, C.size_t(width*height*4))}, nil
}

// PNG is the pixels as a PNG file.
func (c *Canvas) PNG() ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_canvas_png(c.c, &size, &message)
	if out == nil {
		return nil, takeError(message, "no PNG")
	}
	return takeBytes(out, size), nil
}

func (c *Canvas) Save()                  { C.zebkit_canvas_save(c.c) }
func (c *Canvas) Restore()               { C.zebkit_canvas_restore(c.c) }
func (c *Canvas) Translate(x, y float64) { C.zebkit_canvas_translate(c.c, C.double(x), C.double(y)) }
func (c *Canvas) Scale(x, y float64)     { C.zebkit_canvas_scale(c.c, C.double(x), C.double(y)) }
func (c *Canvas) Rotate(degrees float64) { C.zebkit_canvas_rotate(c.c, C.double(degrees)) }

// Transform multiplies the matrix by [a c e; b d f; 0 0 1].
func (c *Canvas) Transform(a, b, cc, d, e, f float64) {
	C.zebkit_canvas_transform(c.c, C.double(a), C.double(b), C.double(cc), C.double(d), C.double(e), C.double(f))
}

// SetMatrix replaces the matrix.
func (c *Canvas) SetMatrix(a, b, cc, d, e, f float64) {
	C.zebkit_canvas_set_matrix(c.c, C.double(a), C.double(b), C.double(cc), C.double(d), C.double(e), C.double(f))
}

// SetFillColor and SetStrokeColor take 0xRRGGBBAA.
func (c *Canvas) SetFillColor(rgba uint32)   { C.zebkit_canvas_set_fill_color(c.c, C.uint32_t(rgba)) }
func (c *Canvas) SetStrokeColor(rgba uint32) { C.zebkit_canvas_set_stroke_color(c.c, C.uint32_t(rgba)) }

// SetAlpha is 0 to 1, multiplied into everything drawn.
func (c *Canvas) SetAlpha(alpha float64) { C.zebkit_canvas_set_alpha(c.c, C.double(alpha)) }

// SetBlend takes PDF's names: "Normal", "Multiply", "Screen", ...; false for another.
func (c *Canvas) SetBlend(mode string) bool {
	cmode := cString(mode)
	defer free(cmode)
	return bool(C.zebkit_canvas_set_blend(c.c, cmode))
}

func (c *Canvas) SetLineWidth(width float64) { C.zebkit_canvas_set_line_width(c.c, C.double(width)) }

// LineCap and LineJoin are a line's ends and corners.
type LineCap int
type LineJoin int

const (
	CapButt   LineCap  = 0
	CapRound  LineCap  = 1
	CapSquare LineCap  = 2
	JoinMiter LineJoin = 0
	JoinRound LineJoin = 1
	JoinBevel LineJoin = 2
)

func (c *Canvas) SetLineCap(cap LineCap) { C.zebkit_canvas_set_line_cap(c.c, C.zebkit_canvas_cap(cap)) }
func (c *Canvas) SetLineJoin(join LineJoin) {
	C.zebkit_canvas_set_line_join(c.c, C.zebkit_canvas_join(join))
}
func (c *Canvas) SetMiterLimit(limit float64) {
	C.zebkit_canvas_set_miter_limit(c.c, C.double(limit))
}

// SetDash takes lengths on and off in turn; none for a solid line.
func (c *Canvas) SetDash(lengths []float64, phase float64) {
	var p *C.double
	if len(lengths) > 0 {
		p = (*C.double)(unsafe.Pointer(unsafe.SliceData(lengths)))
	}
	C.zebkit_canvas_set_dash(c.c, p, C.int(len(lengths)), C.double(phase))
}

func gradientStops(offsets []float64, colors []uint32) (*C.double, *C.uint32_t, C.int, bool) {
	if len(offsets) < 2 || len(offsets) != len(colors) {
		return nil, nil, 0, false
	}
	return (*C.double)(unsafe.Pointer(unsafe.SliceData(offsets))), (*C.uint32_t)(unsafe.Pointer(unsafe.SliceData(colors))),
		C.int(len(offsets)), true
}

// SetLinearGradient fills from now on with a gradient from (x0, y0) to (x1,
// y1): offsets from 0 to 1, going up, and their colors as 0xRRGGBBAA.
func (c *Canvas) SetLinearGradient(x0, y0, x1, y1 float64, offsets []float64, colors []uint32) bool {
	o, k, n, ok := gradientStops(offsets, colors)
	return ok && bool(C.zebkit_canvas_set_linear_gradient(c.c, C.double(x0), C.double(y0), C.double(x1), C.double(y1), o, k, n))
}

// SetRadialGradient is the same about (x, y), to radius.
func (c *Canvas) SetRadialGradient(x, y, radius float64, offsets []float64, colors []uint32) bool {
	o, k, n, ok := gradientStops(offsets, colors)
	return ok && bool(C.zebkit_canvas_set_radial_gradient(c.c, C.double(x), C.double(y), C.double(radius), o, k, n))
}

func (c *Canvas) BeginPath()          { C.zebkit_canvas_begin_path(c.c) }
func (c *Canvas) MoveTo(x, y float64) { C.zebkit_canvas_move_to(c.c, C.double(x), C.double(y)) }
func (c *Canvas) LineTo(x, y float64) { C.zebkit_canvas_line_to(c.c, C.double(x), C.double(y)) }
func (c *Canvas) QuadTo(cx, cy, x, y float64) {
	C.zebkit_canvas_quad_to(c.c, C.double(cx), C.double(cy), C.double(x), C.double(y))
}
func (c *Canvas) CurveTo(c1x, c1y, c2x, c2y, x, y float64) {
	C.zebkit_canvas_curve_to(c.c, C.double(c1x), C.double(c1y), C.double(c2x), C.double(c2y), C.double(x), C.double(y))
}

// Arc goes from start to end degrees, clockwise on the screen unless counterclockwise.
func (c *Canvas) Arc(x, y, radius, start, end float64, counterclockwise bool) {
	C.zebkit_canvas_arc(c.c, C.double(x), C.double(y), C.double(radius), C.double(start), C.double(end), C.bool(counterclockwise))
}
func (c *Canvas) Rect(x, y, width, height float64) {
	C.zebkit_canvas_rect(c.c, C.double(x), C.double(y), C.double(width), C.double(height))
}
func (c *Canvas) ClosePath()        { C.zebkit_canvas_close_path(c.c) }
func (c *Canvas) Fill(evenOdd bool) { C.zebkit_canvas_fill(c.c, C.bool(evenOdd)) }
func (c *Canvas) Stroke()           { C.zebkit_canvas_stroke(c.c) }
func (c *Canvas) Clip(evenOdd bool) { C.zebkit_canvas_clip(c.c, C.bool(evenOdd)) }
func (c *Canvas) Clear(rgba uint32) { C.zebkit_canvas_clear(c.c, C.uint32_t(rgba)) }

// DrawImage draws RGBA pixels, not premultiplied, over the rectangle (x, y)
// destWidth by destHeight; false for sizes not positive or over 1 << 14.
func (c *Canvas) DrawImage(image *Pixels, x, y, destWidth, destHeight float64) bool {
	if image == nil || len(image.RGBA) < image.Width*image.Height*4 {
		return false
	}
	return bool(C.zebkit_canvas_draw_image(c.c, (*C.uint8_t)(bytesArg(image.RGBA)), C.int(image.Width), C.int(image.Height),
		C.double(x), C.double(y), C.double(destWidth), C.double(destHeight)))
}

// Font is a font for a canvas, from a TrueType or OpenType file.
type Font struct{ f *C.zebkit_canvas_font }

// NewFont makes a font from its bytes, which are copied.
func NewFont(data []byte) (*Font, error) {
	var message *C.char
	f := C.zebkit_canvas_font_new(bytesArg(data), C.size_t(len(data)), &message)
	if f == nil {
		return nil, takeError(message, "not a font")
	}
	return &Font{f: f}, nil
}

// Close frees the font; canvases keep what they use.
func (f *Font) Close() {
	if f.f != nil {
		C.zebkit_canvas_font_close(f.f)
		f.f = nil
	}
}

func (c *Canvas) SetFont(font *Font, size float64) {
	C.zebkit_canvas_set_font(c.c, font.f, C.double(size))
}

// FillText shapes text and fills its outlines with the baseline's left end at (x, y).
func (c *Canvas) FillText(text string, x, y float64) error {
	ctext := cString(text)
	defer free(ctext)
	var message *C.char
	if !C.zebkit_canvas_fill_text(c.c, ctext, C.double(x), C.double(y), &message) {
		return takeError(message, "no text drawn")
	}
	return nil
}

// TextPath adds text's outlines to the current path, to stroke or clip.
func (c *Canvas) TextPath(text string, x, y float64) error {
	ctext := cString(text)
	defer free(ctext)
	var message *C.char
	if !C.zebkit_canvas_text_path(c.c, ctext, C.double(x), C.double(y), &message) {
		return takeError(message, "no text path")
	}
	return nil
}

// MeasureText is text's advance in the canvas's font; false without a font.
func (c *Canvas) MeasureText(text string) (float64, bool) {
	ctext := cString(text)
	defer free(ctext)
	advance := float64(C.zebkit_canvas_measure_text(c.c, ctext))
	return advance, advance >= 0
}

// Run runs ops, a JSON array of operations (<zebkit/canvas.h>), stopping at
// the first that is wrong.
func (c *Canvas) Run(ops string) error {
	cops := cString(ops)
	defer free(cops)
	var message *C.char
	if !C.zebkit_canvas_run(c.c, cops, &message) {
		return takeError(message, "the operations were refused")
	}
	return nil
}
