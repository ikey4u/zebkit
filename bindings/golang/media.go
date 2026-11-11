package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"strconv"
	"unsafe"
)

// The media a page plays, saved as it plays (<zebkit/media.h>), its packets,
// frames and sound as they play (<zebkit/media_tap.h>), and FFmpeg's tools
// inside the engine (<zebkit/media_tools.h>).

// KeepMediaHistory keeps what media elements played before a capture asks.
func KeepMediaHistory(keep bool) {
	onEngineThread()
	C.zebkit_media_keep_history(C.bool(keep))
}

// MediaList lists the page's <audio> and <video> elements as a JSON array
// (<zebkit/media.h> has its members); "id" is what a capture takes.
func (v *View) MediaList(done func(list json.RawMessage, err error)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	C.zebkit_view_media_list(raw, C.zebkit_result_func(C.zk_result), data(resultOnce(done)))
}

// MediaAt is the media element at (x, y), CSS pixels in the view, as JSON
// (null for none).
func (v *View) MediaAt(x, y float32, done func(media json.RawMessage, err error)) {
	raw, ok := v.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	C.zebkit_view_media_at(raw, C.float(x), C.float(y), C.zebkit_result_func(C.zk_result), data(resultOnce(done)))
}

// MediaCapture copies what a media element plays to a file while it plays.
type MediaCapture struct {
	capture        *C.zebkit_media_capture
	progress, done slot
}

// StartMediaCapture starts copying media id to path, with options, JSON (nil
// for none: format, wallClock, limits, ...).
func (v *View) StartMediaCapture(mediaID, path string, options any) (*MediaCapture, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	coptions, err := optionalJSON(options)
	if err != nil {
		return nil, err
	}
	defer free(coptions)
	cid, cpath := cString(mediaID), cString(path)
	defer free(cid)
	defer free(cpath)
	var message *C.char
	capture := C.zebkit_media_capture_start(raw, cid, cpath, coptions, &message)
	if capture == nil {
		return nil, takeError(message, "the capture did not start")
	}
	return &MediaCapture{capture: capture}, nil
}

// OnProgress calls function with the capture's state as JSON as it goes; nil
// stops.
func (c *MediaCapture) OnProgress(function func(state json.RawMessage)) {
	if c.capture == nil {
		return
	}
	if function == nil {
		C.zebkit_media_capture_on_progress(c.capture, nil, nil)
		c.progress.clear()
		return
	}
	h := c.progress.set(rawFunc(function))
	C.zebkit_media_capture_on_progress(c.capture, C.zebkit_media_capture_func(C.zk_capture_text), data(h))
}

// OnDone calls function once, with the final state as JSON, when the files
// are written; nil stops.
func (c *MediaCapture) OnDone(function func(state json.RawMessage)) {
	if c.capture == nil {
		return
	}
	if function == nil {
		C.zebkit_media_capture_on_done(c.capture, nil, nil)
		c.done.clear()
		return
	}
	h := c.done.set(rawFunc(function))
	C.zebkit_media_capture_on_done(c.capture, C.zebkit_media_capture_func(C.zk_capture_text), data(h))
}

// rawFunc adapts a function of JSON to the bridge's text.
func rawFunc(function func(json.RawMessage)) func(string) {
	return func(text string) { function(json.RawMessage(text)) }
}

// Stop stops the capture and writes its files; OnDone's function follows.
func (c *MediaCapture) Stop() {
	if c.capture != nil {
		C.zebkit_media_capture_stop(c.capture)
	}
}

// Status is the capture's state as JSON.
func (c *MediaCapture) Status() (json.RawMessage, error) {
	if c.capture == nil {
		return nil, ErrClosed
	}
	return takeRaw(C.zebkit_media_capture_status(c.capture), "no status")
}

