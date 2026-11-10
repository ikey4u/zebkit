package zebkit

/*
#include "bridge.h"
#include "bridge_c.h"
*/
import "C"

import (
	"runtime/cgo"
	"strconv"
	"unsafe"
)

// The media a page plays, saved as it plays (<zebkit/media.h>), its packets,
// frames and sound as they play (<zebkit/media_tap.h>), and FFmpeg's tools
// inside the engine (<zebkit/media_tools.h>).

// MediaKeepHistory keeps what media elements played before a capture asks.
func MediaKeepHistory(keep bool) { C.zebkit_media_keep_history(C.bool(keep)) }

func resultDone(done func(string, error)) C.uintptr_t {
	var h C.uintptr_t
	h = handle(func(result, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if failed {
			done("", &Error{failure})
		} else {
			done(result, nil)
		}
	})
	return h
}

// MediaList lists the page's <audio> and <video> elements as a JSON array
// (<zebkit/media.h> has its members); "id" is what a capture takes.
func (v *View) MediaList(done func(json string, err error)) {
	C.zebkit_view_media_list(v.view, C.zebkit_result_func(C.zk_result), data(resultDone(done)))
}

// MediaAt is the media element at (x, y), CSS pixels in the view, as JSON
// (null for none).
func (v *View) MediaAt(x, y float32, done func(json string, err error)) {
	C.zebkit_view_media_at(v.view, C.float(x), C.float(y), C.zebkit_result_func(C.zk_result), data(resultDone(done)))
}

// MediaCapture copies what a media element plays to a file while it plays.
type MediaCapture struct {
	capture        *C.zebkit_media_capture
	progress, done slot
}

// MediaCaptureStart starts copying media id to path, with options as JSON
// ("" for none: format, wallClock, limits, ...).
func (v *View) MediaCaptureStart(mediaID, path, options string) (*MediaCapture, error) {
	cid, cpath := cString(mediaID), cString(path)
	defer free(cid)
	defer free(cpath)
	coptions := optionalString(options)
	defer free(coptions)
	var message *C.char
	capture := C.zebkit_media_capture_start(v.view, cid, cpath, coptions, &message)
	if capture == nil {
		return nil, takeError(message, "the capture did not start")
	}
	return &MediaCapture{capture: capture}, nil
}

// OnProgress calls function with the capture's state as JSON as it goes.
func (c *MediaCapture) OnProgress(function func(json string)) {
	if c.capture == nil {
		return
	}
	if function == nil {
		C.zebkit_media_capture_on_progress(c.capture, nil, nil)
		c.progress.clear()
		return
	}
	h := c.progress.set(function)
	C.zebkit_media_capture_on_progress(c.capture, C.zebkit_media_capture_func(C.zkc_capture_json), data(h))
}

// OnDone calls function once, with the final state, when the files are written.
func (c *MediaCapture) OnDone(function func(json string)) {
	if c.capture == nil {
		return
	}
	if function == nil {
		C.zebkit_media_capture_on_done(c.capture, nil, nil)
		c.done.clear()
		return
	}
	h := c.done.set(function)
	C.zebkit_media_capture_on_done(c.capture, C.zebkit_media_capture_func(C.zkc_capture_json), data(h))
}

// Stop stops the capture and writes its files; OnDone's function follows.
func (c *MediaCapture) Stop() {
	if c.capture != nil {
		C.zebkit_media_capture_stop(c.capture)
	}
}

// Status is the capture's state as JSON.
func (c *MediaCapture) Status() string {
	if c.capture == nil {
		return ""
	}
	return takeString(C.zebkit_media_capture_status(c.capture))
}

// Free lets the capture go; one still running is stopped.
func (c *MediaCapture) Free() {
	if c.capture == nil {
		return
	}
	C.zebkit_media_capture_free(c.capture)
	c.capture = nil
	c.progress.clear()
	c.done.clear()
}

// MediaTap is real-time access to a media element's encoded packets,
// displayed video (RGBA) and sound (planar float32).
type MediaTap struct {
	tap      *C.zebkit_media_tap
	callback slot
}

// MediaTapStart taps media id with options as JSON (packets, video, audio,
// maxFps, ...; <zebkit/media_tap.h>); function gets each event as JSON with
// its bytes, copied: audioConfig, videoConfig, packet, video, audio,
// timeline, progress, done.
func (v *View) MediaTapStart(mediaID, options string, function func(json string, bytes []byte)) (*MediaTap, error) {
	tap := &MediaTap{}
	h := tap.callback.set(function)
	cid := cString(mediaID)
	defer free(cid)
	coptions := optionalString(options)
	defer free(coptions)
	var message *C.char
	tap.tap = C.zebkit_media_tap_start(v.view, cid, coptions, C.zebkit_media_tap_func(C.zkc_tap), data(h), &message)
	if tap.tap == nil {
		tap.callback.clear()
		return nil, takeError(message, "the tap did not start")
	}
	return tap, nil
}

