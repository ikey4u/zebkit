package automation

import (
	"encoding/json"
	"regexp"
	"runtime"
	"strings"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// EventKind is what Page.On listens to: Playwright's page events.
type EventKind int

const (
	EventConsole EventKind = iota
	EventDialog
	EventFileChooser
	EventDownload
	EventRequest
	EventResponse
	EventRequestFinished
	EventRequestFailed
	EventFrameNavigated
	EventFrameAttached
	EventFrameDetached
	EventLoadState
	EventWorker
	EventCrash
	EventClose
)

var eventNames = [...]string{"console", "dialog", "filechooser", "download", "request", "response",
	"requestfinished", "requestfailed", "framenavigated", "frameattached", "framedetached", "loadstate",
	"worker", "crash", "close"}

// String is the event's name as Playwright's page.on has it ("dialog").
func (k EventKind) String() string {
	if k >= 0 && int(k) < len(eventNames) {
		return eventNames[k]
	}
	return "unknown"
}

// PageEvent is an event of the page, as Page.On gives it: Kind, and the
// field of its kind.
type PageEvent struct {
	Kind        EventKind
	Console     *ConsoleMessage
	Dialog      *Dialog
	FileChooser *FileChooser
	Download    *Download
	// Request is EventRequest's, EventRequestFinished's and
	// EventRequestFailed's.
	Request  *Request
	Response *Response
	// Frame is EventFrameNavigated's, EventFrameAttached's,
	// EventFrameDetached's and EventLoadState's: the protocol's {"frame",
	// "url", "name", ...}.
	Frame  json.RawMessage
	Worker *Worker
}

// ListenerID is what Page.On, OnProtocolEvent and WaitForEvent return, for
// RemoveListener.
type ListenerID uint64

type listener struct {
	id       ListenerID
	kind     EventKind
	function func(*PageEvent)
	running  bool
}

type protocolListener struct {
	id       ListenerID
	function func(string, json.RawMessage)
	running  bool
}

type routeEntry struct {
	pattern object
	source  string
	matches func(string) bool
	handler func(*Route, *Request)
	running bool
}

type bindingEntry struct {
	function func(*BindingCall)
	running  bool
}

// state is what a page's objects share.
type state struct {
	view         *zebkit.View
	nextListener ListenerID
	listeners    []*listener
	protocol     []*protocolListener
	routes       []*routeEntry
	bindings     map[string]*bindingEntry
	workers      []*Worker
	// The last requests, by id: what a request's end names.
	requests []*Request
	// The enabled:false subscriptions sent and not answered yet, for
	// dialogs and file choosers: one held before it is answered has no
	// listener left to answer it.
	unsubscribing [2]int
}

func (s *state) listening(kind EventKind) bool {
	for _, l := range s.listeners {
		if l.kind == kind {
			return true
		}
	}
	return false
}

// Page is a view's page with Playwright's Page API, and its main frame's
// (Frame's methods). One Page a view: it takes the view's automation events
// (View.OnAutomationEvent).
type Page struct {
	*Frame
	state *state
}

// pages is the Page of each view, so that each view has one.
var pages = map[*zebkit.View]*Page{}

// NewPage is view's page; the same Page for the same view.
func NewPage(view *zebkit.View) *Page {
	mustEngineThread()
	for v := range pages {
		if v.IsClosed() {
			delete(pages, v)
		}
	}
	if page, ok := pages[view]; ok {
		return page
	}
	page := pageOf(&state{view: view, bindings: map[string]*bindingEntry{}})
	if view.IsClosed() {
		return page
	}
	pages[view] = page
	s := page.state
	view.OnAutomationEvent(func(event string, params json.RawMessage) { dispatch(s, event, params) })
	return page
}

// OpenPage is the page of a new view, width by height.
func OpenPage(width, height int) (*Page, error) {
	view, err := zebkit.NewView("", width, height)
	if err != nil {
		return nil, err
	}
	return NewPage(view), nil
}

func pageOf(s *state) *Page {
	page := &Page{state: s}
	page.Frame = &Frame{state: s}
	return page
}

// View is the page's view.
func (p *Page) View() *zebkit.View { return p.state.view }

// URL is the page's URL.
func (p *Page) URL() string { return p.state.view.URL() }

// MainFrame is the page's main frame.
func (p *Page) MainFrame() *Frame { return p.Frame }

// FrameByID is a frame by its id (from Frames or an event).
func (p *Page) FrameByID(id string) *Frame { return &Frame{state: p.state, id: id} }

// On calls listener with each event of kind; it returns the id for
// RemoveListener. A dialog or file chooser listener holds the page's dialogs
// or choosers for it to answer; with none, the page's dialogs are dismissed
// (a beforeunload one accepted) and its file choosers cancelled.
func (p *Page) On(kind EventKind, function func(*PageEvent)) ListenerID {
	mustEngineThread()
	s := p.state
	s.nextListener++
	id := s.nextListener
	first := !s.listening(kind)
	s.listeners = append(s.listeners, &listener{id: id, kind: kind, function: function})
	if first {
		p.subscribe(kind, true)
	}
	return id
}

// subscribe asks for, or stops, the page's held dialogs or file choosers:
// the engine holds them only while a listener of theirs is there.
func (p *Page) subscribe(kind EventKind, enabled bool) {
	var event string
	var at int
	switch kind {
	case EventDialog:
		event, at = "dialog", 0
	case EventFileChooser:
		event, at = "fileChooser", 1
	default:
		return
	}
	params := object{"event": event, "enabled": enabled}
	// At once: commands run in the order they are given.
	if enabled {
		run(p.state.view, "Page.updateSubscription", params, nil)
		return
	}
	s := p.state
	s.unsubscribing[at]++
	run(s.view, "Page.updateSubscription", params, func(answer, error) {
		if s.unsubscribing[at] > 0 {
			s.unsubscribing[at]--
		}
	})
}

// OnProtocolEvent calls function(event, params) for every protocol event of
// the page ("Page.request", "BrowserContext.dialog", ...), before On's.
func (p *Page) OnProtocolEvent(function func(event string, params json.RawMessage)) ListenerID {
	mustEngineThread()
	s := p.state
	s.nextListener++
	s.protocol = append(s.protocol, &protocolListener{id: s.nextListener, function: function})
	return s.nextListener
}

// RemoveListener removes a listener On, OnProtocolEvent or WaitForEvent
// added. The last dialog or file chooser listener gone, the page's dialogs
// and choosers are no longer held.
func (p *Page) RemoveListener(id ListenerID) {
	mustEngineThread()
	s := p.state
	for i, l := range s.listeners {
		if l.id == id {
			s.listeners = append(s.listeners[:i:i], s.listeners[i+1:]...)
			if !s.listening(l.kind) {
				p.subscribe(l.kind, false)
			}
			return
		}
	}
	for i, l := range s.protocol {
		if l.id == id {
			s.protocol = append(s.protocol[:i:i], s.protocol[i+1:]...)
			return
		}
	}
}

// WaitForEvent calls done with the next event of kind for which predicate
// (nil: any) is true. Its listener goes once the event has come;
// RemoveListener with the id it returns stops waiting (done is then never
// called).
func (p *Page) WaitForEvent(kind EventKind, predicate func(*PageEvent) bool, done func(*PageEvent)) ListenerID {
	var id ListenerID
	finished := false
	id = p.On(kind, func(event *PageEvent) {
		if finished || (predicate != nil && !predicate(event)) {
			return
		}
		finished = true
		p.RemoveListener(id)
		if done != nil {
			done(event)
		}
	})
	return id
}

// Command runs one of the page's commands, as the protocol has them, with
// params, a JSON object (nil for none).
func (p *Page) Command(method string, params any, done func(json.RawMessage, error)) {
	command(p.state.view, method, params, nil, done)
}

// command runs method with params and members on view's page.
func command(view *zebkit.View, method string, params any, members object, done func(json.RawMessage, error)) {
	all, err := with(params, members)
	if err != nil {
		failed(done, err)
		return
	}
	run(view, method, all, func(a answer, err error) {
		if done == nil {
			return
		}
		if err != nil {
			done(nil, err)
			return
		}
		raw, _ := json.Marshal(a)
		done(raw, nil)
	})
}

// Route calls handler(route, request) for the requests whose URL matches
// url, a glob as Playwright's ("**/data.json"); the last route added that
// matches answers. A request no handler takes continues. The handler answers
// each route (Continue, Fulfill or Abort).
func (p *Page) Route(url string, handler func(*Route, *Request), done func(error)) {
	glob := newGlob(url)
	p.addRoute(&routeEntry{pattern: object{"glob": url}, source: url, matches: glob.matches, handler: handler}, done)
}

// RouteRegex is Route for a regular expression, which the engine matches as
// JavaScript's (a leading (?i) is its "i" flag).
func (p *Page) RouteRegex(re *regexp.Regexp, handler func(*Route, *Request), done func(error)) {
	source, ignoreCase := regexOf(re)
	flags := ""
	if ignoreCase {
		flags = "i"
	}
	p.addRoute(&routeEntry{pattern: object{"regexSource": source, "regexFlags": flags}, source: source,
		matches: re.MatchString, handler: handler}, done)
}

func (p *Page) addRoute(entry *routeEntry, done func(error)) {
	mustEngineThread()
	p.state.routes = append(p.state.routes, entry)
	p.setRoutes(done)
}

// Unroute removes the routes of url (a glob, or a regular expression's
// source as RouteRegex sent it).
func (p *Page) Unroute(url string, done func(error)) {
	mustEngineThread()
	kept := p.state.routes[:0:0]
	for _, route := range p.state.routes {
		if route.source != url {
			kept = append(kept, route)
		}
	}
	p.state.routes = kept
	p.setRoutes(done)
}

func (p *Page) setRoutes(done func(error)) {
	patterns := []object{}
	for _, route := range p.state.routes {
		patterns = append(patterns, route.pattern)
	}
	run(p.state.view, "Page.setNetworkInterceptionPatterns", object{"patterns": patterns}, unit(done))
}

// ExposeFunction makes window.<name>(...args) in the page's frames call
// function, whose result (encoded with encoding/json) or error answers the
// page's promise.
func (p *Page) ExposeFunction(name string, function func(args []json.RawMessage) (any, error), done func(error)) {
	p.ExposeBinding(name, func(call *BindingCall) {
		value, err := function(call.Args())
		if err != nil {
			call.Reject(err.Error())
			return
		}
		call.Resolve(value)
	}, done)
}

// ExposeBinding is ExposeFunction with the call itself, answered now or
// later.
func (p *Page) ExposeBinding(name string, function func(*BindingCall), done func(error)) {
	mustEngineThread()
	p.state.bindings[name] = &bindingEntry{function: function}
	run(p.state.view, "Page.exposeBinding", object{"name": name}, unit(done))
}

// Frames is the page's frames, the main one first.
func (p *Page) Frames(done func([]*Frame, error)) {
	s := p.state
	run(s.view, "Page.frames", nil, then(done, func(a answer) ([]*Frame, error) {
		frames := []*Frame{}
		for _, raw := range a.list("frames") {
			f := readAnswer(raw)
			frame := &Frame{state: s, name: f.str("name"), url: f.str("url")}
			if parent, ok := f["parentFrame"]; ok && string(parent) != "null" {
				frame.id = f.str("frame")
			}
			frames = append(frames, frame)
		}
		return frames, nil
	}))
}

// FindFrame is the frame named name, or whose URL has urlPart ("" for
// either: not asked).
func (p *Page) FindFrame(name, urlPart string, done func(*Frame, error)) {
	p.Frames(func(frames []*Frame, err error) {
		if err != nil {
			failed(done, err)
			return
		}
		for _, frame := range frames {
			if (name != "" && frame.name == name) || (urlPart != "" && strings.Contains(frame.url, urlPart)) {
				if done != nil {
					done(frame, nil)
				}
				return
			}
		}
		what := name
		if what == "" {
			what = urlPart
		}
		failed(done, fail("no frame "+what))
	})
}

func (p *Page) navigation(method string, opts any, done func(*Response, error)) {
	params, err := options(opts)
	if err != nil {
		failed(done, err)
		return
	}
	s := p.state
	run(s.view, method, params, then(done, func(a answer) (*Response, error) { return responseOf(s, a["response"]), nil }))
}

// Reload reloads the page; options: waitUntil, timeout. The main resource's
// response, if there was one.
func (p *Page) Reload(options any, done func(*Response, error)) {
	p.navigation("Page.reload", options, done)
}

// GoBack goes back in the page's history.
func (p *Page) GoBack(options any, done func(*Response, error)) {
	p.navigation("Page.goBack", options, done)
}

// GoForward goes forward in the page's history.
func (p *Page) GoForward(options any, done func(*Response, error)) {
	p.navigation("Page.goForward", options, done)
}

// Screenshot is a picture of the page, PNG unless options say (type,
// quality, fullPage, clip, omitBackground, animations, caret, scale, mask,
// maskColor, style, timeout); with path it is written there too.
func (p *Page) Screenshot(options any, done func([]byte, error)) {
	params, path, err := takePath(options)
	if err != nil {
		failed(done, err)
		return
	}
	run(p.state.view, "Page.screenshot", params, then(done, func(a answer) ([]byte, error) {
		data, err := binary(a, "binary")
		if err != nil {
			return nil, err
		}
		return writeFile(path, data)
	}))
}

// ScreenshotPixels is the screenshot's pixels, unencoded (RGBA), with
// Screenshot's options but type, quality and path.
func (p *Page) ScreenshotPixels(options any, done func(*zebkit.ScreenshotPixels, error)) {
	params, err := with(options, nil)
	if err != nil {
		failed(done, err)
		return
	}
	zebkit.CallAutomationPixels(p.state.view, "Page.screenshot", params, done)
}

// PDF is the page as PDF, with Page.pdf's options; with path it is written
// there too.
func (p *Page) PDF(options any, done func([]byte, error)) {
	params, path, err := takePath(options)
	if err != nil {
		failed(done, err)
		return
	}
	run(p.state.view, "Page.pdf", params, then(done, func(a answer) ([]byte, error) {
		data, err := binary(a, "pdf")
		if err != nil {
			return nil, err
		}
		return writeFile(path, data)
	}))
}

// SetViewportSize sizes the page's viewport, in CSS pixels.
func (p *Page) SetViewportSize(width, height int, done func(error)) {
	run(p.state.view, "Page.setViewportSize", object{"viewportSize": object{"width": width, "height": height}}, unit(done))
}

// EmulateMedia: options media, colorScheme, reducedMotion, forcedColors,
// contrast.
func (p *Page) EmulateMedia(options any, done func(error)) {
	params, err := with(options, nil)
	if err != nil {
		failedUnit(done, err)
		return
	}
	run(p.state.view, "Page.emulateMedia", params, unit(done))
}

// AddInitScript runs source in every document of the page from now on,
// before the page's own script; done gets a Disposable that takes it away
// again.
func (p *Page) AddInitScript(source string, done func(*Disposable, error)) {
	view := p.state.view
	run(view, "Page.addInitScript", object{"source": source}, then(done, func(a answer) (*Disposable, error) {
		registration, ok := a.text("registration")
		if !ok {
			return nil, fail("the engine gave no registration")
		}
		return &Disposable{view: view, registration: registration}, nil
	}))
}

// SetExtraHTTPHeaders adds headers to every request of the page.
func (p *Page) SetExtraHTTPHeaders(headers []Header, done func(error)) {
	run(p.state.view, "Page.setExtraHTTPHeaders", object{"headers": headerList(headers)}, unit(done))
}

// BringToFront activates the page.
func (p *Page) BringToFront(done func(error)) {
	run(p.state.view, "Page.bringToFront", nil, unit(done))
}

// Close closes the page.
func (p *Page) Close(done func(error)) { run(p.state.view, "Page.close", nil, unit(done)) }

// Workers is the page's dedicated workers, as their events told them.
func (p *Page) Workers() []*Worker {
	mustEngineThread()
	return append([]*Worker(nil), p.state.workers...)
}

// StartVideo records the page to path ("" for a temporary .webm), WebM,
// VP8, width by height (0 for the page's size); done gets the path.
func (p *Page) StartVideo(path string, width, height int, done func(string, error)) {
	params := object{}
	if path != "" {
		params["path"] = path
	}
	if width > 0 && height > 0 {
		params["size"] = object{"width": width, "height": height}
	}
	run(p.state.view, "Page.startVideo", params, then(done, func(a answer) (string, error) { return a.str("path"), nil }))
}

// StopVideo ends the recording; done gets its path.
func (p *Page) StopVideo(done func(string, error)) {
	run(p.state.view, "Page.stopVideo", nil, then(done, func(a answer) (string, error) { return a.str("path"), nil }))
}

// SetGeolocation is {"latitude", "longitude", "accuracy"}, JSON; nil for
// none.
func (p *Page) SetGeolocation(geolocation any, done func(error)) {
	contextCommand(p.state.view, nil, "BrowserContext.setGeolocation", "geolocation", geolocation, done)
}

// SetHTTPCredentials is {"username", "password", "origin"}, JSON; nil for
// none.
func (p *Page) SetHTTPCredentials(credentials any, done func(error)) {
	contextCommand(p.state.view, nil, "BrowserContext.setHTTPCredentials", "httpCredentials", credentials, done)
}

// Cookies is the cookies of the page's profile, of urls (none: all).
func (p *Page) Cookies(urls []string, done func([]Cookie, error)) {
	run(p.state.view, "BrowserContext.cookies", cookieURLs(urls), then(done, cookieList))
}

// AddCookies adds cookies to the page's profile: each with url, or domain and
// path.
func (p *Page) AddCookies(cookies []Cookie, done func(error)) {
	run(p.state.view, "BrowserContext.addCookies", object{"cookies": cookieInput(cookies)}, unit(done))
}

// ClearCookies removes the profile's cookies, of name, domain and path ("":
// any).
func (p *Page) ClearCookies(name, domain, path string, done func(error)) {
	run(p.state.view, "BrowserContext.clearCookies", clearParams(name, domain, path), unit(done))
}

// StorageState is the profile's cookies and storage, as Playwright's
// storageState.
func (p *Page) StorageState(done func(json.RawMessage, error)) {
	command(p.state.view, "BrowserContext.storageState", nil, nil, done)
}

// SetStorageState sets the profile's cookies and storage, JSON.
func (p *Page) SetStorageState(state any, done func(error)) {
	contextCommand(p.state.view, nil, "BrowserContext.setStorageState", "storageState", state, done)
}

// GrantPermissions grants permissions ("geolocation", "notifications", ...)
// to origin ("": every origin).
func (p *Page) GrantPermissions(permissions []string, origin string, done func(error)) {
	run(p.state.view, "BrowserContext.grantPermissions", permissionParams(permissions, origin), unit(done))
}

// ClearPermissions takes back the permissions granted.
func (p *Page) ClearPermissions(done func(error)) {
	run(p.state.view, "BrowserContext.clearPermissions", nil, unit(done))
}

// SetOffline takes the page's profile off the network, or back.
func (p *Page) SetOffline(offline bool, done func(error)) {
	run(p.state.view, "BrowserContext.setOffline", object{"offline": offline}, unit(done))
}

// Keyboard is the page's keyboard.
func (p *Page) Keyboard() *Keyboard { return &Keyboard{p.state} }

// Mouse is the page's mouse.
func (p *Page) Mouse() *Mouse { return &Mouse{p.state} }

// Touchscreen is the page's touch screen.
func (p *Page) Touchscreen() *Touchscreen { return &Touchscreen{p.state} }

// Clock is the page's clock.
func (p *Page) Clock() *Clock { return &Clock{p.state} }

// Tracing is the page's traces and HAR files.
func (p *Page) Tracing() *Tracing { return &Tracing{p.state} }

// Request is requests through the engine's network, with the page's cookies.
func (p *Page) Request() *APIRequestContext { return &APIRequestContext{p.state} }

// Disposable is what AddInitScript added, until Dispose takes it away
// (Playwright's Disposable).
type Disposable struct {
	view         *zebkit.View
	registration string
}

// Registration is the protocol's name for it (Page.removeRegistration's).
func (d *Disposable) Registration() string { return d.registration }

// Dispose takes it away: documents from now on do not get it.
func (d *Disposable) Dispose(done func(error)) {
	run(d.view, "Page.removeRegistration", object{"registration": d.registration}, unit(done))
}

// ---- Events --------------------------------------------------------------------

// dispatch gives the automation event event with params to the page's
// listeners.
func dispatch(s *state, name string, params json.RawMessage) {
	if name == "Page.close" {
		if pages[s.view] != nil && pages[s.view].state == s {
			delete(pages, s.view)
		}
	}
	for _, l := range append([]*protocolListener(nil), s.protocol...) {
		// A listener that runs again from inside itself is not called.
		if !l.running {
			l.running = true
			l.function(name, params)
			l.running = false
		}
	}
	value := readAnswer(params)
	event := &PageEvent{}
	switch name {
	case "BrowserContext.route":
		routeEvent(s, value)
		return
	case "BrowserContext.bindingCall":
		bindingEvent(s, value)
		return
	case "Worker.close":
		id := value.str("worker")
		for i, worker := range s.workers {
			if worker.id == id {
				s.workers = append(s.workers[:i:i], s.workers[i+1:]...)
				listeners := worker.closeListeners
				worker.closeListeners = nil
				for _, function := range listeners {
					function(worker)
				}
				break
			}
		}
		return
	case "BrowserContext.console":
		event.Kind, event.Console = EventConsole, &ConsoleMessage{value: value}
	case "BrowserContext.dialog":
		event.Kind, event.Dialog = EventDialog, &Dialog{state: s, value: value}
	case "Page.fileChooser":
		event.Kind, event.FileChooser = EventFileChooser, &FileChooser{state: s, value: value}
	case "Page.download":
		event.Kind, event.Download = EventDownload, &Download{state: s, value: value}
	case "Page.request":
		request := &Request{value: value}
		if len(s.requests) >= 1000 {
			s.requests = s.requests[1:]
		}
		s.requests = append(s.requests, request)
		event.Kind, event.Request = EventRequest, request
	case "Page.response":
		event.Kind, event.Response = EventResponse, &Response{state: s, value: value}
	case "Page.requestFinished":
		event.Kind, event.Request = EventRequestFinished, endedRequest(s, value, false)
	case "Page.requestFailed":
		event.Kind, event.Request = EventRequestFailed, endedRequest(s, value, true)
	case "Frame.navigated":
		event.Kind, event.Frame = EventFrameNavigated, params
	case "Page.frameAttached":
		event.Kind, event.Frame = EventFrameAttached, params
	case "Page.frameDetached":
		event.Kind, event.Frame = EventFrameDetached, params
	case "Frame.loadstate":
		event.Kind, event.Frame = EventLoadState, params
	case "Page.worker":
		worker := &Worker{state: s, id: value.str("worker"), url: value.str("url")}
		s.workers = append(s.workers, worker)
		event.Kind, event.Worker = EventWorker, worker
	case "Page.crash":
		event.Kind = EventCrash
	case "Page.close":
		event.Kind = EventClose
	default:
		return
	}
	var listeners []*listener
	for _, l := range s.listeners {
		if l.kind == event.Kind {
			listeners = append(listeners, l)
		}
	}
	if len(listeners) == 0 {
		// Held for this page while its last listener went (the engine had
		// not heard yet): let the page go on, as Playwright does without a
		// listener. One held for the application or a CDP client is theirs.
		switch {
		case event.Dialog != nil && s.unsubscribing[0] > 0 && !value.boolean("auto"):
			if event.Dialog.Type() == "beforeunload" {
				event.Dialog.Accept(nil)
			} else {
				event.Dialog.Dismiss(nil)
			}
		case event.FileChooser != nil && s.unsubscribing[1] > 0:
			event.FileChooser.SetFiles([]string{}, nil)
		}
		return
	}
	for _, l := range listeners {
		if !l.running {
			l.running = true
			l.function(event)
			l.running = false
		}
	}
}

// endedRequest is the request a request's end names, with what its end
// says.
func endedRequest(s *state, value answer, failed bool) *Request {
	id, hasID := value.text("request")
	whole := answer{}
	for i := len(s.requests) - 1; hasID && i >= 0; i-- {
		if s.requests[i].value.str("request") == id {
			for k, v := range s.requests[i].value {
				whole[k] = v
			}
			break
		}
	}
	for k, v := range value {
		whole[k] = v
	}
	if failed && hasID {
		kept := s.requests[:0:0]
		for _, request := range s.requests {
			if request.value.str("request") != id {
				kept = append(kept, request)
			}
		}
		s.requests = kept
	}
	return &Request{value: whole}
}

func routeEvent(s *state, value answer) {
	request := &Request{value: value.object("request")}
	if request.value == nil {
		request.value = answer{}
	}
	route := &Route{state: s, id: value.str("route"), request: request}
	var handler *routeEntry
	for i := len(s.routes) - 1; i >= 0; i-- {
		if s.routes[i].matches(request.URL()) {
			handler = s.routes[i]
			break
		}
	}
	// A request no route takes, or one its handler's own run asks for, goes on.
	if handler == nil || handler.running {
		route.Continue(ContinueOptions{}, nil)
		return
	}
	handler.running = true
	defer func() { handler.running = false }()
	handler.handler(route, request)
}

func bindingEvent(s *state, value answer) {
	name := value.str("name")
	call := &BindingCall{state: s, id: value.str("binding"), frame: value.str("frame"), args: value.list("args")}
	if call.args == nil {
		call.args = []json.RawMessage{}
	}
	// One let go unanswered rejects the page's promise.
	runtime.SetFinalizer(call, func(call *BindingCall) {
		if !call.answered {
			id, view := call.id, s.view
			_ = zebkit.Post(func() {
				run(view, "BindingCall.reject", object{"binding": id, "error": "the application did not answer"}, nil)
			})
		}
	})
	entry := s.bindings[name]
	switch {
	case entry == nil:
		call.Reject("no function " + name)
	case entry.running:
		call.Reject("the application's function is already running")
	default:
		entry.running = true
		defer func() { entry.running = false }()
		entry.function(call)
	}
}