// Close lets the capture go; one still running is stopped.
func (c *MediaCapture) Close() {
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

// StartMediaTap taps media id with options, JSON (packets, video, audio,
// maxFps, ...; <zebkit/media_tap.h>); function gets each event as JSON with
// its bytes, copied: audioConfig, videoConfig, packet, video, audio,
// timeline, progress, done.
func (v *View) StartMediaTap(mediaID string, options any, function func(event json.RawMessage, bytes []byte)) (*MediaTap, error) {
	raw, ok := v.raw()
	if !ok {
		return nil, ErrClosed
	}
	if function == nil {
		return nil, refused("a tap needs a function")
	}
	coptions, err := optionalJSON(options)
	if err != nil {
		return nil, err
	}
	defer free(coptions)
	cid := cString(mediaID)
	defer free(cid)
	tap := &MediaTap{}
	h := tap.callback.set(function)
	var message *C.char
	tap.tap = C.zebkit_media_tap_start(raw, cid, coptions, C.zebkit_media_tap_func(C.zk_tap), data(h), &message)
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

// Close stops the tap and lets it go: no event follows.
func (t *MediaTap) Close() {
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

// FFprobe runs the engine's ffprobe with args (without the program's name)
// and returns its JSON. No engine needs to run; any goroutine.
func FFprobe(args ...string) (json.RawMessage, error) {
	argc, argv, done := cStrings(args)
	defer done()
	var out, message *C.char
	if C.zebkit_ffprobe(argc, argv, &out, &message) != 0 {
		return nil, takeError(message, "ffprobe failed")
	}
	if message != nil {
		C.zebkit_free(unsafe.Pointer(message))
	}
	return takeRaw(out, "ffprobe said nothing")
}

// MediaProbe is ffprobe of path, with its format and streams, JSON.
func MediaProbe(path string) (json.RawMessage, error) {
	cpath := cString(path)
	defer free(cpath)
	var out, message *C.char
	if C.zebkit_media_probe(cpath, &out, &message) != 0 {
		return nil, takeError(message, "the probe failed")
	}
	if message != nil {
		C.zebkit_free(unsafe.Pointer(message))
	}
	return takeRaw(out, "the probe said nothing")
}

// FFmpeg runs the engine's ffmpeg with args (without the program's name):
// remuxing, PCM audio and RGBA video (<zebkit/media_tools.h>).
func FFmpeg(args ...string) (MediaCompletion, error) { return FFmpegWithProgress(nil, args...) }

// FFmpegWithProgress is FFmpeg with progress(packets, timeUs) called on the
// calling goroutine as it goes (timeUs is math.MinInt64 when unknown); false
// cancels, leaving no output.
func FFmpegWithProgress(progress func(packets, timeUs int64) bool, args ...string) (MediaCompletion, error) {
	argc, argv, done := cStrings(args)
	defer done()
	var message *C.char
	var result C.int
	if progress == nil {
		result = C.zebkit_ffmpeg(argc, argv, nil, nil, &message)
	} else {
		h := handle(progress)
		defer release(h)
		result = C.zebkit_ffmpeg(argc, argv, C.zebkit_media_progress_func(C.zk_progress), data(h), &message)
	}
	switch result {
	case 0, 1:
		if message != nil {
			C.zebkit_free(unsafe.Pointer(message))
		}
		if result == 1 {
			return MediaCancelled, nil
		}
		return MediaCompleted, nil
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
	return RemuxWith(source, destination, RemuxOptions{Overwrite: overwrite})
}

// ExtractAudio decodes source's audio to 16-bit PCM in a WAV file at rate
// and channels.
func ExtractAudio(source, destination string, rate, channels int, overwrite bool) (MediaCompletion, error) {
	return ExtractAudioWith(source, destination, AudioOptions{SampleRate: rate, Channels: channels, Overwrite: overwrite})
}

// ExtractVideo decodes count frames (count > 0) of source's video to packed
// RGBA in destination, scaled to width by height.
func ExtractVideo(source, destination string, width, height, count int, overwrite bool) (MediaCompletion, error) {
	return ExtractVideoWith(source, destination, VideoOptions{Width: width, Height: height, Frames: count, Overwrite: overwrite})
}

// RemuxOptions are what RemuxWith does beyond copying.
type RemuxOptions struct {
	// Streams are indices of the source's streams; nil for its audio and video.
	Streams []int
	// Format is the container's format ("mp4", "matroska", ...); "" takes it
	// from destination's extension.
	Format    string
	Overwrite bool
	// Progress is FFmpegWithProgress's; nil for none.
	Progress func(packets, timeUs int64) bool
}

// AudioOptions are what ExtractAudioWith makes.
type AudioOptions struct {
	SampleRate int  // 0 for 16000
	Channels   int  // 0 for 1
	Stream     *int // the source's stream index; nil for its audio
	Raw        bool // headerless s16le instead of WAV
	Overwrite  bool
	Progress   func(packets, timeUs int64) bool
}

// VideoOptions are what ExtractVideoWith makes.
type VideoOptions struct {
	Width, Height int  // the frames' size; 0 by 0 for the source's
	Frames        int  // at most this many; 0 for all
	Stream        *int // the source's stream index; nil for its video
	Overwrite     bool
	Progress      func(packets, timeUs int64) bool
}

func mediaStart(source string, overwrite bool) []string {
	return []string{overwriteFlag(overwrite), "-i", source}
}

func mediaRun(args []string, destination string, progress func(packets, timeUs int64) bool) (MediaCompletion, error) {
	return FFmpegWithProgress(progress, append(args, "--", destination)...)
}

// RemuxWith is Remux with its options.
func RemuxWith(source, destination string, options RemuxOptions) (MediaCompletion, error) {
	args := append(mediaStart(source, options.Overwrite), "-c", "copy")
	if options.Streams != nil {
		if len(options.Streams) == 0 {
			return MediaCompleted, refused("streams must not be empty")
		}
		for _, stream := range options.Streams {
			if stream < 0 {
				return MediaCompleted, refused("stream indices are not negative")
			}
			args = append(args, "-map", "0:"+strconv.Itoa(stream))
		}
	}
	if options.Format != "" {
		args = append(args, "-f", options.Format)
	}
	return mediaRun(args, destination, options.Progress)
}

// ExtractAudioWith is ExtractAudio with its options.
func ExtractAudioWith(source, destination string, options AudioOptions) (MediaCompletion, error) {
	rate, channels, format := options.SampleRate, options.Channels, "wav"
	if rate == 0 {
		rate = 16000
	}
	if channels == 0 {
		channels = 1
	}
	if options.Raw {
		format = "s16le"
	}
	args := append(mediaStart(source, options.Overwrite), "-c:a", "pcm_s16le", "-ar", strconv.Itoa(rate),
		"-ac", strconv.Itoa(channels), "-f", format)
	if options.Stream != nil {
		args = append(args, "-map", "0:"+strconv.Itoa(*options.Stream))
	}
	return mediaRun(args, destination, options.Progress)
}

// ExtractVideoWith is ExtractVideo with its options: packed RGBA frames, no
// headers (MediaProbe has the source's size).
func ExtractVideoWith(source, destination string, options VideoOptions) (MediaCompletion, error) {
	args := append(mediaStart(source, options.Overwrite), "-c:v", "rawvideo", "-f", "rawvideo")
	if options.Width != 0 || options.Height != 0 {
		args = append(args, "-s", strconv.Itoa(options.Width)+"x"+strconv.Itoa(options.Height))
	}
	if options.Frames != 0 {
		args = append(args, "-frames:v", strconv.Itoa(options.Frames))
	}
	if options.Stream != nil {
		args = append(args, "-map", "0:"+strconv.Itoa(*options.Stream))
	}
	return mediaRun(args, destination, options.Progress)
}
