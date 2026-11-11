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

/// Format and stream metadata of a local media file, as `ffprobe(&["-i", path])`.
pub fn probe(path: &str) -> Result<String, Error> {
    let path = CString::new(path).map_err(|_| Error::new("the path contains NUL"))?;
    let mut json = std::ptr::null_mut();
    let mut error = std::ptr::null_mut();
    let code = unsafe { sys::zebkit_media_probe(path.as_ptr(), &mut json, &mut error) };
    let message = unsafe { take_string(error) };
    let result = unsafe { take_string(json) };
    if code != 0 { Err(Error::new(message.unwrap_or_else(|| "media probe failed".into()))) }
    else { result.ok_or_else(|| Error::new("probe returned no JSON")) }
}

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
    remux_with(source, destination, &RemuxOptions { overwrite, ..Default::default() }, |_, _| true)
}

/// Decode to interleaved PCM16 in a WAV container, optionally resampling.
pub fn extract_audio(source: &str, destination: &str, rate: u32, channels: u32, overwrite: bool) -> Result<Completion, Error> {
    let options = AudioOptions { sample_rate: rate, channels, overwrite, ..Default::default() };
    extract_audio_with(source, destination, &options, |_, _| true)
}

/// Decode and scale packed RGBA video frames; count must be positive.
pub fn extract_video(source: &str, destination: &str, width: u32, height: u32, count: u32, overwrite: bool) -> Result<Completion, Error> {
    let options = VideoOptions { size: Some((width, height)), frames: Some(count), overwrite, ..Default::default() };
    extract_video_with(source, destination, &options, |_, _| true)
}

/// What [`remux_with`] does beyond copying: the streams (indices of the
/// source's; `None` for its audio and video) and the container's format.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct RemuxOptions {
    pub streams: Option<Vec<u32>>,
    pub format: Option<String>,
    pub overwrite: bool,
}

/// What [`extract_audio_with`] makes: `raw` for headerless s16le instead of WAV.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AudioOptions {
    pub sample_rate: u32,
    pub channels: u32,
    pub stream: Option<u32>,
    pub raw: bool,
    pub overwrite: bool,
}

impl Default for AudioOptions {
    fn default() -> Self {
        AudioOptions { sample_rate: 16000, channels: 1, stream: None, raw: false, overwrite: false }
    }
}

/// What [`extract_video_with`] makes: frames scaled to `size`, at most `frames` of them.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct VideoOptions {
    pub size: Option<(u32, u32)>,
    pub frames: Option<u32>,
    pub stream: Option<u32>,
    pub overwrite: bool,
}

fn run(mut args: Vec<String>, destination: &str, progress: impl FnMut(i64, Option<i64>) -> bool) -> Result<Completion, Error> {
    args.push("--".into());
    args.push(destination.into());
    let args: Vec<&str> = args.iter().map(String::as_str).collect();
    ffmpeg_with_progress(&args, progress)
}

fn start(source: &str, overwrite: bool) -> Vec<String> {
    vec![if overwrite { "-y" } else { "-n" }.into(), "-i".into(), source.into()]
}

/// [`remux`] with its options and a progress function, as [`ffmpeg_with_progress`]'s.
pub fn remux_with<F: FnMut(i64, Option<i64>) -> bool>(source: &str, destination: &str, options: &RemuxOptions, progress: F) -> Result<Completion, Error> {
    let mut args = start(source, options.overwrite);
    args.extend(["-c".into(), "copy".into()]);
    if let Some(streams) = &options.streams {
        if streams.is_empty() { return Err(Error::new("streams must not be empty")); }
        for stream in streams {
            args.extend(["-map".into(), format!("0:{stream}")]);
        }
    }
    if let Some(format) = &options.format {
        args.extend(["-f".into(), format.clone()]);
    }
    run(args, destination, progress)
}

/// [`extract_audio`] with its options and a progress function.
pub fn extract_audio_with<F: FnMut(i64, Option<i64>) -> bool>(source: &str, destination: &str, options: &AudioOptions, progress: F) -> Result<Completion, Error> {
    let mut args = start(source, options.overwrite);
    args.extend(["-c:a".into(), "pcm_s16le".into(), "-ar".into(), options.sample_rate.to_string(),
        "-ac".into(), options.channels.to_string(), "-f".into(), if options.raw { "s16le" } else { "wav" }.into()]);
    if let Some(stream) = options.stream {
        args.extend(["-map".into(), format!("0:{stream}")]);
    }
    run(args, destination, progress)
}

/// [`extract_video`] with its options and a progress function: packed RGBA
/// frames, no headers ([`probe`] has the source's size).
pub fn extract_video_with<F: FnMut(i64, Option<i64>) -> bool>(source: &str, destination: &str, options: &VideoOptions, progress: F) -> Result<Completion, Error> {
    let mut args = start(source, options.overwrite);
    args.extend(["-c:v".into(), "rawvideo".into(), "-f".into(), "rawvideo".into()]);
    if let Some((width, height)) = options.size {
        args.extend(["-s".into(), format!("{width}x{height}")]);
    }
    if let Some(frames) = options.frames {
        args.extend(["-frames:v".into(), frames.to_string()]);
    }
    if let Some(stream) = options.stream {
        args.extend(["-map".into(), format!("0:{stream}")]);
    }
    run(args, destination, progress)
}
