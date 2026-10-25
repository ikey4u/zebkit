//! The media a page plays, saved as it plays (`<zebkit/media.h>`): a
//! page's `<audio>` and `<video>` elements, and captures of what one of
//! them plays, copied into MP4, WebM or Matroska without decoding it,
//! whether the element loads a file, a player appends through Media Source
//! Extensions, or the engine plays HLS itself.
//!
//! ```no_run
//! use zebkit::View;
//!
//! std::process::exit(zebkit::run(|| {
//!     let view = View::new(Some("https://example.com/video"), 1024, 768);
//!     let page = view.clone();
//!     view.media_list(move |list| {
//!         // [{"id": "4.12.3", "kind": "mse", "codecs": {...}, ...}]
//!         let Ok(list) = list else { return };
//!         let Some(id) = list.split("\"id\":\"").nth(1).and_then(|rest| rest.split('"').next()) else { return };
//!         let capture = page.media_capture_start(id, "/tmp/video.mp4", "{}").unwrap();
//!         capture.on_progress(|state| println!("{state}"));
//!         capture.on_done(|done| println!("written: {done}"));
//!         // ... later: capture.stop(); keep `capture` until then.
//!         std::mem::forget(capture);
//!     });
//! }));
//! ```

use crate::view::{call_slot, Slot};
use crate::{c_string, script_result, string_from, sys, take_error, take_string, Error, View};
use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::path::Path;
use std::rc::Rc;

impl View {
    /// The `<audio>` and `<video>` elements in every frame of the page, as a
    /// JSON array: `id` (for [`View::media_capture_start`]), `element`,
    /// `tag`, `path` (a CSS selector in its frame), `frame` (`url`, `main`,
    /// `name`), `src`, `kind` (`file`, `mse`, `hls`, `stream`, `none`),
    /// `duration`, `live`, `currentTime`, `buffered`, `codecs` (`audio`,
    /// `video`), `hasAudio`, `hasVideo`, `width`, `height`, `playing`,
    /// `encrypted`, `capturable`, `capturing`.
    pub fn media_list<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_media_list(raw, Some(on_result::<F>), data.cast()) }
    }

    /// The media element at (`x`, `y`) of the view (its coordinates, as its
    /// input events have them), under it, or whose box holds it, in any
    /// frame: its entry as [`View::media_list`] has it, or `"null"`.
    pub fn media_at<F: FnOnce(Result<String, Error>) + 'static>(&self, x: f32, y: f32, done: F) {
        let Some(raw) = self.raw() else {
            done(Err(Error::new("the view is closed")));
            return;
        };
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_view_media_at(raw, x, y, Some(on_result::<F>), data.cast()) }
    }

    /// Starts copying what media `media_id` plays to `path` (absolute),
    /// with `options_json`: `format` (`"mp4"`, `"webm"`, `"mkv"`,
    /// `"auto"`), `includeBuffered` (what the player holds behind the
    /// playhead too), `closeGaps` (seconds, or `false`), `maxBytes`,
    /// `maxDuration` (seconds). `path` must be UTF-8. The files are
    /// written when it stops ([`MediaCapture::stop`], or when the page goes
    /// or the element loads other media), and reported to
    /// [`MediaCapture::on_done`].
    pub fn media_capture_start(&self, media_id: &str, path: impl AsRef<Path>, options_json: &str) -> Result<MediaCapture, Error> {
        let Some(raw) = self.raw() else {
            return Err(Error::new("the view is closed"));
        };
        let path = path_c_string(path.as_ref())?;
        let mut error: *mut c_char = std::ptr::null_mut();
        let capture = unsafe {
            sys::zebkit_media_capture_start(
                raw,
                c_string(media_id).as_ptr(),
                path.as_ptr(),
                c_string(options_json).as_ptr(),
                &mut error,
            )
        };
        if capture.is_null() {
            return Err(unsafe { take_error(error, "the capture did not start") });
        }
        let slots = Rc::new(Slots::default());
        let data = Rc::as_ptr(&slots).cast_mut().cast::<c_void>();
        unsafe {
            sys::zebkit_media_capture_on_progress(capture, Some(on_progress), data);
            sys::zebkit_media_capture_on_done(capture, Some(on_done), data);
        }
        Ok(MediaCapture { raw: capture, slots })
    }
}

#[derive(Default)]
struct Slots {
    on_progress: Slot<dyn FnMut(&str)>,
    on_done: RefCell<Option<Box<dyn FnOnce(&str)>>>,
}

/// A capture of what a media element plays. Dropping it lets it go: a
/// capture still running is stopped and its files are written, but its
/// functions are not called any more.
pub struct MediaCapture {
    raw: *mut sys::zebkit_media_capture,
    // Shared with a function that runs: one that drops the capture does not
    // free what it runs from.
    slots: Rc<Slots>,
}

impl MediaCapture {
    /// `function` gets the capture's state as JSON about twice a second
    /// while it runs: `state`, `bytes`, `packets`, `duration`, `ranges`
    /// (the media's times captured), `tracks`.
    pub fn on_progress<F: FnMut(&str) + 'static>(&self, function: F) {
        *self.slots.on_progress.borrow_mut() = Some(Box::new(function));
    }

    /// `function` gets the capture's end once its files are written:
    /// `reason`, `error` (when it failed), `ranges`, and `files` (`path`,
    /// `format`, `start`, `duration`, `bytes`, `gaps`, `tracks`).
    pub fn on_done<F: FnOnce(&str) + 'static>(&self, function: F) {
        *self.slots.on_done.borrow_mut() = Some(Box::new(function));
    }

    /// Stops the capture and writes its files.
    pub fn stop(&self) {
        unsafe { sys::zebkit_media_capture_stop(self.raw) }
    }

    /// The state now, as JSON, with `state` (`starting`, `capturing`,
    /// `finishing`, `done`) and `media`, the element's entry.
    pub fn status(&self) -> String {
        unsafe { take_string(sys::zebkit_media_capture_status(self.raw)) }.unwrap_or_else(|| "{}".to_string())
    }
}

impl Drop for MediaCapture {
    fn drop(&mut self) {
        // No function is called after this, so the slots may go.
        unsafe { sys::zebkit_media_capture_free(self.raw) }
    }
}

unsafe extern "C" fn on_result<F: FnOnce(Result<String, Error>)>(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    done(script_result(json, error));
}

/// The capture's slots, held while its function runs: the engine calls
/// none once the capture is freed, so they are there when one starts.
unsafe fn slots_of(data: *mut c_void) -> Rc<Slots> {
    let slots = data.cast_const().cast::<Slots>();
    Rc::increment_strong_count(slots);
    Rc::from_raw(slots)
}

/// The path as the engine takes it (UTF-8), without changing it.
fn path_c_string(path: &Path) -> Result<CString, Error> {
    let text = path.to_str().ok_or_else(|| Error::new("the path is not UTF-8"))?;
    CString::new(text).map_err(|_| Error::new("the path has a zero byte"))
}

unsafe extern "C" fn on_progress(_capture: *mut sys::zebkit_media_capture, json: *const c_char, data: *mut c_void) {
    let slots = slots_of(data);
    let json = string_from(json).unwrap_or_default();
    call_slot(&slots.on_progress, |function| function(&json));
}

unsafe extern "C" fn on_done(_capture: *mut sys::zebkit_media_capture, json: *const c_char, data: *mut c_void) {
    let slots = slots_of(data);
    let json = string_from(json).unwrap_or_default();
    let function = slots.on_done.borrow_mut().take();
    if let Some(function) = function {
        function(&json);
    }
}
