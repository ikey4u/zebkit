//! Media, with the FFmpeg in the engine (`<zebkit/media_tools.h>`, docs/MEDIA_TOOLS.md):
//! no external program, no shell, each argument literal. Its build has no
//! encoders: conversions are to another container, to WAV or raw PCM, to raw RGBA.

use super::{text, ok, Outcome, Run, Tool};
use crate::bounds::Bounds;
use crate::schema::{array, boolean, integer, number, object, string, described, choice};
use serde_json::{json, Value};
use std::path::PathBuf;
use std::time::{Duration, Instant};
use zebkit::media_tools::{ffmpeg_with_progress, ffprobe, Completion};

/// What a decoding tool writes at most, and how long any of them runs: a small file
/// can decode to a full disk, and asks for a lot of pixels cost nothing to ask.
const MAX_OUTPUT: u64 = 4 * 1024 * 1024 * 1024;
const MAX_TIME: Duration = Duration::from_secs(240);

fn paths(bounds: &'static Bounds, args: &Value) -> Result<(PathBuf, PathBuf), String> {
    let input = bounds.read_path(text(args, "input"), false)?;
    let output = bounds.write_path(text(args, "output"))?;
    if input == output {
        return Err("the input and the output are one file".to_string());
    }
    if !args["overwrite"].as_bool().unwrap_or(false) && output.symlink_metadata().is_ok() {
        return Err(format!("{}: exists; pass overwrite to replace it", output.display()));
    }
    Ok((input, output))
}

fn utf8(path: &std::path::Path) -> Result<&str, String> {
    path.to_str().ok_or_else(|| "a path that is not UTF-8".to_string())
}

fn finish(result: Result<Completion, zebkit::Error>, output: &std::path::Path) -> Outcome {
    match result {
        Ok(Completion::Completed) => {
            let size = std::fs::metadata(output).map(|meta| meta.len()).unwrap_or(0);
            ok(json!({"output": output.display().to_string(), "bytes": size}))
        }
        Ok(Completion::Cancelled) => Err(format!("stopped: over the limit of {} bytes of output or {} seconds", MAX_OUTPUT, MAX_TIME.as_secs())),
        Err(error) => Err(error.message().to_string()),
    }
}

fn overwrite_flag(args: &Value) -> &'static str {
    if args["overwrite"].as_bool().unwrap_or(false) { "-y" } else { "-n" }
}

fn maps(args: &Value) -> Vec<String> {
    args["streams"].as_array().into_iter().flatten().filter_map(Value::as_u64).flat_map(|index| ["-map".to_string(), format!("0:{index}")]).collect()
}

fn media_probe(bounds: &'static Bounds, args: &Value) -> Outcome {
    let path = bounds.read_path(text(args, "path"), false)?;
    let json = ffprobe(&["-i", utf8(&path)?]).map_err(|error| error.message().to_string())?;
    ok(serde_json::from_str(&json).unwrap_or(Value::String(json)))
}

fn media_convert(bounds: &'static Bounds, args: &Value) -> Outcome {
    let (input, output) = paths(bounds, args)?;
    let mut owned = vec![overwrite_flag(args).to_string(), "-i".to_string(), utf8(&input)?.to_string()];
    owned.extend(maps(args));
    owned.extend(["-c".to_string(), "copy".to_string(), "--".to_string(), utf8(&output)?.to_string()]);
    run(&owned, &output, 0)
}

/// Runs a job, stopped between packets at the time limit, or when `bytes_a_second`
/// (of the output, for a second of the media) says it is over the size limit.
fn run(owned: &[String], output: &std::path::Path, bytes_a_second: u64) -> Outcome {
    let list: Vec<&str> = owned.iter().map(String::as_str).collect();
    let start = Instant::now();
    let result = ffmpeg_with_progress(&list, |_, time| {
        let seconds = time.unwrap_or(0).max(0) as u64 / 1_000_000;
        start.elapsed() < MAX_TIME && seconds.saturating_mul(bytes_a_second) <= MAX_OUTPUT
    });
    finish(result, output)
}

/// The largest value of a number of the input's streams.
fn stream_value(path: &str, key: &str) -> Option<u64> {
    let probe: Value = serde_json::from_str(&ffprobe(&["-i", path]).ok()?).ok()?;
    probe["streams"].as_array()?.iter().filter_map(|stream| stream[key].as_u64().or_else(|| stream[key].as_str()?.parse().ok())).max()
}

