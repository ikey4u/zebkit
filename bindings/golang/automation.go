package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"bytes"
	"encoding/json"
	"fmt"
	"strings"
	"unsafe"
)

// Playwright's API run inside the engine (<zebkit/automation.h>,
// docs/AUTOMATION.md): commands as Playwright's protocol names them, on a
// view's page, without CDP or Node. Parameters are JSON objects (the
// package's rule); results are the command's JSON object.

// AutomationCommands lists the commands this engine has.
func AutomationCommands() ([]string, error) {
	onEngineThread()
	var names []string
	return names, takeJSON(C.zebkit_automation_commands(), &names, "no commands")
}

// quote is text as a JSON string, as Playwright's selectors are written.
func quote(text string) string {
	var out bytes.Buffer
	encoder := json.NewEncoder(&out)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(text)
	return strings.TrimSuffix(out.String(), "\n")
}

// object is params, a JSON object (nil for {}), with members added.
func object(params any, members map[string]any) (json.RawMessage, error) {
	merged := map[string]json.RawMessage{}
	text, err := optionalJSON(params)
	if err != nil {
		return nil, err
	}
	if text != nil {
		raw := C.GoString(text)
		free(text)
		if err := json.Unmarshal([]byte(raw), &merged); err != nil {
			return nil, fmt.Errorf("zebkit: the parameters are not a JSON object: %w", err)
		}
		if merged == nil {
			merged = map[string]json.RawMessage{}
		}
	}
	for key, value := range members {
		encoded, err := json.Marshal(value)
		if err != nil {
			return nil, err
		}
		merged[key] = encoded
	}
	return json.Marshal(merged)
}

// automationView is the raw view of a command, nil for the browser's; an
// error for a closed view, or before the engine has started (when the
// engine would never answer).
func automationView(view *View) (*C.zebkit_view, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	if view == nil {
		onEngineThread()
		return nil, nil
	}
	raw, ok := view.raw()
	if !ok {
		return nil, ErrClosed
	}
	return raw, nil
}

// CallAutomation runs Playwright's command method on view's page with
// params, a JSON object (nil for {}); done gets the result, or Playwright's
// error. A nil view runs a command of the browser's (Browser.newContext).
func CallAutomation(view *View, method string, params any, done func(result json.RawMessage, err error)) {
	raw, err := automationView(view)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	cparams, err := optionalJSON(params)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	defer free(cparams)
	cmethod := cString(method)
	defer free(cmethod)
	C.zebkit_automation_call(raw, cmethod, cparams, C.zebkit_automation_func(C.zk_result), data(resultOnce(done)))
}

// ScreenshotPixels is a screenshot unencoded: Height rows of Stride bytes,
// RGBA, 8 bits each, straight alpha.
type ScreenshotPixels struct {
	Pixels                []byte
	Width, Height, Stride int
}

// CallAutomationPixels takes "Page.screenshot" (or "ElementHandle.screenshot")
// as pixels instead of a PNG; params as the command's, JSON.
func CallAutomationPixels(view *View, method string, params any, done func(*ScreenshotPixels, error)) {
	raw, ok := view.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	cparams, err := optionalJSON(params)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	defer free(cparams)
	h := onceHandle(func(pixels []byte, width, height, stride int, failure string, isError bool) {
		if done == nil {
			return
		}
		if isError {
			done(nil, &Error{failure})
		} else {
			done(&ScreenshotPixels{pixels, width, height, stride}, nil)
		}
	})
	cmethod := cString(method)
	defer free(cmethod)
	C.zebkit_automation_screenshot_pixels(raw, cmethod, cparams, C.zebkit_automation_pixels_func(C.zk_pixels), data(h))
}

// OnAutomationEvent calls function with the page's events as Playwright's
// protocol has them ("Frame.navigated", "BrowserContext.dialog",
// "Page.close", ...) and their parameters; the last ones can come as the
// view closes, after OnClose's function. nil stops.
func (v *View) OnAutomationEvent(function func(event string, params json.RawMessage)) {
	v.hook("automation-event", function, function, unsafe.Pointer(C.zk_automation_event),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_automation_on_event(raw, C.zebkit_automation_event_func(f), d)
		})
}

// ViewByPageID is the view of a page id ("p2", from BrowserContext.newPage):
// the same View each time.
func ViewByPageID(id string) (*View, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	cid := cString(id)
	defer free(cid)
	view := newView(C.zebkit_automation_page(cid))
	if view == nil {
		return nil, refused("no such page")
	}
	return view, nil
}

// AutomationPageID is the view's page id.
func (v *View) AutomationPageID() string {
	if raw, ok := v.raw(); ok {
		return takeString(C.zebkit_automation_page_id(raw))
	}
	return ""
}

