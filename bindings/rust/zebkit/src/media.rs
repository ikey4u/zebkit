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
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::path::Path;
use std::rc::Rc;

/// Players keep what a capture starts from from the engine's start (the
/// video since their last keyframe, up to about 16 MiB for each video
/// stream), so that a capture starts at the frame on screen even when
/// nothing has listed media yet. Off by default: then only once
/// [`View::media_list`] or [`View::media_at`] ran in the page's process.
/// Before [`crate::run`]; the same as `--zebkit-media-history`.
pub fn keep_history(keep: bool) {
    unsafe { sys::zebkit_media_keep_history(keep) }
}

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
        self.media_capture(media_id, path, options_json)
    }

    /// [`View::media_capture_start`], by the name the other bindings give it.
    pub fn media_capture(&self, media_id: &str, path: impl AsRef<Path>, options_json: &str) -> Result<MediaCapture, Error> {
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
    on_done: Slot<dyn FnOnce(&str)>,
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
        self.slots.on_progress.set(Some(Box::new(function)));
    }

    /// `function` gets the capture's end once its files are written:
    /// `reason`, `error` (when it failed), `ranges`, and `files` (`path`,
    /// `format`, `start`, `duration`, `bytes`, `gaps`, `tracks`).
    pub fn on_done<F: FnOnce(&str) + 'static>(&self, function: F) {
        self.slots.on_done.set(Some(Box::new(function)));
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
    let result = script_result(json, error);
    crate::guard(|| done(result));
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
    if let Some(function) = slots.on_done.take() {
        crate::guard(|| function(&json));
    }
}


type TapSlot = Slot<dyn FnMut(&str, &[u8])>;

/// A native media tap: encoded packets, sampled RGBA frames, planar f32 PCM,
/// and optionally an encoded file. All callbacks run on the engine thread.
/// Dropping stops it and suppresses callbacks. Call `stop()` and wait for the
/// `done` event before shutting down the engine to finalize a recording.
pub struct MediaTap {
    raw: *mut sys::zebkit_media_tap,
    // A callback may drop the handle; the trampoline holds its own Rc.
    _callback: Rc<TapSlot>,
}

impl View {
    /// Starts native media output for an ID from `media_list()`.
    /// `options_json` accepts `packets`, `video`, `audio` (default true),
    /// `maxFps`, `maxWidth`, `maxHeight`, `maxPendingBytes`, `includeBuffered`,
    /// and an optional absolute `path` with `recordingOptions` for file output.
    /// MediaStream requires `packets:false` and no path. Callback metadata is
    /// JSON; payload bytes are borrowed for this call. Copy for later use.
    /// Packet timestamps are decimal strings to preserve int64 precision.
    pub fn media_tap_start<F>(&self, media_id: &str, options_json: &str, callback: F)
        -> Result<MediaTap, Error>
    where F: FnMut(&str, &[u8]) + 'static {
        let raw = self.raw().ok_or_else(|| Error::new("the view is closed"))?;
        let id = CString::new(media_id).map_err(|_| Error::new("the media id has a zero byte"))?;
        let options = CString::new(options_json).map_err(|_| Error::new("the options have a zero byte"))?;
        let slot: Rc<TapSlot> = Rc::new(Slot::new());
        slot.set(Some(Box::new(callback)));
        let mut error = std::ptr::null_mut();
        let tap = unsafe { sys::zebkit_media_tap_start(raw, id.as_ptr(), options.as_ptr(),
            Some(on_tap), Rc::as_ptr(&slot).cast_mut().cast(), &mut error) };
        if tap.is_null() {
            return Err(unsafe { take_error(error, "the media tap did not start") });
        }
        Ok(MediaTap { raw: tap, _callback: slot })
    }
}
impl MediaTap {
    /// Stop samples and finalize any file. The callback gets `type: done` later.
    pub fn stop(&self) { unsafe { sys::zebkit_media_tap_stop(self.raw) } }
}
impl Drop for MediaTap {
    fn drop(&mut self) { unsafe { sys::zebkit_media_tap_free(self.raw) } }
}
unsafe extern "C" fn on_tap(_tap: *mut sys::zebkit_media_tap, json: *const c_char,
    bytes: *const u8, size: usize, data: *mut c_void) {
    let ptr = data.cast_const().cast::<TapSlot>();
    Rc::increment_strong_count(ptr);
    let slot = Rc::from_raw(ptr);
    let json = string_from(json).unwrap_or_default();
    let bytes = if size == 0 { &[] } else { std::slice::from_raw_parts(bytes, size) };
    call_slot(&slot, |callback| callback(&json, bytes));
}
