//! What a page asks of its window, for the application to answer
//! (`<zebkit/page.h>`): JavaScript dialogs, the page printed to PDF, and
//! drag and drop.
//!
//! ```no_run
//! use zebkit::page::{DialogPolicy, DRAG_COPY};
//! use zebkit::View;
//!
//! std::process::exit(zebkit::run(|| {
//!     let view = View::new(Some("https://example.com/"), 1024, 768);
//!     // alert(), confirm(), prompt() and "leave this page?" wait for an answer.
//!     let answering = view.clone();
//!     view.on_dialog(move |dialog| {
//!         if dialog.held {
//!             answering.respond_dialog(dialog.id, true, None);
//!         }
//!     });
//!     view.set_dialog_policy(DialogPolicy::Hold);
//!     view.print_to_pdf(r#"{"printBackground": true}"#, |pdf| {
//!         let _ = pdf.map(|bytes| std::fs::write("page.pdf", bytes));
//!     });
//!     // A file dropped on the page from outside.
//!     view.drag_enter(r#"{"files": ["/tmp/report.pdf"]}"#, 100.0, 100.0, DRAG_COPY, 0);
//!     view.drop_data(100.0, 100.0, 0, |_operation| {});
//! }));
//! ```

use crate::view::{call_slot, Slot};
use crate::{c_string, string_from, sys, take_string, Error, View};
use std::os::raw::{c_char, c_uint, c_void};
use std::rc::Rc;

/// A drag operation, and a mask of them (`|`).
pub const DRAG_NONE: u32 = sys::ZEBKIT_DRAG_NONE;
pub const DRAG_COPY: u32 = sys::ZEBKIT_DRAG_COPY;
pub const DRAG_LINK: u32 = sys::ZEBKIT_DRAG_LINK;
pub const DRAG_MOVE: u32 = sys::ZEBKIT_DRAG_MOVE;
pub const DRAG_ALL: u32 = sys::ZEBKIT_DRAG_ALL;

/// Which dialog a page opened.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DialogType {
    Alert,
    Confirm,
    Prompt,
    /// "Leave this page?": accepting leaves, dismissing stays.
    BeforeUnload,
    /// `window.print()`, which does not wait: answering only closes it.
    Print,
}

impl DialogType {
    /// As CDP names it: "alert", "confirm", "prompt", "beforeunload", "print".
    pub fn name(self) -> &'static str {
        match self {
            DialogType::Alert => "alert",
            DialogType::Confirm => "confirm",
            DialogType::Prompt => "prompt",
            DialogType::BeforeUnload => "beforeunload",
            DialogType::Print => "print",
        }
    }
}

/// What becomes of a view's dialogs ([`View::set_dialog_policy`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DialogPolicy {
    /// To [`View::on_dialog`]'s function when there is one, held; else held
    /// for a CDP client attached to the page; else the content shell's way
    /// (dismissed at once, but for a macOS window's own alert).
    Default,
    /// Always held until answered.
    Hold,
    /// Answered at once with OK.
    Accept,
    /// Answered at once with Cancel.
    Dismiss,
}

/// A dialog a page opened.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Dialog {
    pub id: u64,
    pub kind: DialogType,
    pub message: String,
    pub default_prompt: String,
    /// The URL of the frame that opened it.
    pub url: String,
    /// A beforeunload of a reload.
    pub is_reload: bool,
    /// Whether it waits for an answer ([`View::respond_dialog`]); false when
    /// the policy answered it.
    pub held: bool,
}

/// A drag the page started.
#[derive(Debug, Clone)]
pub struct Drag {
    /// What it carries, as JSON: `text`, `html`, `htmlBaseUrl`, `urls`
    /// (`url`, `title`), `files` (paths), `custom` (type to data),
    /// `fileContents` (`name`, `url`, `base64`).
    pub data_json: String,
    /// What the page allows, `DRAG_*` combined.
    pub operations: u32,
    /// Where it started in the view, CSS pixels.
    pub x: f32,
    pub y: f32,
    /// Its picture, if the page gave one.
    pub image: Option<DragImage>,
}

/// A drag's picture: BGRA premultiplied, `stride` bytes a row.
#[derive(Debug, Clone)]
pub struct DragImage {
    pub pixels: Vec<u8>,
    pub width: u32,
    pub height: u32,
    pub stride: usize,
    /// Device pixels per CSS pixel.
    pub scale: f32,
    /// Where the cursor is in it, CSS pixels from its top left.
    pub offset_x: f32,
    pub offset_y: f32,
}

type DialogFunction = dyn FnMut(&Dialog);
type DialogClosedFunction = dyn FnMut(u64, bool, &str);
type DragFunction = dyn FnMut(&Drag) -> bool;

#[derive(Default)]
pub(crate) struct Slots {
    on_dialog: Slot<DialogFunction>,
    on_dialog_closed: Slot<DialogClosedFunction>,
    on_drag_start: Slot<DragFunction>,
    on_drag_end: Slot<dyn FnMut(u32)>,
}

