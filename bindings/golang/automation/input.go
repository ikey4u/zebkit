package automation

import (
	"encoding/base64"
	"encoding/json"
)

// Keyboard is the page's keyboard.
type Keyboard struct{ state *state }

// Down presses key and holds it.
func (k *Keyboard) Down(key string, done func(error)) {
	run(k.state.view, "Page.keyboardDown", object{"key": key}, unit(done))
}

// Up lets key go.
func (k *Keyboard) Up(key string, done func(error)) {
	run(k.state.view, "Page.keyboardUp", object{"key": key}, unit(done))
}

// Press presses key ("Enter", "Control+A"); delay is milliseconds between
// down and up (0: none).
func (k *Keyboard) Press(key string, delay float64, done func(error)) {
	run(k.state.view, "Page.keyboardPress", object{"key": key, "delay": milliseconds(delay)}, unit(done))
}

// Type types text, a key at a time; delay is milliseconds between keys.
func (k *Keyboard) Type(text string, delay float64, done func(error)) {
	run(k.state.view, "Page.keyboardType", object{"text": text, "delay": milliseconds(delay)}, unit(done))
}

// InsertText inserts text as an input method would, without key events.
func (k *Keyboard) InsertText(text string, done func(error)) {
	run(k.state.view, "Page.keyboardInsertText", object{"text": text}, unit(done))
}

// Mouse is the page's mouse; options are button, clickCount, delay, JSON (nil
// for none).
type Mouse struct{ state *state }

// Move moves the mouse to (x, y), CSS pixels, in steps (0: one).
func (m *Mouse) Move(x, y float64, steps int, done func(error)) {
	params := object{"x": x, "y": y}
	if steps > 0 {
		params["steps"] = steps
	}
	run(m.state.view, "Page.mouseMove", params, unit(done))
}

func (m *Mouse) send(method string, opts any, members object, done func(error)) {
	params, err := with(opts, members)
	if err != nil {
		failedUnit(done, err)
		return
	}
	run(m.state.view, method, params, unit(done))
}

// Down presses a button.
func (m *Mouse) Down(options any, done func(error)) { m.send("Page.mouseDown", options, nil, done) }

// Up lets a button go.
func (m *Mouse) Up(options any, done func(error)) { m.send("Page.mouseUp", options, nil, done) }

// Click clicks at (x, y).
func (m *Mouse) Click(x, y float64, options any, done func(error)) {
	m.send("Page.mouseClick", options, object{"x": x, "y": y}, done)
}

// Dblclick double-clicks at (x, y).
func (m *Mouse) Dblclick(x, y float64, options any, done func(error)) {
	m.send("Page.mouseClick", options, object{"x": x, "y": y, "clickCount": 2}, done)
}

// Wheel scrolls by (deltaX, deltaY).
func (m *Mouse) Wheel(deltaX, deltaY float64, done func(error)) {
	run(m.state.view, "Page.mouseWheel", object{"deltaX": deltaX, "deltaY": deltaY}, unit(done))
}

// Touchscreen is the page's touch screen.
type Touchscreen struct{ state *state }

// Tap taps at (x, y).
func (t *Touchscreen) Tap(x, y float64, done func(error)) {
	run(t.state.view, "Page.touchscreenTap", object{"x": x, "y": y}, unit(done))
}

// Clock is the page's clock, as Playwright's. A time is milliseconds since
// the epoch (a number), a date as JavaScript reads one (a string) or a
// time.Time; ticks are milliseconds, a string ("01:30") or a time.Duration.
type Clock struct{ state *state }

func (c *Clock) send(method string, value any, ticks bool, done func(error)) {
	params := object{}
	if value != nil {
		var err error
		if params, err = clockParams(value, ticks); err != nil {
			failedUnit(done, err)
			return
		}
	}
	run(c.state.view, method, params, unit(done))
}

// Install replaces the page's timers and clock, from time (nil: now).
func (c *Clock) Install(time any, done func(error)) {
	c.send("BrowserContext.clockInstall", time, false, done)
}

// FastForward jumps ahead, firing the timers due at most once.
func (c *Clock) FastForward(ticks any, done func(error)) {
	c.send("BrowserContext.clockFastForward", ticks, true, done)
}

// RunFor runs the clock ahead, firing every timer due.
func (c *Clock) RunFor(ticks any, done func(error)) {
	c.send("BrowserContext.clockRunFor", ticks, true, done)
}

// PauseAt jumps to time and pauses there.
func (c *Clock) PauseAt(time any, done func(error)) {
	c.send("BrowserContext.clockPauseAt", time, false, done)
}

// Resume lets the paused clock run.
func (c *Clock) Resume(done func(error)) { c.send("BrowserContext.clockResume", nil, false, done) }

// SetFixedTime makes Date.now() answer time always.
func (c *Clock) SetFixedTime(time any, done func(error)) {
	c.send("BrowserContext.clockSetFixedTime", time, false, done)
}

// SetSystemTime sets the clock's time, firing no timers.
func (c *Clock) SetSystemTime(time any, done func(error)) {
	c.send("BrowserContext.clockSetSystemTime", time, false, done)
}

// Tracing is Playwright's traces (the zip its trace viewer opens) and HAR
// files of the page.
type Tracing struct{ state *state }

// TracingOptions are Tracing.Start's.
type TracingOptions struct {
	Name        string
	Title       string
	Screenshots bool
	Snapshots   bool
}

