package zebkit

/*
#include "bridge.h"
#include "bridge_c.h"
*/
import "C"

import (
	"bytes"
	"encoding/json"
	"fmt"
	"runtime/cgo"
	"strings"
)

// Playwright's API run inside the engine (<zebkit/automation.h>,
// docs/AUTOMATION.md): commands as Playwright's protocol names them, on a
// view's page, without CDP or Node.

// AutomationCommands lists the commands this engine has.
func AutomationCommands() []string {
	var names []string
	takeJSON(C.zebkit_automation_commands(), &names)
	return names
}

// quote is text as a JSON string, as Playwright's selectors are written.
func quote(text string) string {
	var out bytes.Buffer
	encoder := json.NewEncoder(&out)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(text)
	return strings.TrimSuffix(out.String(), "\n")
}

// automationDone is the handle of a command's answer, run once.
func automationDone(done func(string, error)) C.uintptr_t {
	var h C.uintptr_t
	h = handle(func(result, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if done == nil {
			return
		}
		if failed {
			done("", &Error{failure})
		} else {
			done(result, nil)
		}
	})
	return h
}

func automationCall(view *C.zebkit_view, method, params string, done func(string, error)) {
	cmethod := cString(method)
	defer free(cmethod)
	cparams := optionalString(params)
	defer free(cparams)
	h := automationDone(done)
	C.zebkit_automation_call(view, cmethod, cparams, C.zebkit_automation_func(C.zk_result), data(h))
}

// AutomationCall runs Playwright's command method on view's page with params,
// a JSON object ("" for {}); done gets the result as JSON, or Playwright's
// error. A nil view runs a command of the browser's (Browser.newContext).
func AutomationCall(view *View, method, params string, done func(result string, err error)) {
	var raw *C.zebkit_view
	if view != nil {
		raw = view.view
	}
	automationCall(raw, method, params, done)
}

// ScreenshotPixels is a screenshot unencoded: Height rows of Stride bytes,
// RGBA, 8 bits each, straight alpha.
type ScreenshotPixels struct {
	Pixels                []byte
	Width, Height, Stride int
}

// AutomationScreenshotPixels takes "Page.screenshot" (or
// "ElementHandle.screenshot") as pixels instead of a PNG.
func AutomationScreenshotPixels(view *View, method, params string, done func(*ScreenshotPixels, error)) {
	var h C.uintptr_t
	h = handle(func(pixels []byte, width, height, stride int, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if failed {
			done(nil, &Error{failure})
		} else {
			done(&ScreenshotPixels{pixels, width, height, stride}, nil)
		}
	})
	cmethod := cString(method)
	defer free(cmethod)
	cparams := optionalString(params)
	defer free(cparams)
	C.zebkit_automation_screenshot_pixels(view.view, cmethod, cparams,
		C.zebkit_automation_pixels_func(C.zkc_pixels), data(h))
}

// OnAutomationEvent calls function with the page's events as Playwright's
// protocol has them ("Frame.navigated", "BrowserContext.dialog", ...) and
// their parameters as JSON; nil stops.
func (v *View) OnAutomationEvent(function func(event, json string)) {
	if function == nil {
		C.zebkit_automation_on_event(v.view, nil, nil)
		v.slot("automation-event").clear()
		return
	}
	h := v.slot("automation-event").set(function)
	C.zebkit_automation_on_event(v.view, C.zebkit_automation_event_func(C.zkc_automation_event), data(h))
}

// AutomationPage is the view of a page id ("p2", from BrowserContext.newPage),
// or nil.
func AutomationPage(id string) *View {
	cid := cString(id)
	defer free(cid)
	view := C.zebkit_automation_page(cid)
	if view == nil {
		return nil
	}
	return newView(view)
}

// AutomationPageID is the view's page id.
func (v *View) AutomationPageID() string { return takeString(C.zebkit_automation_page_id(v.view)) }

// AutomationListen serves Playwright's own protocol on a WebSocket on
// 127.0.0.1:port (0 picks one), what chromium.connect(url) connects to. done
// gets the url, which is the credential: give it only to what should drive
// the engine.
func AutomationListen(port int, done func(url string, err error)) {
	var h C.uintptr_t
	h = handle(func(url, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if failed {
			done("", &Error{failure})
		} else {
			done(url, nil)
		}
	})
	C.zebkit_automation_listen(C.int(port), C.zebkit_automation_listen_func(C.zkc_listen), data(h))
}

// BrowserContext is a set of pages with their own cookies and storage.
type BrowserContext struct{ ID string }

// NewBrowserContext makes a context with Browser.newContext's options
// (viewport, userAgent, ...; "" for none).
func NewBrowserContext(options string, done func(*BrowserContext, error)) {
	automationCall(nil, "Browser.newContext", options, func(result string, err error) {
		if err != nil {
			done(nil, err)
			return
		}
		var answer struct{ Context string }
		if json.Unmarshal([]byte(result), &answer) != nil || answer.Context == "" {
			done(nil, &Error{"no context"})
			return
		}
		done(&BrowserContext{answer.Context}, nil)
	})
}

