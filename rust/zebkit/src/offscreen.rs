//! Off-screen views: a page drawn without a window, its frames handed to the
//! application as shared surfaces, its input sent by the application
//! (`<zebkit/offscreen.h>`).
//!
//! ```no_run
//! use zebkit::offscreen::{FrameBuffer, Modifiers, MouseButton, MouseEvent};
//! use zebkit::View;
//!
//! // Started with --headless: a process without windows.
//! std::process::exit(zebkit::run(|| {
//!     let view = View::new_offscreen(Some("https://example.com/"), 800, 600, 2.0).unwrap();
//!     view.on_frame(|frame| {
//!         // The whole picture, and what changed since the last frame.
//!         if let FrameBuffer::SharedMemory { pixels, stride, .. } = frame.buffer() {
//!             let _ = (pixels, stride, frame.damage());
//!         }
//!         // Dropping the frame gives its buffer back.
//!     });
//!     view.send_mouse(MouseEvent::Down, 10.0, 10.0, MouseButton::Left, 1, Modifiers::NONE);
//! }));
//! ```
//!
//! Off-screen views are for a process without windows: `--headless`, or
//! Linux without a display. Frames come on the engine's thread; a [`Frame`]
//! may go to any thread, and gives its buffer back when it is dropped.

use crate::view::{call_slot, Slot};
use crate::{c_string, string_from, sys, View};
use std::ops::BitOr;
use std::os::raw::{c_char, c_int, c_void};
use std::rc::Rc;

/// A rectangle, in CSS pixels or in a frame's pixels as each use says.
pub type Rect = sys::zebkit_rect;

/// Which layer of the view a frame is of.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Layer {
    /// The page.
    View,
    /// A popup widget over it: a `<select>`'s list, a date or colour picker.
    Popup,
}

/// Where a frame's pixels are.
#[derive(Debug)]
pub enum FrameBuffer<'a> {
    /// BGRA pixels in shared memory: here, and through `fd` for another
    /// process (the pixels at `offset` of `size` bytes mapped from 0).
    SharedMemory {
        pixels: &'a [u8],
        stride: usize,
        fd: i32,
        offset: usize,
        size: usize,
    },
    /// An `IOSurfaceRef` (macOS), the engine's while the frame lives.
    IoSurface(*mut c_void),
    /// A dmabuf's planes (Linux).
    Dmabuf {
        fds: Vec<i32>,
        strides: Vec<u32>,
        offsets: Vec<u32>,
        modifier: u64,
        fourcc: u32,
    },
}

/// A frame of an off-screen view: the whole picture, and what changed since
/// the layer's previous frame. Its buffer is the application's to read until
/// the frame is dropped, which gives it back; the engine holds back newer
/// frames while the application holds as many as it may.
pub struct Frame {
    raw: *const sys::zebkit_frame,
}

// The engine takes a frame back from any thread.
unsafe impl Send for Frame {}

impl Frame {
    fn get(&self) -> &sys::zebkit_frame {
        unsafe { &*self.raw }
    }

    /// 1, 2, 3, ... for each layer.
    pub fn sequence(&self) -> u64 {
        self.get().sequence
    }

    pub fn layer(&self) -> Layer {
        if self.get().layer == sys::ZEBKIT_LAYER_POPUP {
            Layer::Popup
        } else {
            Layer::View
        }
    }

    /// Where it goes in the view, in CSS pixels.
    pub fn bounds(&self) -> Rect {
        self.get().bounds
    }

    /// Its size in pixels.
    pub fn width(&self) -> u32 {
        self.get().width.max(0) as u32
    }

    pub fn height(&self) -> u32 {
        self.get().height.max(0) as u32
    }

    /// Device pixels per CSS pixel.
    pub fn scale(&self) -> f32 {
        self.get().scale
    }

