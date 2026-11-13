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
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
	"github.com/ikey4u/zebkit/bindings/golang/automation"
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

// act waits for a command that answers nothing.
func act(run func(func(error))) error {
	_, err := result(func(done func(struct{}, error)) { run(func(e error) { done(struct{}{}, e) }) })
	return err
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
	mux.HandleFunc("/download", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Disposition", "attachment; filename=report.txt")
		fmt.Fprint(w, "the report")
	})
	mux.HandleFunc("/api", func(w http.ResponseWriter, r *http.Request) {
		body, _ := io.ReadAll(r.Body)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"Method": r.Method, "Body": string(body), "Cookie": r.Header.Get("Cookie")})
	})
	// tests/engine/pages, beside the media.
	mux.Handle("/pages/", http.StripPrefix("/pages/", http.FileServer(http.Dir(filepath.Dir(mediaDir)))))
	mux.Handle("/media/", http.StripPrefix("/media/", http.FileServer(http.Dir(mediaDir))))
	go http.Serve(listener, mux)
	base := fmt.Sprintf("http://%s", listener.Addr())
	pages := base + "/pages"
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
	commands, err := automation.Commands()
	check(err == nil && len(commands) > 50 && strings.Contains(strings.Join(commands, " "), "Frame.click"), "Commands", len(commands))

	view := mustView(zebkit.NewView("", 1280, 800))
	page := automation.NewPage(view)
	check(automation.NewPage(view) == page, "NewPage: one Page a view")
	protocolEvents := []string{}
	page.OnProtocolEvent(func(event string, _ json.RawMessage) { protocolEvents = append(protocolEvents, event) })
	requests, finished := []string{}, []string{}
	page.On(automation.EventRequest, func(e *automation.PageEvent) { requests = append(requests, e.Request.URL()) })
	page.On(automation.EventRequestFinished, func(e *automation.PageEvent) {
		finished = append(finished, e.Request.Method()+" "+e.Request.URL())
	})
	response, err := result(func(done func(*automation.Response, error)) { page.Goto(pages+"/automation.html", done) })
	check(err == nil && response != nil && response.Status() == 200 && response.OK(), "Goto: the response", err)
	title, err := result(func(done func(string, error)) { page.Title(done) })
	check(err == nil && title == "Automation", "Title", title, err)
	check(wait(func() bool { return len(requests) > 0 && len(finished) > 0 }, 5) && strings.HasSuffix(requests[0], "/automation.html") &&
		strings.HasPrefix(finished[0], "GET "), "EventRequest and EventRequestFinished: the typed Request", requests, finished)
	if response != nil {
		body, err := result(func(done func(string, error)) { response.Text(done) })
		check(err == nil && strings.Contains(body, "<title>Automation</title>"), "Response.Text: the body", err)
		headers := response.Headers()
		check(len(headers) > 0 && headers[0].Name == strings.ToLower(headers[0].Name), "Response.Headers, in lower case", headers)
	}

	check(act(func(done func(error)) { page.GetByRole("button", automation.RoleOptions{Name: "Go"}).Click(done) }) == nil, "GetByRole(...).Click")
	text, err := result(func(done func(*string, error)) { page.Locator("#out").TextContent(done) })
	check(err == nil && text != nil && *text == "clicked 1", "Locator.TextContent", text, err)
	check(act(func(done func(error)) { automation.Expect(page.Locator("#out")).ToHaveText("clicked 1", done) }) == nil, "Expect.ToHaveText")
	err = act(func(done func(error)) { automation.Expect(page.Locator("#out")).Timeout(300).ToHaveText("nope", done) })
	check(err != nil && strings.Contains(err.Error(), "timed out"), "a failing expect is an error", err)
	check(act(func(done func(error)) { automation.Expect(page.Locator("#out")).Not().ToHaveText("nope", done) }) == nil, "Expect.Not")
	check(act(func(done func(error)) {
		automation.Expect(page.Locator("#out")).ToHaveText(regexp.MustCompile(`clicked \d`), done)
	}) == nil, "Expect.ToHaveText with a regular expression")
	check(act(func(done func(error)) { automation.ExpectPage(page).ToHaveTitle("Automation", done) }) == nil, "ExpectPage.ToHaveTitle")
	check(act(func(done func(error)) { automation.Expect(page.Locator(".item")).ToHaveCount(3, done) }) == nil, "Expect.ToHaveCount")
	check(act(func(done func(error)) { automation.Expect(page.Locator("#later")).ToBeDisabled(done) }) == nil, "Expect.ToBeDisabled")

	check(act(func(done func(error)) { page.GetByPlaceholder("Name", false).Fill("Grace", done) }) == nil, "GetByPlaceholder().Fill")
	check(act(func(done func(error)) { page.Locator("#name").Press("End", done) }) == nil &&
		act(func(done func(error)) { page.Locator("#name").PressSequentially(" H", done) }) == nil, "Press and PressSequentially")
	value, err := result(func(done func(string, error)) { page.Locator("#name").InputValue(done) })
	check(err == nil && value == "Grace H", "Locator.InputValue", value, err)
	count, err := result(func(done func(int, error)) { page.Locator(".item").Count(done) })
	check(err == nil && count == 3, "Locator.Count", count, err)
	contents, err := result(func(done func([]string, error)) { page.Locator(".item").AllTextContents(done) })
	check(err == nil && strings.Join(contents, ",") == "one,two,three", "AllTextContents", contents, err)
	text, err = result(func(done func(*string, error)) {
		page.Locator(".item").Filter(automation.Filter{HasText: "two"}).TextContent(done)
	})
	check(err == nil && text != nil && *text == "two", "Filter{HasText}", err)
	text, err = result(func(done func(*string, error)) { page.Locator(".item").Last().TextContent(done) })
	check(err == nil && text != nil && *text == "three", "Last", err)
	attribute, err := result(func(done func(*string, error)) { page.Locator("#name").GetAttribute("title", done) })
	check(err == nil && attribute == nil, "GetAttribute of none: nil", attribute, err)
	check(act(func(done func(error)) { page.GetByText("I agree", false).Check(done) }) == nil, "GetByText().Check through its label")
	checked, err := result(func(done func(bool, error)) { page.Locator("#agree").IsChecked(done) })
	check(err == nil && checked, "IsChecked", err)
	selected, err := result(func(done func([]string, error)) {
		page.Locator("#color").SelectOption([]automation.SelectOption{{Label: "Blue"}}, done)
	})
	check(err == nil && len(selected) == 1 && selected[0] == "b", "SelectOption{Label}", selected, err)

	deep := page.FrameLocator("#inner").Locator("#deep")
	check(act(deep.Click) == nil, "FrameLocator().Locator().Click")
	text, err = result(func(done func(*string, error)) { deep.TextContent(done) })
	check(err == nil && text != nil && *text == "pressed", "a frame locator's element", err)
	frames, err := result(func(done func([]*automation.Frame, error)) { page.Frames(done) })
	check(err == nil && len(frames) == 2 && frames[0].ID() == "" && frames[1].ID() != "", "Page.Frames", len(frames), err)

	handle, err := result(func(done func(*automation.ElementHandle, error)) { page.QuerySelector("#go", done) })
	check(err == nil && handle != nil, "QuerySelector: an ElementHandle", err)
	if handle != nil {
		text, err = result(func(done func(*string, error)) { handle.TextContent(done) })
		check(err == nil && text != nil && *text == "Go", "ElementHandle.TextContent", err)
		box, err := result(func(done func(*automation.BoundingBox, error)) { handle.BoundingBox(done) })
		check(err == nil && box != nil && box.Width > 0, "ElementHandle.BoundingBox", box, err)
		tag, err := raw(func(done func(json.RawMessage, error)) { page.Evaluate("(b) => b.tagName", handle, done) })
		check(err == nil && tag == `"BUTTON"`, "Evaluate with a handle as its argument", tag, err)
		check(act(handle.Dispose) == nil, "ElementHandle.Dispose")
	}
	object, err := result(func(done func(*automation.JSHandle, error)) {
		page.EvaluateHandle("() => ({a: 1, b: 'two'})", nil, done)
	})
	check(err == nil && object != nil && object.AsElement() == nil, "EvaluateHandle: a JSHandle", err)
	if object != nil {
		properties, err := result(func(done func([]automation.Property, error)) { object.GetProperties(done) })
		check(err == nil && len(properties) == 2 && properties[0].Name == "a", "JSHandle.GetProperties", properties, err)
		whole, err := raw(func(done func(json.RawMessage, error)) { object.JSONValue(done) })
		check(err == nil && whole == `{"a":1,"b":"two"}`, "JSHandle.JSONValue", whole, err)
	}
	answer, err := raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("(a) => a.x * 2", map[string]int{"x": 21}, done)
	})
	check(err == nil && answer == "42", "Evaluate with an argument", answer, err)
	err = act(func(done func(error)) { page.Locator("#nothing").WithTimeout(300).Click(done) })
	check(automation.IsTimeout(err) && strings.Contains(err.Error(), "Timeout 300ms exceeded"), "a timeout: IsTimeout", err)
	_, err = raw(func(done func(json.RawMessage, error)) { zebkit.CallAutomation(view, "Frame.nonsense", nil, done) })
	check(err != nil, "an unknown command is an error", err)

	// Events: the console, through On and WaitForEvent.
	consoles := []string{}
	consoleID := page.On(automation.EventConsole, func(e *automation.PageEvent) { consoles = append(consoles, e.Console.Text()) })
	var waited *automation.PageEvent
	page.WaitForEvent(automation.EventConsole, func(e *automation.PageEvent) bool { return e.Console.Text() == "second" },
		func(e *automation.PageEvent) { waited = e })
	_, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => { console.log('first'); console.log('second') }", nil, done)
	})
	check(wait(func() bool { return waited != nil && len(consoles) == 2 }, 5) && consoles[0] == "first" &&
		waited.Console.Type() == "log", "On(EventConsole) and WaitForEvent", consoles)
	page.RemoveListener(consoleID)
	_, _ = raw(func(done func(json.RawMessage, error)) { page.Evaluate("() => console.log('third')", nil, done) })
	wait(func() bool { return false }, 0.3)
	check(len(consoles) == 2, "RemoveListener: no more events", consoles)
	check(strings.Contains(strings.Join(protocolEvents, " "), "BrowserContext.console"), "OnProtocolEvent: every event")

	// Dialogs: with a listener held for it; without one dismissed.
	seen := []string{}
	dialogID := page.On(automation.EventDialog, func(e *automation.PageEvent) {
		seen = append(seen, e.Dialog.Type()+" "+e.Dialog.Message())
		if e.Dialog.Type() == "prompt" {
			e.Dialog.AcceptPrompt("Ada", nil)
		} else {
			e.Dialog.Accept(nil)
		}
	})
	answer, err = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => new Promise(r => setTimeout(() => r(confirm('Proceed?')), 0))", nil, done)
	})
	check(err == nil && answer == "true" && len(seen) == 1 && seen[0] == "confirm Proceed?", "On(EventDialog) and Accept", answer, seen)
	answer, err = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => new Promise(r => setTimeout(() => r(prompt('Name?', 'nobody')), 0))", nil, done)
	})
	check(err == nil && answer == `"Ada"`, "Dialog.AcceptPrompt", answer, err)
	page.RemoveListener(dialogID)
	answer, err = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => new Promise(r => setTimeout(() => r(confirm('Again?')), 0))", nil, done)
	})
	check(err == nil && answer == "false" && len(seen) == 2, "without a dialog listener a dialog is dismissed", answer, seen)
	var waitedDialog *automation.PageEvent
	page.WaitForEvent(automation.EventDialog, nil, func(e *automation.PageEvent) { waitedDialog = e; e.Dialog.Accept(nil) })
	_, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => new Promise(r => setTimeout(() => { alert('one'); r(1) }, 0))", nil, done)
	})
	check(waitedDialog != nil && waitedDialog.Dialog.Message() == "one", "WaitForEvent(EventDialog)")
	answer, err = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => new Promise(r => setTimeout(() => { alert('two'); r('after') }, 0))", nil, done)
	})
	held, _ := view.Dialogs()
	check(err == nil && answer == `"after"` && len(held) == 0, "its listener gone, the next dialog is not held", answer, held, err)

	// Routes: fulfil, abort, continue, a regular expression, Unroute.
	check(act(func(done func(error)) {
		page.Route("**/data.json", func(route *automation.Route, request *automation.Request) {
			route.Fulfill(automation.FulfillOptions{JSON: map[string]string{"from": "route"}}, nil)
		}, done)
	}) == nil, "Route")
	got, err := raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => fetch('data.json').then(r => r.json())", nil, done)
	})
	check(err == nil && got == `{"from":"route"}`, "Route.Fulfill", got, err)
	failedRequests := []string{}
	page.On(automation.EventRequestFailed, func(e *automation.PageEvent) { failedRequests = append(failedRequests, e.Request.Failure()) })
	_ = act(func(done func(error)) {
		page.Route("**/blocked", func(route *automation.Route, _ *automation.Request) { route.Abort("", nil) }, done)
	})
	got, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => fetch('blocked').then(() => 'loaded', () => 'failed')", nil, done)
	})
	check(got == `"failed"` && wait(func() bool { return len(failedRequests) > 0 }, 5), "Route.Abort, and EventRequestFailed", got, failedRequests)
	var seenMethod string
	_ = act(func(done func(error)) {
		page.RouteRegex(regexp.MustCompile(`(?i)HELLO\.txt$`), func(route *automation.Route, request *automation.Request) {
			seenMethod = request.Method()
			route.Continue(automation.ContinueOptions{}, nil)
		}, done)
	})
	got, err = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => fetch('hello.txt').then(r => r.text())", nil, done)
	})
	check(err == nil && got == `"hello\n"` && seenMethod == "GET", "RouteRegex and Route.Continue", got, seenMethod, err)
	_ = act(func(done func(error)) { page.Unroute("**/data.json", done) })
	got, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => fetch('data.json').then(r => r.status)", nil, done)
	})
	check(got == "404", "Unroute", got)

	// Exposed functions.
	check(act(func(done func(error)) {
		page.ExposeFunction("add", func(args []json.RawMessage) (any, error) {
			var a, b int
			if len(args) != 2 || json.Unmarshal(args[0], &a) != nil || json.Unmarshal(args[1], &b) != nil {
				return nil, errors.New("two numbers")
			}
			return a + b, nil
		}, done)
	}) == nil, "ExposeFunction")
	total, err := raw(func(done func(json.RawMessage, error)) { page.Evaluate("() => add(20, 22)", nil, done) })
	check(err == nil && total == "42", "an exposed function's answer", total, err)
	rejected, _ := raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => add('x').catch(e => e.message)", nil, done)
	})
	check(strings.Contains(rejected, "two numbers"), "an exposed function's error rejects", rejected)
	_ = act(func(done func(error)) {
		page.ExposeBinding("later", func(call *automation.BindingCall) {
			zebkit.PostDelayed(func() { call.Resolve("later " + call.Frame()) }, 20)
		}, done)
	})
	got, err = raw(func(done func(json.RawMessage, error)) { page.Evaluate("() => later()", nil, done) })
	check(strings.HasPrefix(got, `"later`), "ExposeBinding answered later", got, err)

	// The keyboard and the mouse.
	_ = act(func(done func(error)) { page.Locator("#name").Fill("", done) })
	_ = act(func(done func(error)) { page.Locator("#name").Focus(done) })
	keyboard := page.Keyboard()
	check(act(func(done func(error)) { keyboard.Type("ab", 0, done) }) == nil &&
		act(func(done func(error)) { keyboard.Press("Shift+KeyC", 0, done) }) == nil &&
		act(func(done func(error)) { keyboard.InsertText("!", done) }) == nil, "Keyboard.Type, Press, InsertText")
	value, _ = result(func(done func(string, error)) { page.Locator("#name").InputValue(done) })
	check(value == "abC!", "the keyboard's text", value)
	box, err := result(func(done func(*automation.BoundingBox, error)) { page.Locator("#go").BoundingBox(done) })
	check(err == nil && box != nil, "Locator.BoundingBox", err)
	if box != nil {
		mouse := page.Mouse()
		check(act(func(done func(error)) { mouse.Move(box.X+box.Width/2, box.Y+box.Height/2, 2, done) }) == nil &&
			act(func(done func(error)) { mouse.Click(box.X+box.Width/2, box.Y+box.Height/2, nil, done) }) == nil, "Mouse.Move and Click")
		text, _ = result(func(done func(*string, error)) { page.Locator("#out").TextContent(done) })
		check(text != nil && *text == "clicked 2", "the mouse's click", text)
		check(act(func(done func(error)) { mouse.Wheel(0, 40, done) }) == nil, "Mouse.Wheel")
	}

	// The clock.
	check(act(func(done func(error)) { page.Clock().Install(0, done) }) == nil &&
		act(func(done func(error)) { page.Clock().SetFixedTime(5000, done) }) == nil, "Clock.Install and SetFixedTime")
	now, _ := raw(func(done func(json.RawMessage, error)) { page.Evaluate("() => Date.now()", nil, done) })
	check(now == "5000", "the clock's time", now)

	// Screenshots and the ARIA snapshot.
	png, err := result(func(done func([]byte, error)) { page.Screenshot(nil, done) })
	check(err == nil && len(png) > 8 && string(png[1:4]) == "PNG", "Page.Screenshot", err)
	shot := filepath.Join(scratch, "shot.jpg")
	jpeg, err := result(func(done func([]byte, error)) {
		page.Screenshot(map[string]any{"type": "jpeg", "quality": 60, "path": shot}, done)
	})
	onDisk, _ := os.ReadFile(shot)
	check(err == nil && len(jpeg) > 3 && jpeg[0] == 0xff && jpeg[1] == 0xd8 && len(onDisk) == len(jpeg), "Screenshot to a JPEG file", err)
	pixels, err := result(func(done func(*zebkit.ScreenshotPixels, error)) { page.ScreenshotPixels(nil, done) })
	check(err == nil && pixels.Width == 1280 && len(pixels.Pixels) == pixels.Height*pixels.Stride, "Page.ScreenshotPixels", err)
	element, err := result(func(done func(*zebkit.ScreenshotPixels, error)) { page.Locator("#go").ScreenshotPixels(nil, done) })
	check(err == nil && element.Width > 0 && element.Width < 200, "Locator.ScreenshotPixels: the element's", err)
	snapshot, err := result(func(done func(string, error)) { page.Locator("ul").AriaSnapshot(done) })
	check(err == nil && strings.Contains(snapshot, "- listitem: one"), "AriaSnapshot", snapshot, err)
	check(act(func(done func(error)) {
		automation.Expect(page.Locator("ul")).ToMatchAriaSnapshot("- list:\n  - listitem: one\n  - listitem: two\n  - listitem: three", done)
	}) == nil, "ToMatchAriaSnapshot")

	// An init script, and its Disposable.
	disposable, err := result(func(done func(*automation.Disposable, error)) { page.AddInitScript("window.fromInit = 7", done) })
	check(err == nil && disposable != nil && disposable.Registration() != "", "AddInitScript: a Disposable", err)
	_, _ = result(func(done func(*automation.Response, error)) { page.Reload(nil, done) })
	got, _ = raw(func(done func(json.RawMessage, error)) { page.Evaluate("window.fromInit", nil, done) })
	check(got == "7", "the init script runs in the next document", got)
	if disposable != nil {
		check(act(disposable.Dispose) == nil, "Disposable.Dispose")
		_, _ = result(func(done func(*automation.Response, error)) { page.Reload(nil, done) })
		got, _ = raw(func(done func(json.RawMessage, error)) { page.Evaluate("typeof window.fromInit", nil, done) })
		check(got == `"undefined"`, "a disposed init script runs no more", got)
	}

	// File choosers: one listener answers; with none, the chooser is cancelled.
	upload := filepath.Join(scratch, "upload.txt")
	_ = os.WriteFile(upload, []byte("upload"), 0o644)
	var chooser *automation.FileChooser
	chooserID := page.On(automation.EventFileChooser, func(e *automation.PageEvent) {
		chooser = e.FileChooser
		e.FileChooser.SetFiles([]string{upload}, nil)
	})
	_ = act(func(done func(error)) { page.Locator("#upload").Click(done) })
	files := ""
	wait(func() bool {
		files = evaluate(view, "Array.from(document.getElementById('upload').files).map(f => f.name).join()")
		return files == `"upload.txt"`
	}, 5)
	check(chooser != nil && chooser.IsMultiple() && files == `"upload.txt"`, "On(EventFileChooser) and SetFiles", files)
	page.RemoveListener(chooserID)
	_ = act(func(done func(error)) { page.Locator("#upload").Click(done) })
	wait(func() bool { return false }, 0.3)
	check(evaluate(view, "document.getElementById('upload').files.length") == "1", "without a listener, a chooser changes nothing")
	choosers := func() int { return strings.Count(strings.Join(protocolEvents, " "), "Page.fileChooser") }
	before := choosers()
	_ = act(func(done func(error)) { page.Locator("#upload").Click(done) })
	wait(func() bool { return false }, 0.3)
	check(choosers() == before, "its last listener gone, file choosers are no longer held (enabled:false)", before, choosers())
	_ = act(func(done func(error)) { page.Locator("#upload").SetInputFiles([]string{upload, upload}, done) })
	check(evaluate(view, "document.getElementById('upload').files.length") == "2", "Locator.SetInputFiles")

	// A download.
	var download *automation.Download
	page.WaitForEvent(automation.EventDownload, nil, func(e *automation.PageEvent) { download = e.Download })
	_, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("(u) => { const a = document.createElement('a'); a.href = u; document.body.append(a); a.click(); }", base+"/download", done)
	})
	check(wait(func() bool { return download != nil }, 10), "EventDownload")
	if download != nil {
		check(download.SuggestedFilename() == "report.txt" && strings.HasSuffix(download.URL(), "/download"), "Download: its name and URL",
			download.SuggestedFilename())
		path, err := result(func(done func(*string, error)) { download.Path(done) })
		saved, _ := os.ReadFile(func() string {
			if path != nil {
				return *path
			}
			return ""
		}())
		check(err == nil && path != nil && string(saved) == "the report", "Download.Path, once finished", err)
		copied := filepath.Join(scratch, "report.txt")
		check(act(func(done func(error)) { download.SaveAs(copied, done) }) == nil, "Download.SaveAs")
		failure, err := result(func(done func(*string, error)) { download.Failure(done) })
		check(err == nil && failure == nil, "Download.Failure: none", failure, err)
		check(act(download.Delete) == nil, "Download.Delete")
	}

	// A worker.
	var worker *automation.Worker
	page.WaitForEvent(automation.EventWorker, nil, func(e *automation.PageEvent) { worker = e.Worker })
	_, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => { window.w = new Worker(URL.createObjectURL(new Blob(['self.x = 7']))) }", nil, done)
	})
	check(wait(func() bool { return worker != nil }, 10), "EventWorker")
	if worker != nil {
		got, err = raw(func(done func(json.RawMessage, error)) { worker.Evaluate("self.x * 6", nil, done) })
		check(err == nil && got == "42" && len(page.Workers()) == 1, "Worker.Evaluate, Page.Workers", got, err)
		closed := false
		worker.OnClose(func(*automation.Worker) { closed = true })
		_, _ = raw(func(done func(json.RawMessage, error)) { page.Evaluate("() => window.w.terminate()", nil, done) })
		check(wait(func() bool { return closed }, 10) && len(page.Workers()) == 0, "Worker.OnClose")
	}

	// Tracing.
	check(act(func(done func(error)) { page.Tracing().Start(automation.TracingOptions{Screenshots: true}, done) }) == nil, "Tracing.Start")
	_ = act(func(done func(error)) { page.Locator("#go").Click(done) })
	zip := filepath.Join(scratch, "trace.zip")
	trace, err := result(func(done func(string, error)) { page.Tracing().Stop(zip, done) })
	head := make([]byte, 2)
	if f, openErr := os.Open(trace); openErr == nil {
		_, _ = f.Read(head)
		f.Close()
	}
	check(err == nil && string(head) == "PK", "Tracing.Stop writes the zip", trace, err)

	// The API request context, with the page's cookies.
	check(act(func(done func(error)) {
		page.AddCookies([]automation.Cookie{{Name: "jar", Value: "full", URL: base + "/"}}, done)
	}) == nil, "Page.AddCookies")
	api, err := result(func(done func(*automation.APIResponse, error)) { page.Request().Get(base+"/api", done) })
	check(err == nil && api.Status() == 200 && api.OK() && strings.Contains(api.Text(), `"Cookie":"jar=full"`), "APIRequestContext.Get, with the cookies",
		err, func() string {
			if api != nil {
				return api.Text()
			}
			return ""
		}())
	api, err = result(func(done func(*automation.APIResponse, error)) {
		page.Request().Post(base+"/api", []byte("data"), done)
	})
	var echoed struct{ Method, Body string }
	check(err == nil && api.JSON(&echoed) == nil && echoed.Method == "POST" && echoed.Body == "data", "APIRequestContext.Post", err, echoed)
	cookies, err := result(func(done func([]automation.Cookie, error)) { page.Cookies([]string{base + "/"}, done) })
	check(err == nil && len(cookies) == 1 && cookies[0].Name == "jar" && cookies[0].Value == "full", "Page.Cookies", cookies, err)
	check(act(func(done func(error)) { page.GrantPermissions([]string{"geolocation"}, base, done) }) == nil, "Page.GrantPermissions")
	state, _ := raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => navigator.permissions.query({name: 'geolocation'}).then(s => s.state)", nil, done)
	})
	check(state == `"granted"`, "a granted permission", state)
	check(act(page.ClearPermissions) == nil, "Page.ClearPermissions")
	state, _ = raw(func(done func(json.RawMessage, error)) {
		page.Evaluate("() => navigator.permissions.query({name: 'geolocation'}).then(s => s.state)", nil, done)
	})
	check(state == `"prompt"`, "permissions cleared", state)
	check(act(func(done func(error)) { page.ClearCookies("", "", "", done) }) == nil, "Page.ClearCookies")

	// A context of its own: cookies and permissions its own, its pages.
	browserContext, err := result(func(done func(*automation.BrowserContext, error)) {
		automation.NewContext(map[string]any{"viewport": map[string]int{"width": 640, "height": 480}, "userAgent": "zebkit-go-context"}, done)
	})
	check(err == nil && browserContext != nil && browserContext.ID() != "", "NewContext", err)
	if browserContext != nil {
		other, err := result(func(done func(*automation.Page, error)) { browserContext.NewPage(done) })
		check(err == nil && other != nil && len(browserContext.Pages()) == 1, "BrowserContext.NewPage", err)
		if other != nil {
			agent, _ := raw(func(done func(json.RawMessage, error)) { other.Evaluate("() => navigator.userAgent", nil, done) })
			check(agent == `"zebkit-go-context"`, "the context's user agent", agent)
			_ = act(func(done func(error)) {
				browserContext.AddCookies([]automation.Cookie{{Name: "only", Value: "x", URL: base + "/"}}, done)
			})
			theirs, _ := result(func(done func([]automation.Cookie, error)) { browserContext.Cookies([]string{base + "/"}, done) })
			mine, _ := result(func(done func([]automation.Cookie, error)) { page.Cookies([]string{base + "/"}, done) })
			check(len(theirs) == 1 && theirs[0].Name == "only" && len(mine) == 0, "a context's cookies are its own", theirs, mine)
			check(act(func(done func(error)) { browserContext.GrantPermissions([]string{"notifications"}, base, done) }) == nil &&
				act(browserContext.ClearPermissions) == nil, "BrowserContext.GrantPermissions and ClearPermissions")
			_, err = result(func(done func(*automation.Response, error)) { other.Goto(pages+"/automation.html", done) })
			check(err == nil, "a context's page loads", err)
			check(act(browserContext.Close) == nil, "BrowserContext.Close")
			check(wait(func() bool { return other.View().IsClosed() }, 10), "a context's page closes with it")
			_, err = result(func(done func(*automation.Response, error)) { other.Goto(pages+"/automation.html", done) })
			check(err == zebkit.ErrClosed, "a closed page's commands answer ErrClosed", err)
		}
	}

	pageID := view.AutomationPageID()
	first, err1 := zebkit.ViewByPageID(pageID)
	check(pageID != "" && err1 == nil && first == view, "ViewByPageID gives the same View", err1)
	_, err = zebkit.ViewByPageID("p-none")
	check(err != nil, "ViewByPageID of no page is an error")

	endpoint, err := result(func(done func(string, error)) { automation.Listen(0, done) })
	check(err == nil && strings.HasPrefix(endpoint, "ws://127.0.0.1:"), "Listen", err)

	// The page for what follows.
	_, err = result(func(done func(*automation.Response, error)) { page.Goto(base+"/form", done) })
	check(err == nil && view.Title() == "Form", "Goto the form", err)
	page.SetViewportSize(640, 480, nil)

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

	// Find in page, as Chrome's find bar finds.
	finding := mustView(zebkit.NewView("data:text/html,<title>Find</title><p>apple one</p><p>Apple two</p><p>an apple three</p>", 400, 300))
	// Loaded and laid out (innerText lays out), as a find bar finds what is shown.
	wait(func() bool {
		return evaluate(finding, "document.readyState == 'complete' && document.body.innerText.includes('apple three')") == "true"
	}, 10)
	results := []zebkit.FindResult{}
	finding.OnFind(func(r zebkit.FindResult) { results = append(results, r) })
	final := func(request int) *zebkit.FindResult {
		var found *zebkit.FindResult
		wait(func() bool {
			for i := range results {
				if r := results[i]; r.RequestID == request && r.Final {
					copied := results[i]
					found = &copied
				}
			}
			return found != nil
		}, 10)
		return found
	}
	found, err := finding.Find("apple", 0)
	r := final(found)
	check(err == nil && found > 0 && r != nil && r.Matches == 3 && r.ActiveMatch == 1 && r.Text == "apple" && r.Rect != nil,
		"Find and OnFind: every match, the first active", found, err, r)
	next, err := finding.Find("apple", 0)
	r = final(next)
	check(err == nil && next > found && r != nil && r.ActiveMatch == 2, "Find again: the next match", r)
	back, _ := finding.Find("apple", zebkit.FindBackward)
	r = final(back)
	check(r != nil && r.ActiveMatch == 1, "FindBackward: the previous one", r)
	cased, _ := finding.Find("Apple", zebkit.FindMatchCase)
	r = final(cased)
	check(r != nil && r.Matches == 1, "FindMatchCase", r)
	counted, _ := finding.Find("apple", zebkit.FindHighlightOnly)
	r = final(counted)
	check(r != nil && r.Matches == 3 && r.ActiveMatch == 0, "FindHighlightOnly: counted, none active", r)
	last, err := finding.FindResult()
	check(err == nil && last.Text == "apple" && last.Matches == 3, "FindResult: the search on", last, err)
	finding.StopFinding(zebkit.FindStopClear)
	check(wait(func() bool { return len(results) > 0 && results[len(results)-1].Stopped }, 10), "StopFinding is told", results[len(results)-1])
	last, err = finding.FindResult()
	check(err == nil && last.Stopped && last.Matches == 0, "FindResult: none on", last, err)
	none, err := finding.Find("", 0)
	check(none == 0 && err == nil, "Find of empty text stops, and is no request", none, err)
	_, err = finding.Find("app\x00le", 0)
	check(errors.Is(err, zebkit.ErrRefused), "Find of text with a zero byte is refused", err)
	finding.OnFind(nil)
	finding.Close()
	_, err = finding.Find("apple", 0)
	_, resultErr := finding.FindResult()
	check(err == zebkit.ErrClosed && resultErr == zebkit.ErrClosed, "Find and FindResult of a closed view: ErrClosed", err, resultErr)

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
	_, err = result(func(done func(*automation.Response, error)) { page.Goto(base+"/video", done) })
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
