package zebkit

/*
#include "bridge.h"

static zebkit_rect zk_damage_at(const zebkit_frame *frame, size_t i) { return frame->damage[i]; }
static int zk_fd_at(const zebkit_frame *frame, int i) { return frame->fds[i]; }
static uint32_t zk_stride_at(const zebkit_frame *frame, int i) { return frame->strides[i]; }
static uint32_t zk_offset_at(const zebkit_frame *frame, int i) { return frame->offsets[i]; }
*/
import "C"

import (
	"sync"
	"unsafe"
)

// Off-screen views (<zebkit/offscreen.h>): pages drawn into buffers handed
// to the application, with the input it sends.

// Rect is a rectangle, in CSS pixels or a frame's pixels as each use says.
type Rect struct{ X, Y, Width, Height int32 }

func rectFrom(r *C.zebkit_rect) Rect {
	return Rect{int32(r.x), int32(r.y), int32(r.width), int32(r.height)}
}

// NewOffscreenView makes a view drawn off-screen, width by height CSS pixels
// at scale device pixels per CSS pixel (0 for 1); "" loads nothing. Where
// the engine cannot make one, its error says why (a closed context, the
// engine stopping, a process with windows on Windows, Wayland).
func NewOffscreenView(url string, width, height int, scale float32) (*View, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	return newOffscreenView(nil, url, width, height, scale)
}

func newOffscreenView(context *C.zebkit_context, url string, width, height int, scale float32) (*View, error) {
	curl := optionalString(url)
	defer free(curl)
	var message *C.char
	raw := C.zebkit_view_new_offscreen_ex(context, curl, C.int(width), C.int(height), C.float(scale), &message)
	if raw == nil {
		return nil, takeError(message, "no off-screen view")
	}
	return newView(raw), nil
}

// NewOffscreenView is the package's NewOffscreenView in the context (its
// cookies and storage).
func (c *Context) NewOffscreenView(url string, width, height int, scale float32) (*View, error) {
	raw, ok := c.raw()
	if !ok {
		return nil, ErrClosed
	}
	return newOffscreenView(raw, url, width, height, scale)
}

// IsOffscreen says whether the view is drawn off-screen.
func (v *View) IsOffscreen() bool {
	raw, ok := v.raw()
	return ok && bool(C.zebkit_view_is_offscreen(raw))
}

// SetSize changes an off-screen view's size and scale.
func (v *View) SetSize(width, height int, scale float32) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_size(raw, C.int(width), C.int(height), C.float(scale))
	}
}

// FrameLayer is the layer of the view a frame is of.
type FrameLayer int

const (
	LayerView  FrameLayer = 0 // the page
	LayerPopup FrameLayer = 1 // a <select>'s list, a date or colour picker
)

// FrameKind is where a frame's pixels are.
type FrameKind int

const (
	FrameSharedMemory FrameKind = 1 // Pixels here; FD (Handle on Windows) for another process
	FrameIOSurface    FrameKind = 2 // IOSurface (macOS)
	FrameDmabuf       FrameKind = 3 // Planes (Linux)
	FrameD3D11        FrameKind = 4 // Handle to a shared D3D11 texture (Windows)
)

// PixelFormat is a frame's pixel format.
type PixelFormat int

const (
	FormatBGRA8 PixelFormat = 1
	FormatRGBA8 PixelFormat = 2
)

// DmabufPlane is a plane of a dmabuf frame: its fd is the engine's until the
// frame is released (dup it to keep it).
type DmabufPlane struct {
	FD             int
	Stride, Offset uint32
}

// OffscreenFrame is a frame of an off-screen view: the whole picture, and
// what changed since the layer's previous frame. Its buffer is the
// application's to read until Release, which must be called once, by the
// application (nothing releases a frame dropped without it); the engine
// holds back newer frames while the application holds as many as it may.
type OffscreenFrame struct {
	frame    *C.zebkit_frame
	release  sync.Once
	Sequence uint64
	Layer    FrameLayer
	Bounds   Rect // where it goes in the view, in CSS pixels
	Width    int  // its size in pixels
	Height   int
	Scale    float32
	Damage   []Rect // what changed, in the frame's pixels
	DrawnUs  int64  // monotonic microseconds
	ReadyUs  int64
	Kind     FrameKind
	Format   PixelFormat
	BufferID uint64 // the same for every frame in the same buffer

	// FrameSharedMemory: the first pixel's row at Pixels in this process,
	// Stride bytes a row; FD, Size and Offset for mapping it elsewhere (FD is
	// -1 on Windows, where Handle is the file mapping).
	Stride uintptr
	FD     int
	Size   uintptr
	Offset uintptr

	// FrameDmabuf: the planes, Modifier and FourCC (the DRM format).
	Planes   []DmabufPlane
	Modifier uint64
	FourCC   uint32

	// FrameIOSurface: an IOSurfaceRef. FrameD3D11, and shared memory on
	// Windows: a HANDLE. The engine's until Release.
	IOSurface unsafe.Pointer
	Handle    unsafe.Pointer
}