    /// What changed since the previous frame of the layer, in the frame's pixels.
    pub fn damage(&self) -> &[Rect] {
        let frame = self.get();
        if frame.damage.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(frame.damage, frame.damage_count) }
        }
    }

    /// When the page's change was drawn and when the frame was ready, in
    /// microseconds of the monotonic clock.
    pub fn drawn_us(&self) -> i64 {
        self.get().drawn_us
    }

    pub fn ready_us(&self) -> i64 {
        self.get().ready_us
    }

    /// The same for every frame in the same buffer, while it lives.
    pub fn buffer_id(&self) -> u64 {
        self.get().buffer_id
    }

    /// Where the pixels are.
    pub fn buffer(&self) -> FrameBuffer<'_> {
        let frame = self.get();
        match frame.kind {
            sys::ZEBKIT_FRAME_IOSURFACE => FrameBuffer::IoSurface(frame.iosurface),
            sys::ZEBKIT_FRAME_DMABUF => {
                let planes = frame.plane_count.clamp(0, 4) as usize;
                FrameBuffer::Dmabuf {
                    fds: frame.fds[..planes].to_vec(),
                    strides: frame.strides[..planes].to_vec(),
                    offsets: frame.offsets[..planes].to_vec(),
                    modifier: frame.modifier,
                    fourcc: frame.fourcc,
                }
            }
            _ => {
                let length = if frame.height > 0 {
                    frame.stride * (frame.height as usize - 1) + frame.width.max(0) as usize * 4
                } else {
                    0
                };
                let pixels = if frame.pixels.is_null() {
                    &[][..]
                } else {
                    unsafe { std::slice::from_raw_parts(frame.pixels, length) }
                };
                FrameBuffer::SharedMemory {
                    pixels,
                    stride: frame.stride,
                    fd: frame.fd,
                    offset: frame.offset,
                    size: frame.size,
                }
            }
        }
    }

    /// A new send right to the frame's IOSurface, for another process
    /// (macOS): the caller's, to send or deallocate.
    pub fn iosurface_mach_port(&self) -> Option<u32> {
        let port = unsafe { sys::zebkit_frame_iosurface_mach_port(self.raw) };
        (port != 0).then_some(port)
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        unsafe { sys::zebkit_frame_release(self.raw) }
    }
}

impl std::fmt::Debug for Frame {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Frame")
            .field("sequence", &self.sequence())
            .field("layer", &self.layer())
            .field("width", &self.width())
            .field("height", &self.height())
            .field("scale", &self.scale())
            .field("damage", &self.damage())
            .finish()
    }
}

/// What frames are made in, for [`View::set_frame_buffers`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Buffers {
    /// IOSurfaces on macOS, shared memory on Linux.
    Auto,
    SharedMemory,
    /// IOSurfaces; dmabufs on Linux where the GPU makes them.
    Gpu,
}

/// Modifier keys and buttons held. Combine with `|`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Modifiers(pub u32);

impl Modifiers {
    pub const NONE: Modifiers = Modifiers(0);
    pub const SHIFT: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_SHIFT);
    pub const CONTROL: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_CONTROL);
    /// Option on macOS.
    pub const ALT: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_ALT);
    /// Command on macOS.
    pub const META: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_META);
    pub const CAPS_LOCK: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_CAPS_LOCK);
    pub const NUM_LOCK: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_NUM_LOCK);
    pub const LEFT_BUTTON: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_LEFT_BUTTON);
    pub const MIDDLE_BUTTON: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_MIDDLE_BUTTON);
    pub const RIGHT_BUTTON: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_RIGHT_BUTTON);
    pub const KEYPAD: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_KEYPAD);
    pub const AUTO_REPEAT: Modifiers = Modifiers(sys::ZEBKIT_MODIFIER_AUTO_REPEAT);
}