// Command runs a context's command, with members added to {"context": id}.
func (c *BrowserContext) Command(method, members string, done func(string, error)) {
	params := fmt.Sprintf("{\"context\":%s}", quote(c.ID))
	if members != "" {
		params = fmt.Sprintf("{\"context\":%s,%s}", quote(c.ID), members)
	}
	automationCall(nil, method, params, done)
}

// NewPage opens a page in the context.
func (c *BrowserContext) NewPage(done func(*Page, error)) {
	c.Command("BrowserContext.newPage", "", func(result string, err error) {
		if err != nil {
			done(nil, err)
			return
		}
		var answer struct{ Page string }
		if json.Unmarshal([]byte(result), &answer) != nil || answer.Page == "" {
			done(nil, &Error{"no page"})
			return
		}
		view := AutomationPage(answer.Page)
		if view == nil {
			done(nil, &Error{"the page is closed"})
			return
		}
		done(NewPage(view), nil)
	})
}

// Close closes the context and its pages.
func (c *BrowserContext) Close(done func(string, error)) { c.Command("BrowserContext.close", "", done) }

// Page is a view's page as Playwright drives it.
type Page struct{ View *View }

func NewPage(view *View) *Page { return &Page{view} }

func (p *Page) MainFrame() *PageFrame      { return &PageFrame{page: p} }
func (p *Page) Frame(id string) *PageFrame { return &PageFrame{page: p, id: id} }

// Command runs a page's command with params, a JSON object.
func (p *Page) Command(method, params string, done func(string, error)) {
	automationCall(p.View.view, method, params, done)
}

func (p *Page) OnEvent(function func(event, json string)) { p.View.OnAutomationEvent(function) }
func (p *Page) Goto(url string, done func(string, error)) { p.MainFrame().Goto(url, done) }
func (p *Page) Locator(selector string) *Locator          { return p.MainFrame().Locator(selector) }
func (p *Page) GetByRole(role, name string) *Locator      { return p.MainFrame().GetByRole(role, name) }
func (p *Page) GetByText(text string, exact bool) *Locator {
	return p.MainFrame().GetByText(text, exact)
}
func (p *Page) GetByTestID(id string) *Locator { return p.MainFrame().GetByTestID(id) }

// Evaluate runs function (a JavaScript function's source) with arg (JSON, or
// "" for none) in the main frame.
func (p *Page) Evaluate(function, arg string, done func(string, error)) {
	p.MainFrame().Evaluate(function, arg, done)
}

// Screenshot writes a PNG of the page to path.
func (p *Page) Screenshot(path string, done func(string, error)) {
	p.Command("Page.screenshot", fmt.Sprintf("{\"path\":%s}", quote(path)), done)
}

// ScreenshotWith takes a screenshot with Page.screenshot's options; the
// result's "binary" is the image, in base64.
func (p *Page) ScreenshotWith(options string, done func(string, error)) {
	p.Command("Page.screenshot", options, done)
}

// ScreenshotPixels takes a screenshot as pixels.
func (p *Page) ScreenshotPixels(options string, done func(*ScreenshotPixels, error)) {
	AutomationScreenshotPixels(p.View, "Page.screenshot", options, done)
}

func (p *Page) Press(key string, done func(string, error)) {
	p.Command("Page.keyboardPress", fmt.Sprintf("{\"key\":%s}", quote(key)), done)
}

func (p *Page) TypeText(text string, done func(string, error)) {
	p.Command("Page.keyboardType", fmt.Sprintf("{\"text\":%s}", quote(text)), done)
}

func (p *Page) MouseClick(x, y float64, done func(string, error)) {
	p.Command("Page.mouseClick", fmt.Sprintf("{\"x\":%v,\"y\":%v}", x, y), done)
}

// PageFrame is a frame of a page: its main frame, or one by id.
type PageFrame struct {
	page *Page
	id   string
}

func (f *PageFrame) params(members string) string {
	switch {
	case f.id != "" && members == "":
		return fmt.Sprintf("{\"frame\":%s}", quote(f.id))
	case f.id != "":
		return fmt.Sprintf("{\"frame\":%s,%s}", quote(f.id), members)
	default:
		return "{" + members + "}"
	}
}

// Command runs a frame's command; members are JSON members added to the frame.
func (f *PageFrame) Command(method, members string, done func(string, error)) {
	f.page.Command(method, f.params(members), done)
}

func (f *PageFrame) Goto(url string, done func(string, error)) {
	f.Command("Frame.goto", "\"url\":"+quote(url), done)
}

func (f *PageFrame) Evaluate(function, arg string, done func(string, error)) {
	members := fmt.Sprintf("\"expression\":%s,\"isFunction\":true", quote(function))
	if arg != "" {
		members += ",\"arg\":" + arg
	}
	f.Command("Frame.evaluateExpression", members, done)
}

func (f *PageFrame) Locator(selector string) *Locator { return &Locator{frame: f, selector: selector} }