impl Slots {
    pub(crate) fn clear(&self) {
        self.on_dialog.borrow_mut().take();
        self.on_dialog_closed.borrow_mut().take();
        self.on_drag_start.borrow_mut().take();
        self.on_drag_end.borrow_mut().take();
    }
}

fn slots<'a>(data: *mut c_void) -> &'a Slots {
    unsafe { &(*data.cast::<crate::view::Inner>()).page }
}

impl View {
    fn page_data(&self) -> *mut c_void {
        Rc::as_ptr(&self.inner).cast_mut().cast::<c_void>()
    }

    /// Calls `function` with every dialog the view's pages open. With a
    /// function, [`DialogPolicy::Default`] holds them for the application:
    /// answer with [`View::respond_dialog`].
    pub fn on_dialog<F: FnMut(&Dialog) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.page.on_dialog.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_dialog(raw, Some(on_dialog), self.page_data()) }
    }

    /// Stops [`View::on_dialog`].
    pub fn stop_dialogs(&self) {
        let Some(raw) = self.raw() else { return };
        unsafe { sys::zebkit_view_on_dialog(raw, None, std::ptr::null_mut()) }
        self.inner.page.on_dialog.borrow_mut().take();
    }

    /// Calls `function(id, accepted, user_input)` when a dialog closes,
    /// however it was answered.
    pub fn on_dialog_closed<F: FnMut(u64, bool, &str) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.page.on_dialog_closed.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_dialog_closed(raw, Some(on_dialog_closed), self.page_data()) }
    }

    pub fn set_dialog_policy(&self, policy: DialogPolicy) {
        let Some(raw) = self.raw() else { return };
        let value = match policy {
            DialogPolicy::Default => sys::ZEBKIT_DIALOG_POLICY_DEFAULT,
            DialogPolicy::Hold => sys::ZEBKIT_DIALOG_POLICY_HOLD,
            DialogPolicy::Accept => sys::ZEBKIT_DIALOG_POLICY_ACCEPT,
            DialogPolicy::Dismiss => sys::ZEBKIT_DIALOG_POLICY_DISMISS,
        };
        unsafe { sys::zebkit_view_set_dialog_policy(raw, value) }
    }

    pub fn dialog_policy(&self) -> DialogPolicy {
        let Some(raw) = self.raw() else { return DialogPolicy::Default };
        match unsafe { sys::zebkit_view_get_dialog_policy(raw) } {
            sys::ZEBKIT_DIALOG_POLICY_HOLD => DialogPolicy::Hold,
            sys::ZEBKIT_DIALOG_POLICY_ACCEPT => DialogPolicy::Accept,
            sys::ZEBKIT_DIALOG_POLICY_DISMISS => DialogPolicy::Dismiss,
            _ => DialogPolicy::Default,
        }
    }

    /// Answers dialog `id`: OK or Cancel, a prompt with `prompt_text`
    /// (`None` for its default). False when it is not open.
    pub fn respond_dialog(&self, id: u64, accept: bool, prompt_text: Option<&str>) -> bool {
        let Some(raw) = self.raw() else { return false };
        let text = prompt_text.map(c_string);
        unsafe {
            sys::zebkit_view_dialog_respond(raw, id, accept, text.as_ref().map_or(std::ptr::null(), |t| t.as_ptr()))
        }
    }

    /// The open dialogs, as a JSON array (`id`, `type`, `message`,
    /// `defaultPrompt`, `url`, `isReload`).
    pub fn dialogs_json(&self) -> String {
        let Some(raw) = self.raw() else { return "[]".to_string() };
        unsafe { take_string(sys::zebkit_view_dialogs(raw)) }.unwrap_or_else(|| "[]".to_string())
    }

    /// Closes the view as a tab is closed: the page's beforeunload first,
    /// whose dialog is reported; the view stays if the page is kept.
    pub fn request_close(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_request_close(raw) }
        }
    }

    /// Prints the page to PDF with CDP's `Page.printToPDF` parameters as a
    /// JSON object (`"{}"` for the defaults), and calls `done` with the
    /// document's bytes.
    pub fn print_to_pdf<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, options_json: &str, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        let options = c_string(options_json);
        unsafe { sys::zebkit_view_print_to_pdf(raw, options.as_ptr(), on_pdf::<F>, data.cast()) }
    }

    /// Calls `function` when the page starts a drag. Return false to have
    /// the engine run it inside the page (an off-screen view's mouse moves
    /// are then the drag's), true to run it yourself and end it with
    /// [`View::drag_source_end`].
    pub fn on_drag_start<F: FnMut(&Drag) -> bool + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.page.on_drag_start.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_drag_start(raw, Some(on_drag_start), self.page_data()) }
    }

    /// Calls `function` with what was done when the page's drag ends.
    pub fn on_drag_end<F: FnMut(u32) + 'static>(&self, function: F) {
        let Some(raw) = self.raw() else { return };
        *self.inner.page.on_drag_end.borrow_mut() = Some(Box::new(function));
        unsafe { sys::zebkit_view_on_drag_end(raw, Some(on_drag_end), self.page_data()) }
    }

    /// Whether a drag the page started is under way.
    pub fn is_dragging(&self) -> bool {
        self.raw().is_some_and(|raw| unsafe { sys::zebkit_view_is_dragging(raw) })
    }

    /// A drag from outside enters the page at (`x`, `y`) with `data_json`
    /// (as [`Drag::data_json`]), allowing `operations`; `modifiers` are
    /// [`crate::offscreen::Modifiers`] bits.
    pub fn drag_enter(&self, data_json: &str, x: f32, y: f32, operations: u32, modifiers: u32) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_drag_enter(raw, c_string(data_json).as_ptr(), x, y, operations, modifiers) }
        }
    }

    /// It moves; `done` gets the operation the page chose.
    pub fn drag_over<F: FnOnce(u32) + 'static>(&self, x: f32, y: f32, operations: u32, modifiers: u32, done: F) {
        let Some(raw) = self.raw() else { return };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_drag_over(raw, x, y, operations, modifiers, Some(on_operation::<F>), data.cast()) }
    }

    pub fn drag_leave(&self) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_drag_leave(raw) }
        }
    }

    /// It is dropped at (`x`, `y`); `done` gets the operation done.
    pub fn drop_data<F: FnOnce(u32) + 'static>(&self, x: f32, y: f32, modifiers: u32, done: F) {
        let Some(raw) = self.raw() else { return };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_drop(raw, x, y, modifiers, Some(on_operation::<F>), data.cast()) }
    }

    /// Ends the page's own drag at (`x`, `y`) with what was done with it.
    pub fn drag_source_end(&self, x: f32, y: f32, operation: u32) {
        if let Some(raw) = self.raw() {
            unsafe { sys::zebkit_view_drag_source_end(raw, x, y, operation) }
        }
    }
}