impl BitOr for Modifiers {
    type Output = Modifiers;
    fn bitor(self, other: Modifiers) -> Modifiers {
        Modifiers(self.0 | other.0)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseEvent {
    Move,
    Down,
    Up,
    /// The pointer left the view.
    Leave,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
    None,
    Left,
    Middle,
    Right,
    Back,
    Forward,
}

/// A trackpad gesture's phase, or [`WheelPhase::None`] for a mouse's wheel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WheelPhase {
    None,
    Began,
    Changed,
    Ended,
    /// The scroll going on after the fingers left.
    Momentum,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyEventType {
    /// A key pressed, what `keydown` sees.
    Down,
    Up,
    /// The text a key typed, after its `Down`.
    Char,
}

/// A key, for [`View::send_key`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KeyEvent<'a> {
    pub kind: KeyEventType,
    pub modifiers: Modifiers,
    /// The Windows virtual key code, the DOM's `keyCode`; 0 for none.
    pub key_code: i32,
    /// The system's own code for the key, or 0.
    pub native_key_code: i32,
    /// The DOM's `code` ("KeyA") and `key` ("a").
    pub code: Option<&'a str>,
    pub key: Option<&'a str>,
    /// For [`KeyEventType::Char`]: what was typed, and without the modifiers.
    pub text: Option<&'a str>,
    pub unmodified_text: Option<&'a str>,
}

/// A cursor the page asks for that is an image of its own.
#[derive(Debug, Clone, PartialEq)]
pub struct CursorImage {
    /// BGRA, premultiplied.
    pub pixels: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub hot_x: i32,
    pub hot_y: i32,
    pub scale: f32,
}

type CursorFunction = dyn FnMut(&str, Option<CursorImage>);

/// The callbacks of an off-screen view, kept with the view.
#[derive(Default)]
pub(crate) struct Slots {
    on_frame: Slot<dyn FnMut(Frame)>,
    on_caret: Slot<dyn FnMut(Option<Rect>)>,
    on_cursor: Slot<CursorFunction>,
    on_popup: Slot<dyn FnMut(Option<Rect>)>,
}

impl Slots {
    pub(crate) fn clear(&self) {
        self.on_frame.borrow_mut().take();
        self.on_caret.borrow_mut().take();
        self.on_cursor.borrow_mut().take();
        self.on_popup.borrow_mut().take();
    }
}

fn int(value: u32) -> c_int {
    c_int::try_from(value).unwrap_or(c_int::MAX)
}

impl View {
    /// A view without a window, loading `url`, `width` by `height` CSS
    /// pixels at `scale` device pixels per CSS pixel. `None` when the
    /// process has windows: off-screen views are for one started with
    /// `--headless` (or Linux without a display).
    pub fn new_offscreen(url: Option<&str>, width: u32, height: u32, scale: f32) -> Option<View> {
        crate::engine::require_engine_thread("View::new_offscreen()");
        let url = url.map(c_string);
        let raw = unsafe {
            sys::zebkit_view_new_offscreen(
                url.as_ref().map_or(std::ptr::null(), |url| url.as_ptr()),
                int(width),
                int(height),
                scale,
            )
        };
        (!raw.is_null()).then(|| View::wrap(raw))
    }