func newOffscreenFrame(f *C.zebkit_frame) *OffscreenFrame {
	frame := &OffscreenFrame{
		frame:     f,
		Sequence:  uint64(f.sequence),
		Layer:     FrameLayer(f.layer),
		Bounds:    rectFrom(&f.bounds),
		Width:     int(f.width),
		Height:    int(f.height),
		Scale:     float32(f.scale),
		DrawnUs:   int64(f.drawn_us),
		ReadyUs:   int64(f.ready_us),
		Kind:      FrameKind(f.kind),
		Format:    PixelFormat(f.format),
		BufferID:  uint64(f.buffer_id),
		Stride:    uintptr(f.stride),
		FD:        int(f.fd),
		Size:      uintptr(f.size),
		Offset:    uintptr(f.offset),
		Modifier:  uint64(f.modifier),
		FourCC:    uint32(f.fourcc),
		IOSurface: f.iosurface,
		Handle:    f.handle,
	}
	for i := C.size_t(0); i < f.damage_count; i++ {
		r := C.zk_damage_at(f, i)
		frame.Damage = append(frame.Damage, rectFrom(&r))
	}
	if frame.Kind == FrameDmabuf {
		for i := C.int(0); i < f.plane_count && i < 4; i++ {
			frame.Planes = append(frame.Planes, DmabufPlane{int(C.zk_fd_at(f, i)), uint32(C.zk_stride_at(f, i)),
				uint32(C.zk_offset_at(f, i))})
		}
	}
	return frame
}

// Pixels are the shared-memory frame's pixels, the engine's memory, valid
// until Release and not after (nil for other kinds): Height rows of Stride
// bytes, 4 bytes a pixel in Format. CopyPixels keeps them.
func (f *OffscreenFrame) Pixels() []byte {
	if f.frame == nil || f.frame.pixels == nil || f.Height <= 0 || f.Width <= 0 {
		return nil
	}
	length := int(f.Stride)*(f.Height-1) + f.Width*4
	return unsafe.Slice((*byte)(unsafe.Pointer(f.frame.pixels)), length)
}

// CopyPixels is a copy of Pixels, the application's to keep.
func (f *OffscreenFrame) CopyPixels() []byte {
	return append([]byte(nil), f.Pixels()...)
}

// IOSurfaceMachPort is a new send right to the frame's IOSurface, for
// another process (macOS); 0 elsewhere. The caller's to send or deallocate.
func (f *OffscreenFrame) IOSurfaceMachPort() uint32 {
	if f.frame == nil {
		return 0
	}
	return uint32(C.zebkit_frame_iosurface_mach_port(f.frame))
}

// Release gives the frame back: its buffer may be drawn into again, and
// Pixels' slice is no longer the application's. Any goroutine while the
// engine runs; once is enough, later calls do nothing. After Shutdown the
// buffers are gone with the engine, and it does nothing.
func (f *OffscreenFrame) Release() {
	f.release.Do(func() {
		if started.Load() {
			C.zebkit_frame_release(f.frame)
		}
		f.frame = nil
	})
}

// OnFrame calls function with each frame of an off-screen view; the
// function, or what it hands the frame to, releases it. Nil stops.
func (v *View) OnFrame(function func(*OffscreenFrame)) {
	var value func(*C.zebkit_frame)
	if function != nil {
		value = func(f *C.zebkit_frame) { function(newOffscreenFrame(f)) }
	}
	v.hook("frame", function, value, unsafe.Pointer(C.zk_frame), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_frame(raw, C.zebkit_frame_func(f), d)
	})
}

// SetFrameRate sets the most frames a second an off-screen view draws.
func (v *View) SetFrameRate(fps float64) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_frame_rate(raw, C.double(fps))
	}
}

// SetMaxFramesInFlight is how many frames the application may hold at once.
func (v *View) SetMaxFramesInFlight(count int) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_max_frames_in_flight(raw, C.int(count))
	}
}

// FrameBuffers is what an off-screen view's frames are made in.
type FrameBuffers int

const (
	BuffersAuto         FrameBuffers = 0 // IOSurfaces on macOS, shared memory elsewhere
	BuffersSharedMemory FrameBuffers = 1
	BuffersGPU          FrameBuffers = 2 // IOSurfaces, dmabufs, D3D11 textures where the GPU makes them
)