// Stop stops the tap; its done event follows.
func (t *MediaTap) Stop() {
	if t.tap != nil {
		C.zebkit_media_tap_stop(t.tap)
	}
}

// Free stops the tap and lets it go: no event follows.
func (t *MediaTap) Free() {
	if t.tap == nil {
		return
	}
	C.zebkit_media_tap_free(t.tap)
	t.tap = nil
	t.callback.clear()
}

// MediaCompletion is how a media tool ended.
type MediaCompletion int

const (
	MediaCompleted MediaCompletion = 0
	MediaCancelled MediaCompletion = 1
)

func toolArgs(args []string) (C.int, **C.char, func()) {
	array := C.malloc(C.size_t(len(args)+1) * C.size_t(unsafe.Sizeof(uintptr(0))))
	items := unsafe.Slice((**C.char)(array), len(args)+1)
	for i, arg := range args {
		items[i] = C.CString(arg)
	}
	items[len(args)] = nil
	return C.int(len(args)), (**C.char)(array), func() {
		for i := range args {
			C.free(unsafe.Pointer(items[i]))
		}
		C.free(array)
	}
}

// FFprobe runs the engine's ffprobe with args (without the program's name)
// and returns its JSON. No engine needs to run; any goroutine.
func FFprobe(args ...string) (string, error) {
	argc, argv, done := toolArgs(args)
	defer done()
	var json, message *C.char
	if C.zebkit_ffprobe(argc, argv, &json, &message) != 0 {
		return "", takeError(message, "ffprobe failed")
	}
	if message != nil {
		C.zebkit_free(unsafe.Pointer(message))
	}
	return takeString(json), nil
}

// MediaProbe is ffprobe of path, with its format and streams.
func MediaProbe(path string) (string, error) {
	cpath := cString(path)
	defer free(cpath)
	var json, message *C.char
	if C.zebkit_media_probe(cpath, &json, &message) != 0 {
		return "", takeError(message, "the probe failed")
	}
	if message != nil {
		C.zebkit_free(unsafe.Pointer(message))
	}
	return takeString(json), nil
}

// FFmpeg runs the engine's ffmpeg with args (without the program's name):
// remuxing, PCM audio and RGBA video (<zebkit/media_tools.h>).
func FFmpeg(args ...string) (MediaCompletion, error) { return FFmpegWithProgress(nil, args...) }

// FFmpegWithProgress is FFmpeg with progress(packets, timeUs) called on the
// calling goroutine as it goes (timeUs is math.MinInt64 when unknown); false
// cancels, leaving no output.
func FFmpegWithProgress(progress func(packets, timeUs int64) bool, args ...string) (MediaCompletion, error) {
	argc, argv, done := toolArgs(args)
	defer done()
	var message *C.char
	var result C.int
	if progress == nil {
		result = C.zebkit_ffmpeg(argc, argv, nil, nil, &message)
	} else {
		h := handle(progress)
		defer cgo.Handle(h).Delete()
		result = C.zebkit_ffmpeg(argc, argv, C.zebkit_media_progress_func(C.zkc_progress), data(h), &message)
	}
	switch result {
	case 0:
		if message != nil {
			C.zebkit_free(unsafe.Pointer(message))
		}
		return MediaCompleted, nil
	case 1:
		if message != nil {
			C.zebkit_free(unsafe.Pointer(message))
		}
		return MediaCancelled, nil
	}
	return MediaCompleted, takeError(message, "ffmpeg failed")
}

func overwriteFlag(overwrite bool) string {
	if overwrite {
		return "-y"
	}
	return "-n"
}

// Remux copies source's streams into destination's container, without
// re-encoding (MP4, MOV, Matroska, WebM).
func Remux(source, destination string, overwrite bool) (MediaCompletion, error) {
	return FFmpeg(overwriteFlag(overwrite), "-i", source, "-c", "copy", "--", destination)
}

// ExtractAudio decodes source's audio to 16-bit PCM in a WAV file at rate
// and channels.
func ExtractAudio(source, destination string, rate, channels int, overwrite bool) (MediaCompletion, error) {
	return FFmpeg(overwriteFlag(overwrite), "-i", source, "-c:a", "pcm_s16le", "-ar", strconv.Itoa(rate),
		"-ac", strconv.Itoa(channels), "-f", "wav", "--", destination)
}

// ExtractVideo decodes count frames (count > 0) of source's video to packed
// RGBA in destination, scaled to width by height.
func ExtractVideo(source, destination string, width, height, count int, overwrite bool) (MediaCompletion, error) {
	return FFmpeg(overwriteFlag(overwrite), "-i", source, "-c:v", "rawvideo",
		"-s", strconv.Itoa(width)+"x"+strconv.Itoa(height), "-frames:v", strconv.Itoa(count), "--", destination)
}