// ListenAutomation serves Playwright's own protocol on a WebSocket on
// 127.0.0.1:port (0 picks one), what chromium.connect(url) connects to. done
// gets the url, which is the credential: give it only to what should drive
// the engine. Before the engine has started, done gets ErrNotStarted.
func ListenAutomation(port int, done func(url string, err error)) {
	if !started.Load() {
		if done != nil {
			done("", ErrNotStarted)
		}
		return
	}
	onEngineThread()
	h := onceHandle(func(url, failure string, isError bool) {
		if done == nil {
			return
		}
		if isError || url == "" {
			if failure == "" {
				failure = "the endpoint did not listen"
			}
			done("", &Error{failure})
		} else {
			done(url, nil)
		}
	})
	C.zebkit_automation_listen(C.int(port), C.zebkit_automation_listen_func(C.zk_listen), data(h))
}

// BrowserContext is a set of pages with their own cookies and storage.
type BrowserContext struct{ ID string }

// NewBrowserContext makes a context with Browser.newContext's options, JSON
// (viewport, userAgent, ...; nil for none).
func NewBrowserContext(options any, done func(*BrowserContext, error)) {
	CallAutomation(nil, "Browser.newContext", options, func(result json.RawMessage, err error) {
		if done == nil {
			return
		}
		if err != nil {
			done(nil, err)
			return
		}
		var answer struct{ Context string }
		if json.Unmarshal(result, &answer) != nil || answer.Context == "" {
			done(nil, refused("no context"))
			return
		}
		done(&BrowserContext{answer.Context}, nil)
	})
}

// Command runs a context's command with params, a JSON object, and the
// context's id.
func (c *BrowserContext) Command(method string, params any, done func(json.RawMessage, error)) {
	merged, err := object(params, map[string]any{"context": c.ID})
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	CallAutomation(nil, method, merged, done)
}

// NewPage opens a page in the context.
func (c *BrowserContext) NewPage(done func(*Page, error)) {
	c.Command("BrowserContext.newPage", nil, func(result json.RawMessage, err error) {
		if done == nil {
			return
		}
		if err != nil {
			done(nil, err)
			return
		}
		var answer struct{ Page string }
		if json.Unmarshal(result, &answer) != nil || answer.Page == "" {
			done(nil, refused("no page"))
			return
		}
		view, err := ViewByPageID(answer.Page)
		if err != nil {
			done(nil, err)
			return
		}
		done(NewPage(view), nil)
	})
}

// Close closes the context and its pages.
func (c *BrowserContext) Close(done func(json.RawMessage, error)) {
	c.Command("BrowserContext.close", nil, done)
}

// Page is a view's page as Playwright drives it.
type Page struct{ View *View }

// NewPage drives view's page.
func NewPage(view *View) *Page { return &Page{view} }

// MainFrame is the page's main frame.
func (p *Page) MainFrame() *Frame { return &Frame{page: p} }

// Frame is the page's frame of id, as Playwright's events name it.
func (p *Page) Frame(id string) *Frame { return &Frame{page: p, id: id} }

// Command runs a page's command with params, a JSON object.
func (p *Page) Command(method string, params any, done func(json.RawMessage, error)) {
	CallAutomation(p.View, method, params, done)
}

// OnEvent is the view's OnAutomationEvent.
func (p *Page) OnEvent(function func(event string, params json.RawMessage)) {
	p.View.OnAutomationEvent(function)
}

// Goto loads url in the main frame.
func (p *Page) Goto(url string, done func(json.RawMessage, error)) { p.MainFrame().Goto(url, done) }

// Locator finds selector in the main frame.
func (p *Page) Locator(selector string) *Locator { return p.MainFrame().Locator(selector) }

// GetByRole finds by ARIA role and, unless "", accessible name.
func (p *Page) GetByRole(role, name string) *Locator { return p.MainFrame().GetByRole(role, name) }

// GetByText finds by text.
func (p *Page) GetByText(text string, exact bool) *Locator {
	return p.MainFrame().GetByText(text, exact)
}

// GetByTestID finds by data-testid.
func (p *Page) GetByTestID(id string) *Locator { return p.MainFrame().GetByTestID(id) }

// EvaluateFunction runs function (a JavaScript function's source) with arg
// (JSON; nil for none) in the main frame; done gets {"value": ...}.
func (p *Page) EvaluateFunction(function string, arg any, done func(json.RawMessage, error)) {
	p.MainFrame().EvaluateFunction(function, arg, done)
}

// Screenshot writes a PNG of the page to path.
func (p *Page) Screenshot(path string, done func(json.RawMessage, error)) {
	p.Command("Page.screenshot", map[string]any{"path": path}, done)
}

// ScreenshotWith takes a screenshot with Page.screenshot's options, JSON;
// the result's "binary" is the image, in base64.
func (p *Page) ScreenshotWith(options any, done func(json.RawMessage, error)) {
	p.Command("Page.screenshot", options, done)
}