unsafe extern "C" fn on_dialog(_view: *mut sys::zebkit_view, dialog: *const sys::zebkit_dialog, data: *mut c_void) {
    let raw = &*dialog;
    let dialog = Dialog {
        id: raw.id,
        kind: match raw.type_ {
            sys::ZEBKIT_DIALOG_CONFIRM => DialogType::Confirm,
            sys::ZEBKIT_DIALOG_PROMPT => DialogType::Prompt,
            sys::ZEBKIT_DIALOG_BEFOREUNLOAD => DialogType::BeforeUnload,
            sys::ZEBKIT_DIALOG_PRINT => DialogType::Print,
            _ => DialogType::Alert,
        },
        message: string_from(raw.message).unwrap_or_default(),
        default_prompt: string_from(raw.default_prompt).unwrap_or_default(),
        url: string_from(raw.url).unwrap_or_default(),
        is_reload: raw.is_reload,
        held: raw.held,
    };
    call_slot(&slots(data).on_dialog, |function| function(&dialog));
}

unsafe extern "C" fn on_dialog_closed(
    _view: *mut sys::zebkit_view,
    id: u64,
    accepted: bool,
    user_input: *const c_char,
    data: *mut c_void,
) {
    let input = string_from(user_input).unwrap_or_default();
    call_slot(&slots(data).on_dialog_closed, |function| function(id, accepted, &input));
}

unsafe extern "C" fn on_pdf<F: FnOnce(Result<Vec<u8>, Error>)>(
    _view: *mut sys::zebkit_view,
    pdf: *const u8,
    size: usize,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    if pdf.is_null() {
        done(Err(Error::new(string_from(error).unwrap_or_else(|| "printing failed".to_string()))));
    } else {
        done(Ok(std::slice::from_raw_parts(pdf, size).to_vec()));
    }
}

unsafe extern "C" fn on_drag_start(_view: *mut sys::zebkit_view, drag: *const sys::zebkit_drag, data: *mut c_void) -> bool {
    let raw = &*drag;
    let image = (!raw.image.is_null() && raw.image_width > 0 && raw.image_height > 0).then(|| {
        let stride = raw.image_stride.max(0) as usize;
        let height = raw.image_height as u32;
        DragImage {
            pixels: std::slice::from_raw_parts(raw.image, stride * height as usize).to_vec(),
            width: raw.image_width as u32,
            height,
            stride,
            scale: raw.image_scale,
            offset_x: raw.image_offset_x,
            offset_y: raw.image_offset_y,
        }
    });
    let drag = Drag {
        data_json: string_from(raw.data_json).unwrap_or_else(|| "{}".to_string()),
        operations: raw.operations,
        x: raw.x,
        y: raw.y,
        image,
    };
    let mut taken = false;
    call_slot(&slots(data).on_drag_start, |function| taken = function(&drag));
    taken
}

unsafe extern "C" fn on_drag_end(_view: *mut sys::zebkit_view, operation: c_uint, data: *mut c_void) {
    call_slot(&slots(data).on_drag_end, |function| function(operation));
}

unsafe extern "C" fn on_operation<F: FnOnce(u32)>(_view: *mut sys::zebkit_view, operation: c_uint, data: *mut c_void) {
    let done = Box::from_raw(data.cast::<F>());
    done(operation);
}