    /// Whether the view is off-screen.
    pub fn is_offscreen(&self) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_is_offscreen(raw) })
    }

    /// The view's size in CSS pixels and its scale (0 to keep it).
    pub fn set_size(&self, width: u32, height: u32, scale: f32) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_size(raw, int(width), int(height), scale) }
        }
    }

    fn data(&self) -> *mut c_void {
        Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>()
    }

    /// Calls `function` with each frame, which gives its buffer back when it
    /// is dropped. Frames start with this.
    pub fn on_frame<F: FnMut(Frame) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.offscreen.on_frame.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_frame(raw, Some(on_frame), self.data()) }
    }

    /// Stops the frames.
    pub fn stop_frames(&self) {
        let Some(raw) = self.raw() else { return };
        unsafe { sys::zebkit_view_on_frame(raw, None, std::ptr::null_mut()) }
        self.inner.offscreen.on_frame.borrow_mut().take();
    }

    /// At most `fps` frames a second; 0 for the display's rate.
    pub fn set_frame_rate(&self, fps: f64) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_frame_rate(raw, fps) }
        }
    }

    /// How many frames the application may hold at once, 1 to 8.
    pub fn set_max_frames_in_flight(&self, count: u32) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_max_frames_in_flight(raw, int(count)) }
        }
    }

    pub fn set_frame_buffers(&self, buffers: Buffers) {
        let value = match buffers {
            Buffers::Auto => sys::ZEBKIT_BUFFERS_AUTO,
            Buffers::SharedMemory => sys::ZEBKIT_BUFFERS_SHARED_MEMORY,
            Buffers::Gpu => sys::ZEBKIT_BUFFERS_GPU,
        };
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_frame_buffers(raw, value) }
        }
    }

    /// A frame now, all of it damaged.
    pub fn request_frame(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_request_frame(raw) }
        }
    }

    /// A mouse event at (`x`, `y`) in the view, in CSS pixels.
    pub fn send_mouse(&self, event: MouseEvent, x: f32, y: f32, button: MouseButton, click_count: u32, modifiers: Modifiers) {
        let Some(raw) = self.raw() else { return };
        let event = match event {
            MouseEvent::Move => sys::ZEBKIT_MOUSE_MOVE,
            MouseEvent::Down => sys::ZEBKIT_MOUSE_DOWN,
            MouseEvent::Up => sys::ZEBKIT_MOUSE_UP,
            MouseEvent::Leave => sys::ZEBKIT_MOUSE_LEAVE,
        };
        let button = match button {
            MouseButton::None => sys::ZEBKIT_BUTTON_NONE,
            MouseButton::Left => sys::ZEBKIT_BUTTON_LEFT,
            MouseButton::Middle => sys::ZEBKIT_BUTTON_MIDDLE,
            MouseButton::Right => sys::ZEBKIT_BUTTON_RIGHT,
            MouseButton::Back => sys::ZEBKIT_BUTTON_BACK,
            MouseButton::Forward => sys::ZEBKIT_BUTTON_FORWARD,
        };
        unsafe { sys::zebkit_view_send_mouse(raw, event, x, y, button, int(click_count), modifiers.0) }
    }

    /// A scroll at (`x`, `y`) by (`delta_x`, `delta_y`) CSS pixels, positive
    /// to the right and down; `precise` for a trackpad's deltas.
    #[allow(clippy::too_many_arguments)]
    pub fn send_wheel(&self, x: f32, y: f32, delta_x: f32, delta_y: f32, precise: bool, phase: WheelPhase, modifiers: Modifiers) {
        let Some(raw) = self.raw() else { return };
        let phase = match phase {
            WheelPhase::None => sys::ZEBKIT_WHEEL_PHASE_NONE,
            WheelPhase::Began => sys::ZEBKIT_WHEEL_PHASE_BEGAN,
            WheelPhase::Changed => sys::ZEBKIT_WHEEL_PHASE_CHANGED,
            WheelPhase::Ended => sys::ZEBKIT_WHEEL_PHASE_ENDED,
            WheelPhase::Momentum => sys::ZEBKIT_WHEEL_PHASE_MOMENTUM,
        };
        unsafe { sys::zebkit_view_send_wheel(raw, x, y, delta_x, delta_y, precise, phase, modifiers.0) }
    }

    /// A key, to the focused element or the open popup.
    pub fn send_key(&self, event: &KeyEvent<'_>) {
        let Some(raw) = self.raw() else { return };
        let code = event.code.map(c_string);
        let key = event.key.map(c_string);
        let text = event.text.map(c_string);
        let unmodified = event.unmodified_text.map(c_string);
        let pointer = |value: &Option<std::ffi::CString>| value.as_ref().map_or(std::ptr::null(), |v| v.as_ptr());
        let raw_event = sys::zebkit_key_event {
            type_: match event.kind {
                KeyEventType::Down => sys::ZEBKIT_KEY_DOWN,
                KeyEventType::Up => sys::ZEBKIT_KEY_UP,
                KeyEventType::Char => sys::ZEBKIT_KEY_CHAR,
            },
            modifiers: event.modifiers.0,
            key_code: event.key_code,
            native_key_code: event.native_key_code,
            code: pointer(&code),
            key: pointer(&key),
            text: pointer(&text),
            unmodified_text: pointer(&unmodified),
        };
        unsafe { sys::zebkit_view_send_key(raw, &raw_event) }
    }

    /// An input method's composition, `selection` in UTF-16 code units
    /// (`None` for its end).
    pub fn ime_set_composition(&self, text: &str, selection: Option<(u32, u32)>) {
        let Some(raw) = self.raw() else { return };
        let (start, end) = selection.map_or((-1, -1), |(start, end)| (int(start), int(end)));
        unsafe { sys::zebkit_view_ime_set_composition(raw, c_string(text).as_ptr(), start, end) }
    }

    /// Types `text` in place of the composition.
    pub fn ime_commit(&self, text: &str) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_ime_commit(raw, c_string(text).as_ptr()) }
        }
    }

    /// Takes the composition away.
    pub fn ime_cancel(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_ime_cancel(raw) }
        }
    }

    /// Whether the view has the keyboard, as its window would.
    pub fn set_focus(&self, focused: bool) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_set_focus(raw, focused) }
        }
    }

    /// Allows native macOS Passkey UI for this view. Off-screen views opt in;
    /// their host must maintain focus and the page must have a recent user
    /// activation before a temporary authentication window can be shown.
    /// Returns false when unsupported or after the view has closed.
    pub fn set_authentication_ui(&self, enabled: bool) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_set_authentication_ui(raw, enabled) })
    }

    /// Calls `function` with the caret's place in the view, in CSS pixels,
    /// as it moves; `None` when there is none.
    pub fn on_caret<F: FnMut(Option<Rect>) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.offscreen.on_caret.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_caret(raw, Some(on_caret), self.data()) }
    }

    /// Calls `function` with the cursor the page asks for, as CSS names it
    /// ("default", "pointer", "text", ...), with its image for "custom".
    pub fn on_cursor<F: FnMut(&str, Option<CursorImage>) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.offscreen.on_cursor.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_cursor(raw, Some(on_cursor), self.data()) }
    }

    /// Calls `function` with a popup widget's place in the view when it is
    /// shown, and `None` when it is gone. Its frames are [`Layer::Popup`].
    pub fn on_popup<F: FnMut(Option<Rect>) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.offscreen.on_popup.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_popup(raw, Some(on_popup), self.data()) }
    }
}