// ScreenshotPixels takes a screenshot as pixels, with Page.screenshot's
// options, JSON.
func (p *Page) ScreenshotPixels(options any, done func(*ScreenshotPixels, error)) {
	CallAutomationPixels(p.View, "Page.screenshot", options, done)
}

// Press presses key ("Enter", "Control+A") on the page's keyboard.
func (p *Page) Press(key string, done func(json.RawMessage, error)) {
	p.Command("Page.keyboardPress", map[string]any{"key": key}, done)
}

// TypeText types text on the page's keyboard.
func (p *Page) TypeText(text string, done func(json.RawMessage, error)) {
	p.Command("Page.keyboardType", map[string]any{"text": text}, done)
}

// MouseClick clicks at (x, y), CSS pixels.
func (p *Page) MouseClick(x, y float64, done func(json.RawMessage, error)) {
	p.Command("Page.mouseClick", map[string]any{"x": x, "y": y}, done)
}

// Frame is a frame of a page: its main frame, or one by id.
type Frame struct {
	page *Page
	id   string
}

// Command runs a frame's command with params, a JSON object, and the
// frame's id.
func (f *Frame) Command(method string, params any, done func(json.RawMessage, error)) {
	members := map[string]any{}
	if f.id != "" {
		members["frame"] = f.id
	}
	merged, err := object(params, members)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	f.page.Command(method, merged, done)
}

// Goto loads url in the frame.
func (f *Frame) Goto(url string, done func(json.RawMessage, error)) {
	f.Command("Frame.goto", map[string]any{"url": url}, done)
}

// EvaluateFunction runs function (a JavaScript function's source) with arg
// (JSON; nil for none) in the frame; done gets {"value": ...}.
func (f *Frame) EvaluateFunction(function string, arg any, done func(json.RawMessage, error)) {
	members := map[string]any{"expression": function, "isFunction": true}
	text, err := optionalJSON(arg)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	if text != nil {
		members["arg"] = json.RawMessage(C.GoString(text))
		free(text)
	}
	f.Command("Frame.evaluateExpression", members, done)
}

// Locator finds selector in the frame.
func (f *Frame) Locator(selector string) *Locator { return &Locator{frame: f, selector: selector} }

// GetByRole finds by ARIA role and, unless "", accessible name.
func (f *Frame) GetByRole(role, name string) *Locator {
	if name != "" {
		return f.Locator(fmt.Sprintf("internal:role=%s[name=%s]", role, attributeSelector(name, false)))
	}
	return f.Locator("internal:role=" + role)
}

// GetByText finds by text, exactly or not.
func (f *Frame) GetByText(text string, exact bool) *Locator {
	return f.Locator("internal:text=" + textSelector(text, exact))
}

// GetByLabel finds by a label's text.
func (f *Frame) GetByLabel(text string, exact bool) *Locator {
	return f.Locator("internal:label=" + textSelector(text, exact))
}

// GetByPlaceholder finds by placeholder.
func (f *Frame) GetByPlaceholder(text string, exact bool) *Locator {
	return f.Locator(fmt.Sprintf("internal:attr=[placeholder=%s]", attributeSelector(text, exact)))
}

// GetByTestID finds by data-testid.
func (f *Frame) GetByTestID(id string) *Locator {
	return f.Locator(fmt.Sprintf("internal:testid=[data-testid=%s]", attributeSelector(id, true)))
}

// Selectors as Playwright's locatorUtils.ts makes them.
func textSelector(text string, exact bool) string {
	if exact {
		return quote(text) + "s"
	}
	return quote(text) + "i"
}

func attributeSelector(value string, exact bool) string {
	escaped := "\"" + strings.ReplaceAll(strings.ReplaceAll(value, "\\", "\\\\"), "\"", "\\\"") + "\""
	if exact {
		return escaped + "s"
	}
	return escaped + "i"
}

// Locator is a way to find elements, resolved when a command runs, strictly
// (one element).
type Locator struct {
	frame    *Frame
	selector string
}

// Selector is the locator's Playwright selector.
func (l *Locator) Selector() string { return l.selector }

// Locator finds selector inside the locator's element.
func (l *Locator) Locator(selector string) *Locator {
	return &Locator{frame: l.frame, selector: l.selector + " >> " + selector}
}

// Nth is the index-th element found (-1 for the last).
func (l *Locator) Nth(index int) *Locator { return l.Locator(fmt.Sprintf("nth=%d", index)) }

// First is the first element found.
func (l *Locator) First() *Locator { return l.Nth(0) }

// Last is the last element found.
func (l *Locator) Last() *Locator { return l.Nth(-1) }

