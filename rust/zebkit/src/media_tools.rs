//! Synchronous local media tools, with the FFmpeg built into zebkit.
//!
//! No engine loop or external executable is needed. Run long jobs on worker
//! threads. argv excludes the program name; no shell parsing takes place.
//! See docs/MEDIA_TOOLS.md for the supported subset (no encoders/filters).
use crate::{sys, take_string, Error};
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, resume_unwind, AssertUnwindSafe};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Completion { Completed, Cancelled }

fn arguments(args: &[&str]) -> Result<Vec<CString>, Error> {
    if args.is_empty() || args.len() > 4096 { return Err(Error::new("expected 1..4096 arguments")); }
    args.iter().map(|s| CString::new(*s).map_err(|_| Error::new("argument contains NUL"))).collect()
}

/// JSON containing format and stream metadata, as a subset of ffprobe's schema.
pub fn ffprobe(args: &[&str]) -> Result<String, Error> {
    let strings = arguments(args)?;
    let pointers: Vec<*const c_char> = strings.iter().map(|s| s.as_ptr()).collect();
    let mut json = std::ptr::null_mut();
    let mut error = std::ptr::null_mut();
    let code = unsafe { sys::zebkit_ffprobe(pointers.len() as c_int, pointers.as_ptr(), &mut json, &mut error) };
    let message = unsafe { take_string(error) };
    let result = unsafe { take_string(json) };
    if code != 0 { Err(Error::new(message.unwrap_or_else(|| "media probe failed".into()))) }
    else { result.ok_or_else(|| Error::new("probe returned no JSON")) }
}

pub fn probe(path: &str) -> Result<String, Error> { ffprobe(&["-i", path]) }

/// Run a supported FFmpeg operation. The destination is published only on
/// success. Unknown parameters fail, rather than being silently ignored.
pub fn ffmpeg(args: &[&str]) -> Result<Completion, Error> {
    ffmpeg_with_progress(args, |_, _| true)
}

/// Return false from progress to cancel. Timestamp is in microseconds and can
/// be unknown. Panics are caught at the FFI boundary and resumed after cleanup.
pub fn ffmpeg_with_progress<F: FnMut(i64, Option<i64>) -> bool>(args: &[&str], progress: F) -> Result<Completion, Error> {
    struct State<F> { callback: F, panic: Option<Box<dyn std::any::Any + Send>> }
    unsafe extern "C" fn callback<F: FnMut(i64, Option<i64>) -> bool>(packets: i64, time: i64, data: *mut c_void) -> c_int {
        let state = &mut *data.cast::<State<F>>();
        match catch_unwind(AssertUnwindSafe(|| (state.callback)(packets, (time != i64::MIN).then_some(time)))) {
            Ok(value) => value as c_int,
            Err(panic) => { state.panic = Some(panic); 0 }
        }
    }
    let strings = arguments(args)?;
    let pointers: Vec<*const c_char> = strings.iter().map(|s| s.as_ptr()).collect();
    let mut state = State { callback: progress, panic: None };
    let mut error = std::ptr::null_mut();
    let code = unsafe { sys::zebkit_ffmpeg(pointers.len() as c_int, pointers.as_ptr(), Some(callback::<F>), (&mut state as *mut State<F>).cast(), &mut error) };
    let message = unsafe { take_string(error) };
    if let Some(panic) = state.panic { resume_unwind(panic); }
    match code {
        0 => Ok(Completion::Completed),
        1 => Ok(Completion::Cancelled),
        _ => Err(Error::new(message.unwrap_or_else(|| "media processing failed".into())))
    }
}

/// Copy audio/video into another container, without transcoding.
pub fn remux(source: &str, destination: &str, overwrite: bool) -> Result<Completion, Error> {
    ffmpeg(&[if overwrite { "-y" } else { "-n" }, "-i", source, "-c", "copy", "--", destination])
}

/// Decode to interleaved PCM16 in a WAV container, optionally resampling.
pub fn extract_audio(source: &str, destination: &str, rate: u32, channels: u32, overwrite: bool) -> Result<Completion, Error> {
    ffmpeg(&[if overwrite { "-y" } else { "-n" }, "-i", source, "-c:a", "pcm_s16le",
        "-ar", &rate.to_string(), "-ac", &channels.to_string(), "-f", "wav", "--", destination])
}

/// Decode and scale packed RGBA video frames; count must be positive.
pub fn extract_video(source: &str, destination: &str, width: u32, height: u32, count: u32, overwrite: bool) -> Result<Completion, Error> {
    ffmpeg(&[if overwrite { "-y" } else { "-n" }, "-i", source, "-c:v", "rawvideo",
        "-s", &format!("{width}x{height}"), "-frames:v", &count.to_string(), "--", destination])
}