// Start starts a trace and its first chunk.
func (t *Tracing) Start(options TracingOptions, done func(error)) {
	view := t.state.view
	run(view, "Tracing.tracingStart", object{"name": optionalText(options.Name), "screenshots": options.Screenshots,
		"snapshots": options.Snapshots}, func(_ answer, err error) {
		if err != nil {
			failedUnit(done, err)
			return
		}
		run(view, "Tracing.tracingStartChunk", object{"title": optionalText(options.Title)}, unit(done))
	})
}

// StartChunk starts a chunk of the trace.
func (t *Tracing) StartChunk(title string, done func(error)) {
	run(t.state.view, "Tracing.tracingStartChunk", object{"title": optionalText(title)}, unit(done))
}

// StopChunk ends the chunk, written to path as a zip ("": discarded); done
// gets the path written ("" when discarded).
func (t *Tracing) StopChunk(path string, done func(string, error)) {
	params := object{"mode": "discard"}
	if path != "" {
		params = object{"mode": "archive", "path": absolute(path)}
	}
	run(t.state.view, "Tracing.tracingStopChunk", params, then(done, func(a answer) (string, error) {
		if path == "" {
			return "", nil
		}
		return a.str("path"), nil
	}))
}

// Stop ends the trace, its last chunk written to path ("": discarded).
func (t *Tracing) Stop(path string, done func(string, error)) {
	view := t.state.view
	t.StopChunk(path, func(written string, err error) {
		if err != nil {
			failed(done, err)
			return
		}
		run(view, "Tracing.tracingStop", nil, then(done, func(answer) (string, error) { return written, nil }))
	})
}

// HAROptions are Tracing.StartHAR's: Content "omit", "embed" or "attach";
// Mode "full" or "minimal"; URLFilter a glob ("" for each: the default).
type HAROptions struct {
	Path      string
	Content   string
	Mode      string
	URLFilter string
}

// StartHAR starts recording a HAR; done gets its id.
func (t *Tracing) StartHAR(options HAROptions, done func(string, error)) {
	har := object{"content": optionalText(options.Content), "mode": optionalText(options.Mode),
		"urlGlob": optionalText(options.URLFilter)}
	params := object{"options": har.clean()}
	if options.Path != "" {
		params["path"] = absolute(options.Path)
	}
	run(t.state.view, "Tracing.harStart", params, then(done, func(a answer) (string, error) { return a.str("harId"), nil }))
}

// ExportHAR writes HAR harID to path; done gets the path written.
func (t *Tracing) ExportHAR(harID, path string, done func(string, error)) {
	params := object{"harId": harID}
	if path != "" {
		params["path"] = absolute(path)
	}
	run(t.state.view, "Tracing.harExport", params, then(done, func(a answer) (string, error) { return a.str("path"), nil }))
}

// APIRequestContext is requests through the engine's network, with the
// page's cookies (Page.Request).
type APIRequestContext struct{ state *state }

// FetchOptions are APIRequestContext.Fetch's request: Method "" is GET;
// Timeout in milliseconds, 0 for the default; MaxRedirects nil and
// MaxBodySize 0 for the defaults.
type FetchOptions struct {
	Method       string
	Headers      []Header
	Data         []byte
	Timeout      float64
	MaxRedirects *int
	MaxBodySize  int64
}

// Fetch requests url.
func (r *APIRequestContext) Fetch(url string, options FetchOptions, done func(*APIResponse, error)) {
	method := options.Method
	if method == "" {
		method = "GET"
	}
	params := object{"url": url, "method": method, "headers": headerList(options.Headers),
		"timeout": milliseconds(options.Timeout), "maxRedirects": options.MaxRedirects}
	if options.Data != nil {
		params["postData"] = base64.StdEncoding.EncodeToString(options.Data)
	}
	if options.MaxBodySize > 0 {
		params["maxBodySize"] = options.MaxBodySize
	}
	run(r.state.view, "APIRequestContext.fetch", params, then(done, func(a answer) (*APIResponse, error) {
		value := a.object("response")
		if value == nil {
			value = answer{}
		}
		body, err := decodeBase64(value.str("body"))
		if err != nil {
			return nil, err
		}
		return &APIResponse{value: value, body: body}, nil
	}))
}

// Get is a GET of url.
func (r *APIRequestContext) Get(url string, done func(*APIResponse, error)) {
	r.Fetch(url, FetchOptions{}, done)
}

// Post is a POST of data to url.
func (r *APIRequestContext) Post(url string, data []byte, done func(*APIResponse, error)) {
	if data == nil {
		data = []byte{}
	}
	r.Fetch(url, FetchOptions{Method: "POST", Data: data}, done)
}

// APIResponse is APIRequestContext.Fetch's answer, with its body.
type APIResponse struct {
	value answer
	body  []byte
}

// URL is the response's URL.
func (r *APIResponse) URL() string { return r.value.str("url") }

// Status is the HTTP status.
func (r *APIResponse) Status() int {
	n, _ := r.value.number("status")
	return int(n)
}

// StatusText is the HTTP status's text.
func (r *APIResponse) StatusText() string { return r.value.str("statusText") }

// Headers is the response's headers, their names in lower case.
func (r *APIResponse) Headers() []Header { return headersOf(r.value) }

// OK says whether the status is 200-299 (or 0).
func (r *APIResponse) OK() bool {
	status := r.Status()
	return status == 0 || status >= 200 && status <= 299
}

// Body is the response's body.
func (r *APIResponse) Body() []byte { return r.body }

// Text is the response's body as text.
func (r *APIResponse) Text() string { return string(r.body) }

// JSON decodes the response's body into into.
func (r *APIResponse) JSON(into any) error { return json.Unmarshal(r.body, into) }