// FilterHasText keeps the elements that have text.
func (l *Locator) FilterHasText(text string) *Locator {
	return l.Locator("internal:has-text=" + textSelector(text, false))
}

// ContentFrame is the locator's <iframe>'s document.
func (l *Locator) ContentFrame() *Locator { return l.Locator("internal:control=enter-frame") }

// Command runs a frame's command on the locator's element with params, a
// JSON object, and the selector.
func (l *Locator) Command(method string, params any, done func(json.RawMessage, error)) {
	merged, err := object(params, map[string]any{"selector": l.selector, "strict": true})
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	l.frame.Command(method, merged, done)
}

// value runs a command whose result is {"value": T}.
func value[T any](l *Locator, method string, params any, done func(T, error)) {
	l.Command(method, params, decodeOnce(func(answer struct{ Value T }, err error) {
		if done != nil {
			done(answer.Value, err)
		}
	}))
}

// Click clicks the element.
func (l *Locator) Click(done func(json.RawMessage, error)) { l.Command("Frame.click", nil, done) }

// Dblclick double-clicks the element.
func (l *Locator) Dblclick(done func(json.RawMessage, error)) { l.Command("Frame.dblclick", nil, done) }

// Hover moves the mouse over the element.
func (l *Locator) Hover(done func(json.RawMessage, error)) { l.Command("Frame.hover", nil, done) }

// Check checks a checkbox or radio button.
func (l *Locator) Check(done func(json.RawMessage, error)) { l.Command("Frame.check", nil, done) }

// Uncheck unchecks a checkbox.
func (l *Locator) Uncheck(done func(json.RawMessage, error)) { l.Command("Frame.uncheck", nil, done) }

// Fill sets a field's value.
func (l *Locator) Fill(text string, done func(json.RawMessage, error)) {
	l.Command("Frame.fill", map[string]any{"value": text}, done)
}

// Press presses key in the element.
func (l *Locator) Press(key string, done func(json.RawMessage, error)) {
	l.Command("Frame.press", map[string]any{"key": key}, done)
}

// PressSequentially types text into the element, a key at a time.
func (l *Locator) PressSequentially(text string, done func(json.RawMessage, error)) {
	l.Command("Frame.type", map[string]any{"text": text}, done)
}

// SelectOption selects the <select>'s option of that value or label.
func (l *Locator) SelectOption(valueOrLabel string, done func(json.RawMessage, error)) {
	l.Command("Frame.selectOption", map[string]any{"options": []map[string]string{{"valueOrLabel": valueOrLabel}}}, done)
}

// SetInputFiles gives an <input type=file> the files at paths.
func (l *Locator) SetInputFiles(paths []string, done func(json.RawMessage, error)) {
	if paths == nil {
		paths = []string{}
	}
	l.Command("Frame.setInputFiles", map[string]any{"localPaths": paths}, done)
}

// TextContent is the element's textContent ("" for none).
func (l *Locator) TextContent(done func(string, error)) { value(l, "Frame.textContent", nil, done) }

// InnerText is the element's innerText.
func (l *Locator) InnerText(done func(string, error)) { value(l, "Frame.innerText", nil, done) }

// InputValue is a field's value.
func (l *Locator) InputValue(done func(string, error)) { value(l, "Frame.inputValue", nil, done) }

// GetAttribute is the element's attribute name ("" for none).
func (l *Locator) GetAttribute(name string, done func(string, error)) {
	value(l, "Frame.getAttribute", map[string]any{"name": name}, done)
}

// IsVisible says whether the element is visible.
func (l *Locator) IsVisible(done func(bool, error)) { value(l, "Frame.isVisible", nil, done) }

// Count is the number of elements the locator finds (not strict).
func (l *Locator) Count(done func(int, error)) {
	l.frame.Command("Frame.queryCount", map[string]any{"selector": l.selector},
		decodeOnce(func(answer struct{ Value int }, err error) {
			if done != nil {
				done(answer.Value, err)
			}
		}))
}

// WaitFor waits for state: "attached", "detached", "visible", "hidden".
func (l *Locator) WaitFor(state string, done func(json.RawMessage, error)) {
	l.Command("Frame.waitForSelector", map[string]any{"state": state, "omitReturnValue": true}, done)
}

// AriaSnapshot is the element's accessibility tree: {"snapshot": YAML}.
func (l *Locator) AriaSnapshot(done func(json.RawMessage, error)) {
	l.Command("Frame.ariaSnapshot", nil, done)
}

// Expect checks expression ("to.have.text", "to.be.visible", ...) with
// params (expectedText, timeout, ...), a JSON object, as Playwright's
// expect() does.
func (l *Locator) Expect(expression string, params any, done func(json.RawMessage, error)) {
	merged, err := object(params, map[string]any{"expression": expression, "isNot": false})
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	l.Command("Frame.expect", merged, done)
}