// SetFrameBuffers chooses what the view's frames are made in.
func (v *View) SetFrameBuffers(buffers FrameBuffers) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_frame_buffers(raw, C.zebkit_frame_buffers(buffers))
	}
}

// RequestFrame asks for a frame now, changed or not.
func (v *View) RequestFrame() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_request_frame(raw)
	}
}

// Modifiers are the keys and buttons held during an input event.
type Modifiers uint

const (
	ModifierShift        Modifiers = 1 << 0
	ModifierControl      Modifiers = 1 << 1
	ModifierAlt          Modifiers = 1 << 2 // Option on macOS
	ModifierMeta         Modifiers = 1 << 3 // Command on macOS
	ModifierCapsLock     Modifiers = 1 << 4
	ModifierNumLock      Modifiers = 1 << 5
	ModifierLeftButton   Modifiers = 1 << 6
	ModifierMiddleButton Modifiers = 1 << 7
	ModifierRightButton  Modifiers = 1 << 8
	ModifierKeypad       Modifiers = 1 << 9
	ModifierAutoRepeat   Modifiers = 1 << 10
)

// MouseEvent is the kind of a mouse event.
type MouseEvent int

const (
	MouseMove  MouseEvent = 0
	MouseDown  MouseEvent = 1
	MouseUp    MouseEvent = 2
	MouseLeave MouseEvent = 3
)

// MouseButton is the button of a mouse event.
type MouseButton int

const (
	ButtonNone    MouseButton = 0
	ButtonLeft    MouseButton = 1
	ButtonMiddle  MouseButton = 2
	ButtonRight   MouseButton = 3
	ButtonBack    MouseButton = 4
	ButtonForward MouseButton = 5
)

// SendMouse sends a mouse event at (x, y), CSS pixels in the view.
func (v *View) SendMouse(event MouseEvent, x, y float32, button MouseButton, clickCount int, modifiers Modifiers) {
	raw, ok := v.raw()
	if !ok {
		return
	}
	C.zebkit_view_send_mouse(raw, C.zebkit_mouse_type(event), C.float(x), C.float(y),
		C.zebkit_mouse_button(button), C.int(clickCount), C.uint(modifiers))
}

// WheelPhase is where a wheel event is in a trackpad's gesture.
type WheelPhase int

const (
	WheelPhaseNone     WheelPhase = 0 // a mouse's wheel
	WheelPhaseBegan    WheelPhase = 1
	WheelPhaseChanged  WheelPhase = 2
	WheelPhaseEnded    WheelPhase = 3
	WheelPhaseMomentum WheelPhase = 4
)

// SendWheel scrolls by (deltaX, deltaY): pixels when precise, else lines.
func (v *View) SendWheel(x, y, deltaX, deltaY float32, precise bool, phase WheelPhase, modifiers Modifiers) {
	raw, ok := v.raw()
	if !ok {
		return
	}
	C.zebkit_view_send_wheel(raw, C.float(x), C.float(y), C.float(deltaX), C.float(deltaY), C.bool(precise),
		C.zebkit_wheel_phase(phase), C.uint(modifiers))
}

// KeyEventType is the kind of a key event.
type KeyEventType int

const (
	KeyDown KeyEventType = 0 // what keydown sees, without the character
	KeyUp   KeyEventType = 1
	KeyChar KeyEventType = 2 // the character typed, after its KeyDown
)

// KeyEvent is a key to send. KeyCode is the DOM's keyCode (0x41 for A);
// Code and Key are the DOM's ("KeyA", "a"), "" to leave them to the engine;
// Text and UnmodifiedText are a KeyChar's character.
type KeyEvent struct {
	Type                 KeyEventType
	Modifiers            Modifiers
	KeyCode              int
	NativeKeyCode        int
	Code, Key            string
	Text, UnmodifiedText string
}

func (e *KeyEvent) c() (C.zebkit_key_event, func()) {
	event := C.zebkit_key_event{
		_type:           C.zebkit_key_type(e.Type),
		modifiers:       C.uint(e.Modifiers),
		key_code:        C.int(e.KeyCode),
		native_key_code: C.int(e.NativeKeyCode),
		code:            optionalString(e.Code),
		key:             optionalString(e.Key),
		text:            optionalString(e.Text),
		unmodified_text: optionalString(e.UnmodifiedText),
	}
	return event, func() {
		free(event.code)
		free(event.key)
		free(event.text)
		free(event.unmodified_text)
	}
}

// SendKey sends a key event.
func (v *View) SendKey(event KeyEvent) {
	raw, ok := v.raw()
	if !ok {
		return
	}
	e, done := event.c()
	defer done()
	C.zebkit_view_send_key(raw, &e)
}

// EditCommand is an editing command a key runs ("MoveToEndOfLine", ...).
type EditCommand struct{ Name, Value string }

