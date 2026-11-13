package automation

import (
	"encoding/base64"
	"encoding/json"
	"os"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// ---- Globs: Playwright's URL globs: * (no /), **, **/, {a,b}, \ to escape ----

type tokenKind int

const (
	tokenChar tokenKind = iota
	tokenStar           // *: anything but /
	tokenAny            // **: anything
	tokenDirs           // **/: nothing, or anything ending in /
	tokenGroup
)

type token struct {
	kind tokenKind
	char rune
	// afterSlash: a / came before **/, which then needs one character at least.
	afterSlash   bool
	alternatives [][]token
}

type glob struct{ tokens []token }

func newGlob(pattern string) *glob {
	chars := []rune(pattern)
	at := 0
	alternatives := parseGlob(chars, &at, false)
	return &glob{alternatives[len(alternatives)-1]}
}

// parseGlob is the alternatives up to the end, or to } inside a group.
func parseGlob(chars []rune, at *int, inGroup bool) [][]token {
	alternatives := [][]token{{}}
	for *at < len(chars) {
		c := chars[*at]
		*at++
		current := &alternatives[len(alternatives)-1]
		switch {
		case c == '\\' && *at < len(chars):
			*current = append(*current, token{kind: tokenChar, char: chars[*at]})
			*at++
		case c == '*':
			var before rune
			if *at >= 2 {
				before = chars[*at-2]
			}
			stars := 1
			for *at < len(chars) && chars[*at] == '*' {
				stars++
				*at++
			}
			switch {
			case stars > 1 && *at < len(chars) && chars[*at] == '/':
				*at++
				*current = append(*current, token{kind: tokenDirs, afterSlash: before == '/'})
			case stars > 1:
				*current = append(*current, token{kind: tokenAny})
			default:
				*current = append(*current, token{kind: tokenStar})
			}
		case c == '{' && !inGroup:
			group := parseGlob(chars, at, true)
			*current = append(*current, token{kind: tokenGroup, alternatives: group})
		case c == '}' && inGroup:
			return alternatives
		case c == ',' && inGroup:
			alternatives = append(alternatives, []token{})
		default:
			*current = append(*current, token{kind: tokenChar, char: c})
		}
	}
	return alternatives
}

func (g *glob) matches(text string) bool { return matchGlob(g.tokens, []rune(text), nil) }

// matchGlob says whether tokens and then rest (what follows a group) match
// text whole.
func matchGlob(tokens []token, text []rune, rest [][]token) bool {
	if len(tokens) == 0 {
		if len(rest) == 0 {
			return len(text) == 0
		}
		return matchGlob(rest[len(rest)-1], text, rest[:len(rest)-1])
	}
	t, tail := tokens[0], tokens[1:]
	switch t.kind {
	case tokenChar:
		return len(text) > 0 && text[0] == t.char && matchGlob(tail, text[1:], rest)
	case tokenStar:
		for n := 0; n <= len(text); n++ {
			if n > 0 && text[n-1] == '/' {
				break
			}
			if matchGlob(tail, text[n:], rest) {
				return true
			}
		}
		return false
	case tokenAny:
		for n := 0; n <= len(text); n++ {
			if matchGlob(tail, text[n:], rest) {
				return true
			}
		}
		return false
	case tokenDirs:
		// (.*/) after anything, ((.+/)|) after a /.
		if t.afterSlash && matchGlob(tail, text, rest) {
			return true
		}
		for n := 1; n <= len(text); n++ {
			if text[n-1] == '/' && (!t.afterSlash || n >= 2) && matchGlob(tail, text[n:], rest) {
				return true
			}
		}
		return false
	case tokenGroup:
		next := append(append([][]token(nil), rest...), tail)
		for _, alternative := range t.alternatives {
			if matchGlob(alternative, text, next) {
				return true
			}
		}
	}
	return false
}

// ---- Requests and responses ---------------------------------------------------------

// Request is a request of the page, as Playwright's Request.
type Request struct{ value answer }

// URL is the request's URL.
func (r *Request) URL() string { return r.value.str("url") }

// Method is the request's method.
func (r *Request) Method() string { return r.value.str("method") }

// ResourceType is "document", "script", "fetch", ...
func (r *Request) ResourceType() string { return r.value.str("resourceType") }

// Headers is the request's headers, their names in lower case.
func (r *Request) Headers() []Header { return headersOf(r.value) }

// Failure is why it failed, for a failed one ("" for none).
func (r *Request) Failure() string { return r.value.str("failureText") }

// IsNavigationRequest says whether it loads a frame's document.
func (r *Request) IsNavigationRequest() bool { return r.value.boolean("isNavigationRequest") }

// JSON is all the protocol says of it.
func (r *Request) JSON() json.RawMessage {
	raw, _ := json.Marshal(r.value)
	return raw
}

// Response is a response, as Playwright's Response.
type Response struct {
	state *state
	value answer
}

func responseOf(s *state, raw json.RawMessage) *Response {
	value := readAnswer(raw)
	if len(raw) == 0 || raw[0] != '{' {
		return nil
	}
	return &Response{state: s, value: value}
}

// URL is the response's URL.
func (r *Response) URL() string { return r.value.str("url") }

// Status is the HTTP status, 0 when there was none.
func (r *Response) Status() int {
	n, _ := r.value.number("status")
	return int(n)
}

// StatusText is the HTTP status's text.
func (r *Response) StatusText() string { return r.value.str("statusText") }

// Headers is the response's headers, their names in lower case.
func (r *Response) Headers() []Header { return headersOf(r.value) }

// OK says whether the status is 200-299, or 0 (a response with no HTTP, such
// as a file's).
func (r *Response) OK() bool {
	status, ok := r.value.number("status")
	return ok && (status == 0 || status >= 200 && status <= 299)
}

// JSON is all the protocol says of it.
func (r *Response) JSON() json.RawMessage {
	raw, _ := json.Marshal(r.value)
	return raw
}

// Body is the response's body.
func (r *Response) Body(done func([]byte, error)) {
	if r.state == nil {
		failed(done, fail("the response is of no page"))
		return
	}
	request := r.value["request"]
	if request == nil {
		request = json.RawMessage("null")
	}
	run(r.state.view, "Response.body", object{"request": request}, then(done, func(a answer) ([]byte, error) {
		return binary(a, "binary")
	}))
}

// Text is the response's body as text.
func (r *Response) Text(done func(string, error)) {
	r.Body(func(body []byte, err error) {
		if done != nil {
			done(string(body), err)
		}
	})
}

// ContinueOptions are what Route.Continue changes ("" and nil: nothing).
type ContinueOptions struct {
	URL      string
	Method   string
	Headers  []Header
	PostData []byte
}

// FulfillOptions are Route.Fulfill's answer: Status (0: 200), Headers, Body,
// a content-type header when ContentType is set; JSON, when not nil, is the
// body encoded with encoding/json (and application/json); Path, when set, is
// the file whose bytes are the body.
type FulfillOptions struct {
	Status      int
	Headers     []Header
	Body        []byte
	ContentType string
	JSON        any
	Path        string
}

// Route is a request a route holds: continue, fulfill or abort it.
type Route struct {
	state   *state
	id      string
	request *Request
}

// Request is the request the route holds.
func (r *Route) Request() *Request { return r.request }

// Continue lets the request go on, with what options change.
func (r *Route) Continue(options ContinueOptions, done func(error)) {
	params := object{"route": r.id, "url": optionalText(options.URL), "method": optionalText(options.Method)}
	if options.Headers != nil {
		params["headers"] = options.Headers
	}
	if options.PostData != nil {
		params["postData"] = base64.StdEncoding.EncodeToString(options.PostData)
	}
	run(r.state.view, "Route.continue", params, unit(done))
}

// Fulfill answers the request.
func (r *Route) Fulfill(options FulfillOptions, done func(error)) {
	body, contentType := options.Body, options.ContentType
	if options.JSON != nil {
		encoded, err := jsonValue(options.JSON)
		if err != nil {
			failedUnit(done, err)
			return
		}
		body = encoded
		if contentType == "" {
			contentType = "application/json"
		}
	}
	if options.Path != "" {
		data, err := os.ReadFile(options.Path)
		if err != nil {
			failedUnit(done, err)
			return
		}
		body = data
	}
	status := options.Status
	if status == 0 {
		status = 200
	}
	headers := append([]Header{}, options.Headers...)
	if contentType != "" {
		headers = append(headers, Header{"content-type", contentType})
	}
	run(r.state.view, "Route.fulfill", object{"route": r.id, "status": status, "headers": headers,
		"body": base64.StdEncoding.EncodeToString(body), "isBase64": true}, unit(done))
}

// Abort fails the request: errorCode as Playwright's ("failed" for "",
// "aborted", "timedout", ...).
func (r *Route) Abort(errorCode string, done func(error)) {
	if errorCode == "" {
		errorCode = "failed"
	}
	run(r.state.view, "Route.abort", object{"route": r.id, "errorCode": errorCode}, unit(done))
}

// BindingCall is a call of a function Page.ExposeBinding gave the page:
// answer it with Resolve or Reject. One the garbage collector finds
// unanswered rejects the page's promise.
type BindingCall struct {
	state    *state
	id       string
	frame    string
	args     []json.RawMessage
	answered bool
}

// Args is the call's arguments.
func (c *BindingCall) Args() []json.RawMessage { return c.args }

// Frame is the id of the frame that called ("" for the main frame).
func (c *BindingCall) Frame() string { return c.frame }

// Resolve answers the page's promise with value, encoded with
// encoding/json (a json.RawMessage as it is).
func (c *BindingCall) Resolve(value any) {
	if c.answered {
		return
	}
	result, err := jsonValue(value)
	if err != nil {
		c.Reject(err.Error())
		return
	}
	c.answered = true
	run(c.state.view, "BindingCall.resolve", object{"binding": c.id, "result": result}, nil)
}

// Reject rejects the page's promise with message.
func (c *BindingCall) Reject(message string) {
	if c.answered {
		return
	}
	c.answered = true
	run(c.state.view, "BindingCall.reject", object{"binding": c.id, "error": message}, nil)
}

// ---- Dialogs, file choosers, downloads, the console, workers ------------------------

// Dialog is a JavaScript dialog of the page, held for an EventDialog
// listener.
type Dialog struct {
	state *state
	value answer
}

// Type is "alert", "confirm", "prompt" or "beforeunload".
func (d *Dialog) Type() string { return d.value.str("type") }

// Message is the dialog's message.
func (d *Dialog) Message() string { return d.value.str("message") }

// DefaultValue is a prompt's default text.
func (d *Dialog) DefaultValue() string { return d.value.str("defaultValue") }

func (d *Dialog) id() json.RawMessage {
	if id, ok := d.value["dialog"]; ok {
		return id
	}
	return json.RawMessage("null")
}

// Accept answers OK; a prompt answers its default text.
func (d *Dialog) Accept(done func(error)) {
	run(d.state.view, "Dialog.accept", object{"dialog": d.id()}, unit(done))
}

// AcceptPrompt answers OK with text for a prompt.
func (d *Dialog) AcceptPrompt(text string, done func(error)) {
	run(d.state.view, "Dialog.accept", object{"dialog": d.id(), "promptText": text}, unit(done))
}

// Dismiss answers Cancel.
func (d *Dialog) Dismiss(done func(error)) {
	run(d.state.view, "Dialog.dismiss", object{"dialog": d.id()}, unit(done))
}

// FileChooser is a file chooser the page opened, held for an
// EventFileChooser listener.
type FileChooser struct {
	state *state
	value answer
}

// IsMultiple says whether it takes several files.
func (f *FileChooser) IsMultiple() bool { return f.value.boolean("isMultiple") }

// Page is the page that opened it.
func (f *FileChooser) Page() *Page { return (&Frame{state: f.state}).Page() }

// SetFiles answers with the files at paths (none: cancelled).
func (f *FileChooser) SetFiles(paths []string, done func(error)) {
	id := f.value["fileChooser"]
	if id == nil {
		id = json.RawMessage("null")
	}
	run(f.state.view, "FileChooser.setFiles", object{"fileChooser": id, "localPaths": absolutes(paths)}, unit(done))
}

// Download is a download of the page.
type Download struct {
	state *state
	value answer
}

// URL is the download's URL.
func (d *Download) URL() string { return d.value.str("url") }

// SuggestedFilename is the name the download suggests.
func (d *Download) SuggestedFilename() string { return d.value.str("suggestedFilename") }

// Page is the page it is of.
func (d *Download) Page() *Page { return (&Frame{state: d.state}).Page() }

func (d *Download) run(method string, params object, done func(answer, error)) {
	if params == nil {
		params = object{}
	}
	if id, ok := d.value["download"]; ok {
		params["download"] = id
	}
	run(d.state.view, method, params, done)
}

// Path is where it was saved, once it has finished; nil when it failed.
func (d *Download) Path(done func(*string, error)) {
	d.run("Artifact.pathAfterFinished", nil, stringValue(done))
}

// SaveAs copies it to path, once it has finished.
func (d *Download) SaveAs(path string, done func(error)) {
	d.run("Artifact.saveAs", object{"path": absolute(path)}, unit(done))
}

// Failure is why it failed; nil when it did not.
func (d *Download) Failure(done func(*string, error)) {
	d.run("Artifact.failure", nil, then(done, func(a answer) (*string, error) { return a.optional("error"), nil }))
}

// Cancel cancels it.
func (d *Download) Cancel(done func(error)) { d.run("Artifact.cancel", nil, unit(done)) }

// Delete deletes its file.
func (d *Download) Delete(done func(error)) { d.run("Artifact.delete", nil, unit(done)) }

// ConsoleMessage is a message of the page's console.
type ConsoleMessage struct{ value answer }

// Type is "log", "warning", "error", ...
func (m *ConsoleMessage) Type() string { return m.value.str("type") }

// Text is the message's text.
func (m *ConsoleMessage) Text() string { return m.value.str("text") }

// ConsoleLocation is where a console message comes from.
type ConsoleLocation struct {
	URL          string `json:"url"`
	LineNumber   int    `json:"lineNumber"`
	ColumnNumber int    `json:"columnNumber"`
}

// Location is where the message comes from.
func (m *ConsoleMessage) Location() ConsoleLocation {
	var location ConsoleLocation
	_ = json.Unmarshal(m.value["location"], &location)
	return location
}

// JSON is all the protocol says of it.
func (m *ConsoleMessage) JSON() json.RawMessage {
	raw, _ := json.Marshal(m.value)
	return raw
}

// Worker is a dedicated worker of the page (Page.Workers, EventWorker).
type Worker struct {
	state          *state
	id, url        string
	closeListeners []func(*Worker)
}

// URL is the worker's script's URL.
func (w *Worker) URL() string { return w.url }

// Evaluate is the value of expression (or of the function it is, called with
// arg) in the worker.
func (w *Worker) Evaluate(expression string, arg any, done func(json.RawMessage, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	params["worker"] = w.id
	run(w.state.view, "Worker.evaluateExpression", params, rawValue(done))
}

// OnClose calls function once the worker ends.
func (w *Worker) OnClose(function func(*Worker)) {
	if function != nil {
		w.closeListeners = append(w.closeListeners, function)
	}
}

// ---- Cookies and the context's commands --------------------------------------------

// Cookie is Playwright's cookie: with URL, or Domain and Path, to add one.
// Expires is in seconds since the epoch, -1 (or 0 when adding) for a
// session cookie; SameSite is "Strict", "Lax" or "None".
type Cookie struct {
	Name     string  `json:"name"`
	Value    string  `json:"value"`
	URL      string  `json:"url,omitempty"`
	Domain   string  `json:"domain,omitempty"`
	Path     string  `json:"path,omitempty"`
	Expires  float64 `json:"expires,omitempty"`
	HTTPOnly bool    `json:"httpOnly,omitempty"`
	Secure   bool    `json:"secure,omitempty"`
	SameSite string  `json:"sameSite,omitempty"`
}

func cookieURLs(urls []string) object {
	if len(urls) == 0 {
		return object{}
	}
	return object{"urls": urls}
}

func cookieList(a answer) ([]Cookie, error) {
	cookies := []Cookie{}
	if raw, ok := a["cookies"]; ok {
		if err := json.Unmarshal(raw, &cookies); err != nil {
			return nil, err
		}
	}
	return cookies, nil
}

func cookieInput(cookies []Cookie) []Cookie {
	if cookies == nil {
		return []Cookie{}
	}
	return cookies
}

func clearParams(name, domain, path string) object {
	return object{"name": optionalText(name), "domain": optionalText(domain), "path": optionalText(path)}
}

func permissionParams(permissions []string, origin string) object {
	if permissions == nil {
		permissions = []string{}
	}
	return object{"permissions": permissions, "origin": optionalText(origin)}
}

// contextCommand runs method with key: value (JSON) on view's page, or on a
// context (id) when view is nil.
func contextCommand(view *zebkit.View, context *string, method, key string, value any, done func(error)) {
	encoded, err := jsonText(value)
	if err != nil {
		failedUnit(done, err)
		return
	}
	params := object{key: encoded}
	if context != nil {
		params["context"] = *context
	}
	run(view, method, params, unit(done))
}