fn media_extract_audio(bounds: &'static Bounds, args: &Value) -> Outcome {
    let (input, output) = paths(bounds, args)?;
    let mut owned = vec![overwrite_flag(args).to_string(), "-i".to_string(), utf8(&input)?.to_string()];
    owned.extend(maps(args));
    owned.extend(["-c:a".to_string(), "pcm_s16le".to_string(), "-f".to_string(), args["format"].as_str().unwrap_or("wav").to_string()]);
    if let Some(rate) = args["sampleRate"].as_u64() {
        owned.extend(["-ar".to_string(), rate.to_string()]);
    }
    if let Some(channels) = args["channels"].as_u64() {
        owned.extend(["-ac".to_string(), channels.to_string()]);
    }
    owned.extend(["--".to_string(), utf8(&output)?.to_string()]);
    let rate = args["sampleRate"].as_u64().or_else(|| stream_value(utf8(&input).ok()?, "sample_rate")).unwrap_or(48_000);
    let channels = args["channels"].as_u64().or_else(|| stream_value(utf8(&input).ok()?, "channels")).unwrap_or(2);
    run(&owned, &output, rate.saturating_mul(channels).saturating_mul(2))
}

fn media_extract_frames(bounds: &'static Bounds, args: &Value) -> Outcome {
    let (input, output) = paths(bounds, args)?;
    let mut owned = vec![overwrite_flag(args).to_string(), "-i".to_string(), utf8(&input)?.to_string()];
    owned.extend(maps(args));
    owned.extend(["-c:v".to_string(), "rawvideo".to_string()]);
    if let (Some(width), Some(height)) = (args["width"].as_u64(), args["height"].as_u64()) {
        owned.extend(["-s".to_string(), format!("{width}x{height}")]);
    }
    let frames = args["frames"].as_u64().unwrap_or(1);
    let width = args["width"].as_u64().or_else(|| stream_value(utf8(&input).ok()?, "width")).unwrap_or(0);
    let height = args["height"].as_u64().or_else(|| stream_value(utf8(&input).ok()?, "height")).unwrap_or(0);
    let bytes = frames.saturating_mul(width).saturating_mul(height).saturating_mul(4);
    if bytes > MAX_OUTPUT {
        return Err(format!("{frames} frames of {width} by {height} are {bytes} bytes, over the limit of {MAX_OUTPUT}"));
    }
    owned.extend(["-frames:v".to_string(), frames.to_string(), "--".to_string(), utf8(&output)?.to_string()]);
    run(&owned, &output, 0)
}

fn media_cut(bounds: &'static Bounds, args: &Value) -> Outcome {
    let (input, output) = paths(bounds, args)?;
    let start = args["start"].as_f64().unwrap_or(0.0);
    let end = args["end"].as_f64().ok_or("end is required")?;
    if end <= start {
        return Err("end must be after start".to_string());
    }
    let owned = vec![overwrite_flag(args).to_string(), "-i".to_string(), utf8(&input)?.to_string(),
        "-ss".to_string(), format!("{start:.6}"), "-t".to_string(), format!("{:.6}", end - start),
        "-c".to_string(), "copy".to_string(), "--".to_string(), utf8(&output)?.to_string()];
    let mut answer = run(&owned, &output, 0)?;
    if let Some(structured) = &mut answer.structured {
        structured["note"] = json!("cut at the packets: it starts at the key frame before start");
    }
    Ok(answer)
}

fn tool(name: &'static str, description: &'static str, schema: Value, read_only: bool, run: fn(&'static Bounds, &Value) -> Outcome) -> Tool {
    Tool { name, group: "media", description, schema, read_only, destructive: false, run: Run::Worker(run) }
}

pub fn tools() -> Vec<Tool> {
    let streams = || ("streams", described(array(integer(0, 1023), 64), "Stream indices from media_probe"));
    let files = || [("input", string()), ("output", string()), ("overwrite", boolean())];
    let with = |extra: &[(&'static str, Value)]| {
        let mut all: Vec<(&str, Value)> = files().to_vec();
        all.push(streams());
        all.extend_from_slice(extra);
        all
    };
    vec![
        tool("media_probe", "A media file's container and streams, with the FFmpeg in the engine.", object(&[("path", string())], &["path"]), true, media_probe),
        tool("media_convert", "Copy streams into another container (MP4, MOV, Matroska, WebM, M4A) by the output's name, without re-encoding: the engine has no encoders.", object(&with(&[]), &["input", "output"]), false, media_convert),
        tool("media_cut", "Copy the part of a file from start to end, in seconds, without re-encoding; it begins at the key frame before start.",
            object(&[("input", string()), ("output", string()), ("overwrite", boolean()), ("start", number(0.0, 1e9)), ("end", number(0.0, 1e9))], &["input", "output", "end"]), false, media_cut),
        tool("media_extract_audio", "Decode audio to WAV or raw PCM16, resampled if asked.",
            object(&with(&[("sampleRate", integer(1, 384000)), ("channels", integer(1, 32)), ("format", choice(&["wav", "s16le"]))]), &["input", "output"]), false, media_extract_audio),
        tool("media_extract_frames", "Decode video frames to raw RGBA (width * 4 bytes a row, no headers), scaled if asked.",
            object(&with(&[("width", integer(1, 8192)), ("height", integer(1, 8192)), ("frames", integer(1, 100_000))]), &["input", "output"]), false, media_extract_frames),
    ]
}
