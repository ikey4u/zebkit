// The Go binding's checks of Playwright's API inside the engine, off-screen
// views, what a page asks of its window, and media. Exit 0 when every check
// passes.
//
//	go run ./internal/check_automation --no-sandbox --headless
//
// ZEBKIT_MEDIA_DIR names the directory of the test clips (default
// ../../tests/engine/pages/media, from bindings/golang).
package main

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

var checks, failures int

func check(ok bool, what string, detail ...any) {
	checks++
	if ok {
		fmt.Println("ok   " + what)
		return
	}
	failures++
	fmt.Println("FAIL "+what, detail)
}

func wait(condition func() bool, seconds float64) bool {
	end := time.Now().Add(time.Duration(seconds * float64(time.Second)))
	for time.Now().Before(end) {
		zebkit.Pump()
		if condition() {
			return true
		}
		time.Sleep(5 * time.Millisecond)
	}
	return false
}

// result waits for a command's answer.
func result[T any](run func(func(T, error))) (T, error) {
	var value T
	var err error
	done := false
	run(func(v T, e error) { value, err, done = v, e, true })
	if !wait(func() bool { return done }, 20) {
		return value, fmt.Errorf("no answer")
	}
	return value, err
}

// raw waits for a command's JSON answer, as text.
func raw(run func(func(json.RawMessage, error))) (string, error) {
	value, err := result(run)
	return string(value), err
}

func evaluate(view *zebkit.View, script string) string {
	value, err := raw(func(done func(json.RawMessage, error)) { view.Evaluate(script, done) })
	if err != nil {
		return "error: " + err.Error()
	}
	return value
}

func mustView(view *zebkit.View, err error) *zebkit.View {
	if err != nil {
		fmt.Println("FAIL a view:", err)
		exit(1)
	}
	return view
}

// scratch is the checks' directory for files, beside the profile
// (--user-data-dir) when there is one; exit removes it.
var scratch string

func exit(code int) {
	if scratch != "" {
		os.RemoveAll(scratch)
	}
	os.Exit(code)
}

const form = `<title>Form</title>
<input id=name placeholder="Your name"><button id=go onclick="out.textContent='Hello, '+document.getElementById('name').value">Go</button>
<p id=out></p><p id=clicks>0</p>
<div id=target style="width:200px;height:60px">drop here</div>
<script>
addEventListener('mousedown', () => clicks.textContent = +clicks.textContent + 1);
target.addEventListener('dragover', e => e.preventDefault());
target.addEventListener('drop', e => { e.preventDefault(); target.textContent = 'dropped ' + e.dataTransfer.getData('text/plain'); });
</script>`