fn slots<'a>(data: *mut c_void) -> &'a Slots {
    unsafe { &(*data.cast::<crate::view::Inner>()).offscreen }
}

unsafe extern "C" fn on_frame(_view: *mut sys::zebkit_view, frame: *const sys::zebkit_frame, data: *mut c_void) {
    let frame = Frame { raw: frame };
    let slots = slots(data);
    let mut frame = Some(frame);
    call_slot(&slots.on_frame, |function| function(frame.take().expect("one frame")));
    // With no function the frame is dropped: given back.
    drop(frame);
}

unsafe extern "C" fn on_caret(_view: *mut sys::zebkit_view, caret: *const sys::zebkit_rect, data: *mut c_void) {
    let caret = (!caret.is_null()).then(|| *caret);
    call_slot(&slots(data).on_caret, |function| function(caret));
}

unsafe extern "C" fn on_cursor(
    _view: *mut sys::zebkit_view,
    cursor: *const c_char,
    image: *const sys::zebkit_cursor_image,
    data: *mut c_void,
) {
    let name = string_from(cursor).unwrap_or_else(|| "default".to_string());
    let image = (!image.is_null() && !(*image).pixels.is_null()).then(|| {
        let image = &*image;
        let (width, height) = (image.width.max(0) as u32, image.height.max(0) as u32);
        CursorImage {
            pixels: std::slice::from_raw_parts(image.pixels, (width * height * 4) as usize).to_vec(),
            width,
            height,
            hot_x: image.hot_x,
            hot_y: image.hot_y,
            scale: image.scale,
        }
    });
    let mut image = Some(image);
    call_slot(&slots(data).on_cursor, |function| function(&name, image.take().flatten()));
}

unsafe extern "C" fn on_popup(_view: *mut sys::zebkit_view, shown: bool, bounds: *const sys::zebkit_rect, data: *mut c_void) {
    let bounds = (shown && !bounds.is_null()).then(|| *bounds);
    call_slot(&slots(data).on_popup, |function| function(bounds));
}