// GetByRole finds by ARIA role and, unless "", accessible name.
func (f *PageFrame) GetByRole(role, name string) *Locator {
	if name != "" {
		return f.Locator(fmt.Sprintf("internal:role=%s[name=%s]", role, attributeSelector(name, false)))
	}
	return f.Locator("internal:role=" + role)
}

func (f *PageFrame) GetByText(text string, exact bool) *Locator {
	return f.Locator("internal:text=" + textSelector(text, exact))
}

func (f *PageFrame) GetByLabel(text string, exact bool) *Locator {
	return f.Locator("internal:label=" + textSelector(text, exact))
}

func (f *PageFrame) GetByPlaceholder(text string, exact bool) *Locator {
	return f.Locator(fmt.Sprintf("internal:attr=[placeholder=%s]", attributeSelector(text, exact)))
}

func (f *PageFrame) GetByTestID(id string) *Locator {
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
	frame    *PageFrame
	selector string
}

func (l *Locator) Selector() string { return l.selector }

func (l *Locator) Locator(selector string) *Locator {
	return &Locator{frame: l.frame, selector: l.selector + " >> " + selector}
}

func (l *Locator) Nth(index int) *Locator { return l.Locator(fmt.Sprintf("nth=%d", index)) }
func (l *Locator) First() *Locator        { return l.Nth(0) }
func (l *Locator) Last() *Locator         { return l.Nth(-1) }
func (l *Locator) FilterHasText(text string) *Locator {
	return l.Locator("internal:has-text=" + textSelector(text, false))
}
func (l *Locator) ContentFrame() *Locator { return l.Locator("internal:control=enter-frame") }

// Command runs a frame's command on the locator's element; members are
// JSON members added to its selector.
func (l *Locator) Command(method, members string, done func(string, error)) {
	selector := fmt.Sprintf("\"selector\":%s,\"strict\":true", quote(l.selector))
	if members != "" {
		selector += "," + members
	}
	l.frame.Command(method, selector, done)
}

func (l *Locator) Click(done func(string, error))    { l.Command("Frame.click", "", done) }
func (l *Locator) Dblclick(done func(string, error)) { l.Command("Frame.dblclick", "", done) }
func (l *Locator) Hover(done func(string, error))    { l.Command("Frame.hover", "", done) }
func (l *Locator) Check(done func(string, error))    { l.Command("Frame.check", "", done) }
func (l *Locator) Uncheck(done func(string, error))  { l.Command("Frame.uncheck", "", done) }
func (l *Locator) Fill(value string, done func(string, error)) {
	l.Command("Frame.fill", "\"value\":"+quote(value), done)
}
func (l *Locator) Press(key string, done func(string, error)) {
	l.Command("Frame.press", "\"key\":"+quote(key), done)
}
func (l *Locator) PressSequentially(text string, done func(string, error)) {
	l.Command("Frame.type", "\"text\":"+quote(text), done)
}
func (l *Locator) SelectOption(valueOrLabel string, done func(string, error)) {
	l.Command("Frame.selectOption", fmt.Sprintf("\"options\":[{\"valueOrLabel\":%s}]", quote(valueOrLabel)), done)
}
func (l *Locator) SetInputFiles(paths []string, done func(string, error)) {
	quoted := make([]string, len(paths))
	for i, path := range paths {
		quoted[i] = quote(path)
	}
	l.Command("Frame.setInputFiles", "\"localPaths\":["+strings.Join(quoted, ",")+"]", done)
}
func (l *Locator) TextContent(done func(string, error)) { l.Command("Frame.textContent", "", done) }
func (l *Locator) InnerText(done func(string, error))   { l.Command("Frame.innerText", "", done) }
func (l *Locator) InputValue(done func(string, error))  { l.Command("Frame.inputValue", "", done) }
func (l *Locator) GetAttribute(name string, done func(string, error)) {
	l.Command("Frame.getAttribute", "\"name\":"+quote(name), done)
}
func (l *Locator) IsVisible(done func(string, error)) { l.Command("Frame.isVisible", "", done) }

// Count is the number of elements the locator finds (not strict).
func (l *Locator) Count(done func(string, error)) {
	l.frame.Command("Frame.queryCount", "\"selector\":"+quote(l.selector), done)
}

// WaitFor waits for state: "attached", "detached", "visible", "hidden".
func (l *Locator) WaitFor(state string, done func(string, error)) {
	l.Command("Frame.waitForSelector", fmt.Sprintf("\"state\":%s,\"omitReturnValue\":true", quote(state)), done)
}

func (l *Locator) AriaSnapshot(done func(string, error)) { l.Command("Frame.ariaSnapshot", "", done) }

// Expect checks expression ("to.have.text", "to.be.visible", ...) with
// members (expectedText, timeout, ...), as Playwright's expect() does.
func (l *Locator) Expect(expression, members string, done func(string, error)) {
	options := fmt.Sprintf("\"expression\":%s,\"isNot\":false", quote(expression))
	if members != "" {
		options += "," + members
	}
	l.Command("Frame.expect", options, done)
}