func main() {
	mediaDir := os.Getenv("ZEBKIT_MEDIA_DIR")
	if mediaDir == "" {
		mediaDir = filepath.Join("..", "..", "tests", "engine", "pages", "media")
	}
	clip, _ := filepath.Abs(filepath.Join(mediaDir, "clip.mp4"))
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/form", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, form)
	})
	mux.HandleFunc("/video", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, `<title>Video</title><video id=v src="/media/clip.mp4" muted loop autoplay width=320></video>`)
	})
	mux.Handle("/media/", http.StripPrefix("/media/", http.FileServer(http.Dir(mediaDir))))
	go http.Serve(listener, mux)
	base := fmt.Sprintf("http://%s", listener.Addr())
	parent := ""
	for _, arg := range os.Args[1:] {
		if profile, ok := strings.CutPrefix(arg, "--user-data-dir="); ok {
			parent = filepath.Dir(profile)
		}
	}
	if scratch, err = os.MkdirTemp(parent, "zebkit-go-check-"); err != nil {
		fmt.Println("FAIL a scratch directory:", err)
		os.Exit(1)
	}

	if err := zebkit.Init(os.Args[1:]...); err != nil {
		fmt.Println("FAIL zebkit.Init:", err)
		exit(1)
	}

	// ---- Playwright's API ------------------------------------------------
	commands, err := zebkit.AutomationCommands()
	check(err == nil && len(commands) > 50 && strings.Contains(strings.Join(commands, " "), "Frame.click"), "AutomationCommands", len(commands))

	view := mustView(zebkit.NewView("", 640, 480))
	page := zebkit.NewPage(view)
	events := []string{}
	page.OnEvent(func(event string, _ json.RawMessage) { events = append(events, event) })
	_, err = raw(func(done func(json.RawMessage, error)) { page.Goto(base+"/form", done) })
	check(err == nil && view.Title() == "Form", "Page.Goto", err, view.Title())
	count, err := result(func(done func(int, error)) { page.GetByTestID("missing").Count(done) })
	check(err == nil && count == 0, "a locator's count: none", count, err)
	count, err = result(func(done func(int, error)) { page.Locator("p").Count(done) })
	check(err == nil && count == 2, "a locator's count: two", count, err)
	_, err = raw(func(done func(json.RawMessage, error)) { page.Locator("#name").Fill("Gopher", done) })
	check(err == nil, "Locator.Fill", err)
	_, err = raw(func(done func(json.RawMessage, error)) { page.GetByRole("button", "Go").Click(done) })
	check(err == nil, "GetByRole(...).Click", err)
	text, err := result(func(done func(string, error)) { page.Locator("#out").TextContent(done) })
	check(err == nil && text == "Hello, Gopher", "Locator.TextContent", text, err)
	value, err := result(func(done func(string, error)) { page.Locator("#name").InputValue(done) })
	check(err == nil && value == "Gopher", "Locator.InputValue", value, err)
	visible, err := result(func(done func(bool, error)) { page.Locator("#out").IsVisible(done) })
	check(err == nil && visible, "Locator.IsVisible", visible, err)
	_, err = raw(func(done func(json.RawMessage, error)) {
		page.Locator("#out").Expect("to.have.text", `{"expectedText":[{"string":"Hello, Gopher"}],"timeout":2000}`, done)
	})
	check(err == nil, "Locator.Expect", err)
	_, err = raw(func(done func(json.RawMessage, error)) {
		page.Locator("#nothing").Command("Frame.waitForSelector", map[string]any{"state": "visible", "timeout": 300}, done)
	})
	check(err != nil && strings.Contains(err.Error(), "Timeout"), "waiting for nothing fails at its deadline", err)
	answer, err := raw(func(done func(json.RawMessage, error)) {
		page.EvaluateFunction("(a) => a.x * 2", map[string]int{"x": 21}, done)
	})
	check(err == nil && strings.Contains(answer, "42"), "Page.EvaluateFunction with an argument", answer, err)
	_, err = raw(func(done func(json.RawMessage, error)) {
		page.EvaluateFunction("() => console.log('from the page')", nil, done)
	})
	check(err == nil && wait(func() bool { return strings.Contains(strings.Join(events, " "), "BrowserContext.console") }, 5),
		"OnAutomationEvent: the page's console", events)
	png := filepath.Join(scratch, "shot.png")
	_, err = raw(func(done func(json.RawMessage, error)) { page.Screenshot(png, done) })
	bytes, _ := os.ReadFile(png)
	check(err == nil && len(bytes) > 8 && string(bytes[1:4]) == "PNG", "Page.Screenshot to a file", err)
	shot, err := raw(func(done func(json.RawMessage, error)) { page.ScreenshotWith(map[string]string{"type": "png"}, done) })
	var encoded struct{ Binary string }
	_ = json.Unmarshal([]byte(shot), &encoded)
	decoded, _ := base64.StdEncoding.DecodeString(encoded.Binary)
	check(err == nil && len(decoded) > 8 && string(decoded[1:4]) == "PNG", "Page.ScreenshotWith, base64", err)
	var pixels *zebkit.ScreenshotPixels
	pixelsDone := false
	page.ScreenshotPixels(nil, func(p *zebkit.ScreenshotPixels, e error) { pixels, err, pixelsDone = p, e, true })
	check(wait(func() bool { return pixelsDone }, 20) && err == nil && pixels.Width > 0 &&
		len(pixels.Pixels) == pixels.Height*pixels.Stride, "Page.ScreenshotPixels")
	check(view.AutomationPageID() != "", "AutomationPageID")
	_, err = raw(func(done func(json.RawMessage, error)) { zebkit.CallAutomation(view, "Frame.nonsense", nil, done) })
	check(err != nil, "an unknown command is an error", err)
	pageID := view.AutomationPageID()
	first, err1 := zebkit.ViewByPageID(pageID)
	second, err2 := zebkit.ViewByPageID(pageID)
	check(err1 == nil && err2 == nil && first == view && second == view, "two lookups of one view give the same View", err1, err2)
	_, err = zebkit.ViewByPageID("p-none")
	check(err != nil, "ViewByPageID of no page is an error")

	var browserContext *zebkit.BrowserContext
	var contextPage *zebkit.Page
	contextDone := false
	zebkit.NewBrowserContext(`{"viewport":{"width":500,"height":400}}`, func(c *zebkit.BrowserContext, e error) {
		browserContext, err = c, e
		if e != nil {
			contextDone = true
			return
		}
		c.NewPage(func(p *zebkit.Page, e error) { contextPage, err, contextDone = p, e, true })
	})
	check(wait(func() bool { return contextDone }, 20) && err == nil && contextPage != nil, "BrowserContext.NewPage", err)
	if contextPage != nil {
		_, err = raw(func(done func(json.RawMessage, error)) { contextPage.Goto(base+"/form", done) })
		check(err == nil && contextPage.View.Title() == "Form", "a context's page loads", err)
		closedEvents := []string{}
		contextPage.OnEvent(func(event string, _ json.RawMessage) { closedEvents = append(closedEvents, event) })
		_, err = raw(func(done func(json.RawMessage, error)) { browserContext.Close(done) })
		check(err == nil, "BrowserContext.Close", err)
		check(wait(func() bool { return contextPage.View.IsClosed() }, 10), "a context's page closes with it, told to its View")
		wait(func() bool { return strings.Contains(strings.Join(closedEvents, " "), "Page.close") }, 5)
		check(strings.Contains(strings.Join(closedEvents, " "), "Page.close"), "the page's last event, Page.close, still comes", closedEvents)
		_, err = raw(func(done func(json.RawMessage, error)) { contextPage.Goto(base+"/form", done) })
		check(err == zebkit.ErrClosed, "a closed page's commands answer ErrClosed", err)
	}

	var endpoint string
	listened := false
	zebkit.ListenAutomation(0, func(url string, e error) { endpoint, err, listened = url, e, true })
	check(wait(func() bool { return listened }, 10) && err == nil && strings.HasPrefix(endpoint, "ws://127.0.0.1:"),
		"AutomationListen", err)

	// ---- What a page asks of its window -----------------------------------
	// Dialogs on a view of their own: Playwright's events on the other one
	// dismiss dialogs nobody subscribed to, as Playwright does.
	dialogView := mustView(zebkit.NewView(base+"/form", 400, 300))
	wait(func() bool { return dialogView.Title() == "Form" }, 10)
	dialogs := []*zebkit.Dialog{}
	closed := []string{}
	dialogView.SetDialogPolicy(zebkit.DialogPolicyHold)
	check(dialogView.DialogPolicy() == zebkit.DialogPolicyHold, "SetDialogPolicy and DialogPolicy")
	dialogView.OnDialog(func(d *zebkit.Dialog) { dialogs = append(dialogs, d) })
	dialogView.OnDialogClosed(func(id uint64, accepted bool, input string) {
		closed = append(closed, fmt.Sprintf("%d %v %s", id, accepted, input))
	})
	dialogView.Evaluate("setTimeout(() => window.answer = prompt('Your name?', 'nobody'), 0)", nil)
	check(wait(func() bool { return len(dialogs) == 1 }, 10) && dialogs[0].Type == zebkit.DialogPrompt &&
		dialogs[0].DefaultPrompt == "nobody" && dialogs[0].Held, "OnDialog: a held prompt", dialogs)
	open, err := dialogView.Dialogs()
	check(err == nil && len(open) == 1 && open[0].Message == "Your name?" && open[0].Type == zebkit.DialogPrompt, "Dialogs", open, err)
	if len(dialogs) == 1 {
		check(dialogView.RespondDialog(dialogs[0].ID, true, "Ada") == nil, "RespondDialog")
		check(dialogView.RespondDialog(dialogs[0].ID, true, "Ada") != nil, "RespondDialog of a dialog answered")
	}
	check(wait(func() bool { return len(closed) == 1 }, 10) && strings.HasSuffix(closed[0], "true Ada") &&
		evaluate(dialogView, "window.answer") == `"Ada"`, "OnDialogClosed, and the page's answer", closed)
	dialogView.OnDialog(nil)
	dialogView.SetDialogPolicy(zebkit.DialogPolicyDefault)

	dialogView.Close()

	var pdf []byte
	pdfDone := false
	view.PrintToPDF(map[string]bool{"printBackground": true}, func(b []byte, e error) { pdf, err, pdfDone = b, e, true })
	check(wait(func() bool { return pdfDone }, 30) && err == nil && strings.HasPrefix(string(pdf), "%PDF"), "PrintToPDF", err)

	check(view.DragEnter(`{"text": "a note"}`, 50, 120, zebkit.DragAll, 0) == nil, "DragEnter")
	var over, dropped zebkit.DragOperation = 99, 99
	view.DragOver(60, 125, zebkit.DragAll, 0, func(op zebkit.DragOperation) { over = op })
	wait(func() bool { return over != 99 }, 5)
	view.Drop(60, 125, 0, func(op zebkit.DragOperation) { dropped = op })
	check(wait(func() bool { return dropped != 99 }, 5) &&
		wait(func() bool { return strings.Contains(evaluate(view, "target.textContent"), "dropped a note") }, 5),
		"DragEnter, DragOver and Drop", over, dropped, evaluate(view, "target.textContent"))
	check(!view.IsDragging(), "IsDragging")

	closing := mustView(zebkit.NewView("data:text/html,<title>Closing</title>", 200, 100))
	gone := false
	closing.OnClose(func() { gone = true })
	wait(func() bool { return closing.Title() == "Closing" }, 10)
	closing.RequestClose()
	check(wait(func() bool { return gone }, 10), "RequestClose closes a page without beforeunload")

	// ---- Off-screen views --------------------------------------------------
	offscreen, err := zebkit.NewOffscreenView("", 320, 240, 1)
	check(err == nil && offscreen.IsOffscreen(), "NewOffscreenView", err)
	if err == nil {
		frames := 0
		var first *zebkit.OffscreenFrame
		offscreen.OnFrame(func(f *zebkit.OffscreenFrame) {
			frames++
			if first == nil {
				first = f
				return
			}
			f.Release()
		})
		offscreen.SetMaxFramesInFlight(2)
		offscreen.SetFrameRate(60)
		cursors := []string{}
		offscreen.OnCursor(func(cursor string, _ *zebkit.CursorImage) { cursors = append(cursors, cursor) })
		offscreen.Load(base + "/form")
		check(wait(func() bool { return first != nil }, 15), "OnFrame: a frame comes")
		if first != nil {
			pixels := first.CopyPixels()
			check(first.Width == 320 && first.Height == 240 && first.Layer == zebkit.LayerView &&
				(first.Kind != zebkit.FrameSharedMemory || len(pixels) >= first.Width*4) && len(first.Damage) > 0,
				"the frame: size, layer, pixels, damage", first.Width, first.Height, first.Kind, len(first.Damage))
			first.Release()
			first.Release()
		}
		wait(func() bool { return offscreen.Title() == "Form" }, 10)
		before := frames
		offscreen.RequestFrame()
		check(wait(func() bool { return frames > before }, 10), "RequestFrame")
		offscreen.SetFocused(true)
		offscreen.SendMouse(zebkit.MouseMove, 20, 10, zebkit.ButtonNone, 0, 0)
		offscreen.SendMouse(zebkit.MouseDown, 20, 10, zebkit.ButtonLeft, 1, 0)
		offscreen.SendMouse(zebkit.MouseUp, 20, 10, zebkit.ButtonLeft, 1, 0)
		check(wait(func() bool { return evaluate(offscreen, "clicks.textContent") == `"1"` }, 10),
			"SendMouse reaches the page", evaluate(offscreen, "clicks.textContent"))
		evaluate(offscreen, "document.getElementById('name').value = ''; document.getElementById('name').focus(); 1")
		offscreen.SendKey(zebkit.KeyEvent{Type: zebkit.KeyDown, KeyCode: 0x41, Code: "KeyA", Key: "a"})
		offscreen.SendKey(zebkit.KeyEvent{Type: zebkit.KeyChar, KeyCode: 0x41, Code: "KeyA", Key: "a", Text: "a", UnmodifiedText: "a"})
		offscreen.SendKey(zebkit.KeyEvent{Type: zebkit.KeyUp, KeyCode: 0x41, Code: "KeyA", Key: "a"})
		check(wait(func() bool { return evaluate(offscreen, "document.getElementById('name').value") == `"a"` }, 10), "SendKey types into the page",
			evaluate(offscreen, "document.getElementById('name').value"))
		offscreen.CommitIME("é")
		check(wait(func() bool { return evaluate(offscreen, "document.getElementById('name').value") == `"aé"` }, 10), "ImeCommit",
			evaluate(offscreen, "document.getElementById('name').value"))
		check(offscreen.SendKeyWithCommands(zebkit.KeyEvent{Type: zebkit.KeyDown, KeyCode: 0x24, Code: "Home", Key: "Home"},
			[]zebkit.EditCommand{{Name: "MoveToBeginningOfLine"}}) == nil, "SendKeyWithCommands")
		offscreen.SendWheel(100, 100, 0, -40, true, zebkit.WheelPhaseNone, 0)
		offscreen.SetSize(400, 300, 2)
		var resized *zebkit.OffscreenFrame
		offscreen.OnFrame(func(f *zebkit.OffscreenFrame) {
			if f.Width == 800 && resized == nil {
				resized = f
				return
			}
			f.Release()
		})
		offscreen.RequestFrame()
		check(wait(func() bool { return resized != nil }, 10), "SetSize: frames at the new size and scale")
		if resized != nil {
			check(resized.Height == 600 && resized.Scale == 2, "the resized frame", resized.Height, resized.Scale)
			resized.Release()
		}
		check(len(cursors) > 0, "OnCursor", cursors)
		offscreen.OnFrame(nil)
		offscreen.Close()
	}

	context, err := zebkit.NewContext()
	check(err == nil, "NewContext", err)
	inContext, err := context.NewOffscreenView("data:text/html,<title>In a context</title>", 200, 100, 1)
	check(err == nil && inContext.IsOffscreen() &&
		wait(func() bool { return inContext.Title() == "In a context" }, 10), "Context.NewOffscreenView", err)
	inContext.Close()
	context.Close()

	// ---- Media -------------------------------------------------------------
	probe, err := zebkit.MediaProbe(clip)
	check(err == nil && strings.Contains(string(probe), "streams"), "MediaProbe", err)
	probe, err = zebkit.FFprobe("-show_streams", "-of", "json", clip)
	check(err == nil && strings.Contains(string(probe), "codec_type"), "FFprobe", err)
	remuxed := filepath.Join(scratch, "clip.mkv")
	completion, err := zebkit.Remux(clip, remuxed, true)
	info, statErr := os.Stat(remuxed)
	check(err == nil && completion == zebkit.MediaCompleted && statErr == nil && info.Size() > 1000, "Remux", err)
	progressCalls := 0
	completion, err = zebkit.FFmpegWithProgress(func(int64, int64) bool { progressCalls++; return false },
		"-y", "-i", clip, "-c", "copy", filepath.Join(scratch, "cancelled.mkv"))
	_, statErr = os.Stat(filepath.Join(scratch, "cancelled.mkv"))
	check(err == nil && completion == zebkit.MediaCancelled && progressCalls > 0 && os.IsNotExist(statErr),
		"FFmpegWithProgress cancels, leaving nothing", err, completion)
	wav := filepath.Join(scratch, "clip.wav")
	_, err = zebkit.ExtractAudio(clip, wav, 16000, 1, true)
	check(err == nil, "ExtractAudio", err)
	rgba := filepath.Join(scratch, "clip.rgba")
	_, err = zebkit.ExtractVideo(clip, rgba, 32, 18, 2, true)
	info, statErr = os.Stat(rgba)
	check(err == nil && statErr == nil && info.Size() == 32*18*4*2, "ExtractVideo", err)
	videoStream := 0
	var mp4 map[string]any
	if probe, err := zebkit.MediaProbe(clip); err == nil {
		_ = json.Unmarshal(probe, &mp4)
	}
	if streams, ok := mp4["streams"].([]any); ok {
		for i, stream := range streams {
			if s, ok := stream.(map[string]any); ok && s["codec_type"] == "video" {
				videoStream = i
			}
		}
	}
	videoProgress := 0
	_, err = zebkit.ExtractVideoWith(clip, rgba, zebkit.VideoOptions{Width: 16, Height: 8, Frames: 1, Stream: &videoStream,
		Overwrite: true, Progress: func(int64, int64) bool { videoProgress++; return true }})
	info, statErr = os.Stat(rgba)
	check(err == nil && statErr == nil && info.Size() == 16*8*4 && videoProgress > 0, "ExtractVideoWith: a stream, a size, progress", err)
	pcm := filepath.Join(scratch, "clip.s16le")
	_, err = zebkit.ExtractAudioWith(clip, pcm, zebkit.AudioOptions{Raw: true, Channels: 2, Overwrite: true})
	info, statErr = os.Stat(pcm)
	check(err == nil && statErr == nil && info.Size() > 0 && info.Size()%4 == 0, "ExtractAudioWith raw s16le", err)
	_, err = zebkit.RemuxWith(clip, filepath.Join(scratch, "clip.out"), zebkit.RemuxOptions{Streams: []int{videoStream}, Format: "matroska", Overwrite: true})
	check(err == nil, "RemuxWith one stream and a format", err)
	_, err = zebkit.RemuxWith(clip, remuxed, zebkit.RemuxOptions{Streams: []int{}, Overwrite: true})
	check(err != nil, "RemuxWith refuses no streams")
	_, err = zebkit.FFmpeg("-i", clip, "-c", "copy")
	check(err != nil, "FFmpeg refuses what it cannot do", err)

	zebkit.KeepMediaHistory(true)
	_, err = raw(func(done func(json.RawMessage, error)) { page.Goto(base+"/video", done) })
	check(err == nil, "a page with a video", err)
	var media []map[string]any
	wait(func() bool {
		list, _ := result(func(done func(json.RawMessage, error)) { view.MediaList(done) })
		media = nil
		_ = json.Unmarshal(list, &media)
		return len(media) == 1 && media[0]["playing"] == true
	}, 20)
	check(len(media) == 1 && media[0]["capturable"] == true, "MediaList", media)
	at, err := raw(func(done func(json.RawMessage, error)) { view.MediaAt(20, 20, done) })
	check(err == nil && strings.Contains(at, "\"id\""), "MediaAt", at, err)
	if len(media) == 1 {
		id := fmt.Sprint(media[0]["id"])
		out := filepath.Join(scratch, "captured.mp4")
		capture, err := view.StartMediaCapture(id, out, nil)
		check(err == nil, "MediaCaptureStart", err)
		if capture != nil {
			progressed, finished := false, ""
			capture.OnProgress(func(json.RawMessage) { progressed = true })
			capture.OnDone(func(state json.RawMessage) { finished = string(state) })
			wait(func() bool { return progressed }, 10)
			wait(func() bool { return false }, 1.5)
			status, err := capture.Status()
			check(err == nil && strings.Contains(string(status), "\"state\""), "MediaCapture.Status", string(status), err)
			capture.Stop()
			check(wait(func() bool { return finished != "" }, 20), "MediaCapture: OnProgress, Stop, OnDone", progressed, finished)
			info, statErr := os.Stat(out)
			check(statErr == nil && info.Size() > 1000, "the captured file", statErr)
			capture.Close()
			_, err = capture.Status()
			check(err == zebkit.ErrClosed, "a closed capture answers ErrClosed", err)
		}
		events := map[string]int{}
		tap, err := view.StartMediaTap(id, `{"packets": true, "video": true, "audio": false, "includeBuffered": true}`,
			func(state json.RawMessage, bytes []byte) {
				var event struct{ Type string }
				_ = json.Unmarshal(state, &event)
				events[event.Type]++
			})
		check(err == nil, "MediaTapStart", err)
		if tap != nil {
			check(wait(func() bool { return events["packet"] > 0 && events["video"] > 0 }, 15), "the tap's packets and video", events)
			tap.Stop()
			check(wait(func() bool { return events["done"] > 0 }, 15), "MediaTap.Stop, then done", events)
			tap.Close()
		}
	}

	view.Close()
	zebkit.Shutdown()
	fmt.Printf("%d of %d checks passed\n", checks-failures, checks)
	if failures > 0 {
		exit(1)
	}
	exit(0)
}