// SendKeyWithCommands sends a key that runs the given editing commands, as
// the system's key bindings would.
func (v *View) SendKeyWithCommands(event KeyEvent, commands []EditCommand) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	e, done := event.c()
	defer done()
	if len(commands) == 0 {
		return check(C.zebkit_view_send_key_with_commands(raw, &e, nil, 0), "the key was refused")
	}
	array := (*C.zebkit_edit_command)(C.malloc(C.size_t(len(commands)) * C.size_t(unsafe.Sizeof(C.zebkit_edit_command{}))))
	defer C.free(unsafe.Pointer(array))
	items := unsafe.Slice(array, len(commands))
	for i, command := range commands {
		items[i].name = cString(command.Name)
		items[i].value = optionalString(command.Value)
	}
	defer func() {
		for i := range items {
			free(items[i].name)
			free(items[i].value)
		}
	}()
	return check(C.zebkit_view_send_key_with_commands(raw, &e, array, C.size_t(len(commands))), "the key was refused")
}

// SetIMEComposition shows text being composed by an input method; the
// selection is in characters of text (-1, -1 for its end).
func (v *View) SetIMEComposition(text string, selectionStart, selectionEnd int) {
	if raw, ok := v.raw(); ok {
		t := cString(text)
		defer free(t)
		C.zebkit_view_ime_set_composition(raw, t, C.int(selectionStart), C.int(selectionEnd))
	}
}

// CommitIME commits text an input method composed.
func (v *View) CommitIME(text string) {
	if raw, ok := v.raw(); ok {
		t := cString(text)
		defer free(t)
		C.zebkit_view_ime_commit(raw, t)
	}
}

// CancelIME cancels the input method's composition.
func (v *View) CancelIME() {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_ime_cancel(raw)
	}
}

// SetFocused gives the off-screen view the keyboard's focus, or takes it
// (Focus is a window's).
func (v *View) SetFocused(focused bool) {
	if raw, ok := v.raw(); ok {
		C.zebkit_view_set_focus(raw, C.bool(focused))
	}
}

// SetAuthenticationUI turns the engine's own passkey UI for this view on or off.
func (v *View) SetAuthenticationUI(enabled bool) error {
	raw, ok := v.raw()
	if !ok {
		return ErrClosed
	}
	return check(C.zebkit_view_set_authentication_ui(raw, C.bool(enabled)), "refused")
}

// OnCaret calls function with the caret's place, CSS pixels in the view, or
// nil when there is none (for an input method's window).
func (v *View) OnCaret(function func(caret *Rect)) {
	var value func(*C.zebkit_rect)
	if function != nil {
		value = func(r *C.zebkit_rect) { function(optionalRect(r)) }
	}
	v.hook("caret", function, value, unsafe.Pointer(C.zk_caret), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_caret(raw, C.zebkit_caret_func(f), d)
	})
}

func optionalRect(r *C.zebkit_rect) *Rect {
	if r == nil {
		return nil
	}
	rect := rectFrom(r)
	return &rect
}

// CursorImage is a custom cursor's picture: BGRA pixels, Width by Height,
// with its hot spot.
type CursorImage struct {
	Pixels     []byte
	Width      int
	Height     int
	HotX, HotY int
	Scale      float32
}

// OnCursor calls function with the cursor the page asks for ("pointer",
// "text", ..., or "custom" with its image).
func (v *View) OnCursor(function func(cursor string, image *CursorImage)) {
	var value func(string, *C.zebkit_cursor_image)
	if function != nil {
		value = func(cursor string, image *C.zebkit_cursor_image) {
			var picture *CursorImage
			if image != nil && image.pixels != nil && image.width > 0 && image.height > 0 {
				picture = &CursorImage{
					Pixels: goBytes(unsafe.Pointer(image.pixels), rgbaSize(image.width, image.height)),
					Width:  int(image.width), Height: int(image.height),
					HotX: int(image.hot_x), HotY: int(image.hot_y), Scale: float32(image.scale),
				}
			}
			function(cursor, picture)
		}
	}
	v.hook("cursor", function, value, unsafe.Pointer(C.zk_cursor), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_cursor(raw, C.zebkit_cursor_func(f), d)
	})
}

// OnPopup calls function with a popup widget's place when it opens, CSS
// pixels in the view, and nil when it closes.
func (v *View) OnPopup(function func(bounds *Rect)) {
	var value func(*C.zebkit_rect)
	if function != nil {
		value = func(r *C.zebkit_rect) { function(optionalRect(r)) }
	}
	v.hook("popup", function, value, unsafe.Pointer(C.zk_popup), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_popup(raw, C.zebkit_popup_func(f), d)
	})
}
