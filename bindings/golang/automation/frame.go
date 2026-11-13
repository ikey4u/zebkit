package automation

import (
	"encoding/base64"
	"encoding/json"
	"fmt"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// Frame is a frame of a page: its id, "" for the main frame.
type Frame struct {
	state     *state
	id        string
	name, url string
}

// ID is the frame's id; "" for the main frame.
func (f *Frame) ID() string { return f.id }

// Name is the frame's name as Page.Frames found it; "" for a frame made
// otherwise.
func (f *Frame) Name() string { return f.name }

// URL is the frame's URL as Page.Frames found it; "" for a frame made
// otherwise (Page.URL is the page's).
func (f *Frame) URL() string { return f.url }

// Page is the page the frame is of.
func (f *Frame) Page() *Page {
	if page, ok := pages[f.state.view]; ok && page.state == f.state {
		return page
	}
	return pageOf(f.state)
}

// run runs method with params and the frame's id.
func (f *Frame) run(method string, params object, done func(answer, error)) {
	if params == nil {
		params = object{}
	}
	if f.id != "" {
		params["frame"] = f.id
	}
	run(f.state.view, method, params, done)
}

// Command runs one of the frame's commands ("Frame.*") with params, a JSON
// object (nil for none), and the frame's id.
func (f *Frame) Command(method string, params any, done func(json.RawMessage, error)) {
	members := object{}
	if f.id != "" {
		members["frame"] = f.id
	}
	command(f.state.view, method, params, members, done)
}

// Goto navigates the frame; done gets the main resource's response, if there
// was one.
func (f *Frame) Goto(url string, done func(*Response, error)) { f.GotoWith(url, nil, done) }

// GotoWith is Goto with options: waitUntil, timeout, referer.
func (f *Frame) GotoWith(url string, options any, done func(*Response, error)) {
	params, err := with(options, object{"url": url})
	if err != nil {
		failed(done, err)
		return
	}
	s := f.state
	f.run("Frame.goto", params, then(done, func(a answer) (*Response, error) { return responseOf(s, a["response"]), nil }))
}

// Evaluate is the value of expression (or of the function it is, called
// with arg, encoded with encoding/json; a *JSHandle stands for its object).
func (f *Frame) Evaluate(expression string, arg any, done func(json.RawMessage, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	f.run("Frame.evaluateExpression", params, rawValue(done))
}

// EvaluateHandle is Evaluate's value as a handle.
func (f *Frame) EvaluateHandle(expression string, arg any, done func(*JSHandle, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	f.run("Frame.evaluateExpressionHandle", params, then(done, func(a answer) (*JSHandle, error) {
		return handleOf(f, a["handle"]), nil
	}))
}

// Content is the frame's HTML.
func (f *Frame) Content(done func(string, error)) { f.run("Frame.content", nil, textValue(done)) }

// SetContent sets the frame's HTML; options: waitUntil, timeout.
func (f *Frame) SetContent(html string, options any, done func(error)) {
	params, err := with(options, object{"html": html})
	if err != nil {
		failedUnit(done, err)
		return
	}
	f.run("Frame.setContent", params, unit(done))
}

// Title is the frame's document's title.
func (f *Frame) Title(done func(string, error)) { f.run("Frame.title", nil, textValue(done)) }

// WaitForSelector waits for selector; options: state, timeout, strict.
func (f *Frame) WaitForSelector(selector string, options any, done func(*ElementHandle, error)) {
	params, err := with(options, object{"selector": selector})
	if err != nil {
		failed(done, err)
		return
	}
	f.run("Frame.waitForSelector", params, then(done, func(a answer) (*ElementHandle, error) {
		return elementOf(f, a["element"]), nil
	}))
}

// WaitForFunction waits until expression (called with arg) is truthy;
// options: pollingInterval, timeout.
func (f *Frame) WaitForFunction(expression string, arg any, options any, done func(*JSHandle, error)) {
	params, err := evaluation(expression, arg)
	if err == nil {
		params, err = with(options, params)
	}
	if err != nil {
		failed(done, err)
		return
	}
	f.run("Frame.waitForFunction", params, then(done, func(a answer) (*JSHandle, error) {
		return handleOf(f, a["handle"]), nil
	}))
}

// WaitForLoadState waits for state: "load" (""), "domcontentloaded",
// "networkidle"; timeout in milliseconds, 0 for the default.
func (f *Frame) WaitForLoadState(state string, timeout float64, done func(error)) {
	params := object{"timeout": milliseconds(timeout)}
	if state != "" {
		params["state"] = state
	}
	f.run("Frame.waitForLoadState", params, unit(done))
}

// WaitForTimeout waits timeout milliseconds.
func (f *Frame) WaitForTimeout(timeout float64, done func(error)) {
	f.run("Frame.waitForTimeout", object{"waitTimeout": timeout}, unit(done))
}

// QuerySelector is the first element of selector, nil for none.
func (f *Frame) QuerySelector(selector string, done func(*ElementHandle, error)) {
	f.run("Frame.querySelector", object{"selector": selector}, then(done, func(a answer) (*ElementHandle, error) {
		return elementOf(f, a["element"]), nil
	}))
}

// QuerySelectorAll is every element of selector.
func (f *Frame) QuerySelectorAll(selector string, done func([]*ElementHandle, error)) {
	f.run("Frame.querySelectorAll", object{"selector": selector}, then(done, func(a answer) ([]*ElementHandle, error) {
		return elementsOf(f, a.list("elements")), nil
	}))
}

// EvalOnSelector is the function expression called with selector's element
// and arg.
func (f *Frame) EvalOnSelector(selector, expression string, arg any, done func(json.RawMessage, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	params["selector"], params["strict"] = selector, true
	f.run("Frame.evalOnSelector", params, rawValue(done))
}

// EvalOnSelectorAll is the function expression called with every element of
// selector and arg.
func (f *Frame) EvalOnSelectorAll(selector, expression string, arg any, done func(json.RawMessage, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	params["selector"] = selector
	f.run("Frame.evalOnSelectorAll", params, rawValue(done))
}

func optionalText(text string) any {
	if text == "" {
		return nil
	}
	return text
}

// AddScriptTag adds a <script> of url or with content ("" for none); kind is
// its type ("module", "" for a classic one).
func (f *Frame) AddScriptTag(url, content, kind string, done func(*ElementHandle, error)) {
	params := object{"url": optionalText(url), "content": optionalText(content), "type": optionalText(kind)}
	f.run("Frame.addScriptTag", params, then(done, func(a answer) (*ElementHandle, error) {
		return elementOf(f, a["element"]), nil
	}))
}

// AddStyleTag adds a <style> of url or with content.
func (f *Frame) AddStyleTag(url, content string, done func(*ElementHandle, error)) {
	params := object{"url": optionalText(url), "content": optionalText(content)}
	f.run("Frame.addStyleTag", params, then(done, func(a answer) (*ElementHandle, error) {
		return elementOf(f, a["element"]), nil
	}))
}

// DragAndDrop drags source (a selector) onto target; options as
// Playwright's.
func (f *Frame) DragAndDrop(source, target string, options any, done func(error)) {
	params, err := with(options, object{"source": source, "target": target, "strict": true})
	if err != nil {
		failedUnit(done, err)
		return
	}
	f.run("Frame.dragAndDrop", params, unit(done))
}

// Locator finds selector in the frame.
func (f *Frame) Locator(selector string) *Locator { return &Locator{frame: f, selector: selector} }

// FrameLocator is the frame of the <iframe> of selector.
func (f *Frame) FrameLocator(selector string) *FrameLocator {
	return &FrameLocator{frame: f, selector: selector}
}

// GetByRole finds elements by their ARIA role, and the options' name and
// states.
func (f *Frame) GetByRole(role string, options RoleOptions) *Locator {
	return f.Locator(options.selector(role))
}

// GetByText finds elements by their text (a string: a substring ignoring
// case, unless exact; or a *regexp.Regexp).
func (f *Frame) GetByText(text any, exact bool) *Locator { return f.Locator(byText(text, exact)) }

// GetByLabel finds inputs by their label's text (or aria-label).
func (f *Frame) GetByLabel(text any, exact bool) *Locator { return f.Locator(byLabel(text, exact)) }

// GetByPlaceholder finds inputs by their placeholder.
func (f *Frame) GetByPlaceholder(text any, exact bool) *Locator {
	return f.Locator(byAttribute("placeholder", text, exact))
}

// GetByAltText finds elements by their alt text.
func (f *Frame) GetByAltText(text any, exact bool) *Locator {
	return f.Locator(byAttribute("alt", text, exact))
}

// GetByTitle finds elements by their title.
func (f *Frame) GetByTitle(text any, exact bool) *Locator {
	return f.Locator(byAttribute("title", text, exact))
}

// GetByTestID finds elements by their data-testid.
func (f *Frame) GetByTestID(id string) *Locator { return f.Locator(byTestID(id)) }

func byText(text any, exact bool) string  { return "internal:text=" + textSelector(text, exact) }
func byLabel(text any, exact bool) string { return "internal:label=" + textSelector(text, exact) }
func byAttribute(name string, text any, exact bool) string {
	return fmt.Sprintf("internal:attr=[%s=%s]", name, attributeSelector(text, exact))
}
func byTestID(id string) string {
	return "internal:testid=[data-testid=" + attributeSelector(id, true) + "]"
}

// ---- Locators ---------------------------------------------------------------------

// Locator is Playwright's Locator: a selector in a frame, resolved at each
// use, strictly (one element) for an action.
type Locator struct {
	frame    *Frame
	selector string
	// What its commands are given besides their own params.
	options object
}

// Selector is the locator's Playwright selector.
func (l *Locator) Selector() string { return l.selector }

// Frame is the frame the locator finds in.
func (l *Locator) Frame() *Frame { return l.frame }

func (l *Locator) withSelector(selector string) *Locator {
	return &Locator{frame: l.frame, selector: selector, options: l.options}
}

// WithOptions is the same locator, whose commands are also given options, a
// JSON object (the protocol's: timeout, force, noWaitAfter, position,
// modifiers, button, clickCount, delay, trial). Options it cannot read are
// left out.
func (l *Locator) WithOptions(opts any) *Locator {
	o, err := options(opts)
	if err != nil {
		o = object{}
	}
	return &Locator{frame: l.frame, selector: l.selector, options: o}
}

// WithTimeout is WithOptions with a timeout added, in milliseconds.
func (l *Locator) WithTimeout(timeout float64) *Locator {
	o, _ := options(l.options)
	o["timeout"] = timeout
	return &Locator{frame: l.frame, selector: l.selector, options: o}
}

// Locator finds selector inside the locator's elements.
func (l *Locator) Locator(selector string) *Locator {
	return l.withSelector(l.selector + " >> " + selector)
}

// LocatorOf finds other inside the locator's elements.
func (l *Locator) LocatorOf(other *Locator) *Locator {
	return l.withSelector(l.selector + " >> internal:chain=" + quote(other.selector))
}

// Nth is the index-th element found (-1 for the last).
func (l *Locator) Nth(index int) *Locator { return l.Locator(fmt.Sprintf("nth=%d", index)) }

// First is the first element found.
func (l *Locator) First() *Locator { return l.Nth(0) }

// Last is the last element found.
func (l *Locator) Last() *Locator { return l.Nth(-1) }

// Filter keeps the elements filter says.
func (l *Locator) Filter(filter Filter) *Locator {
	selector := l.selector
	if filter.HasText != nil {
		selector += " >> internal:has-text=" + textSelector(filter.HasText, false)
	}
	if filter.HasNotText != nil {
		selector += " >> internal:has-not-text=" + textSelector(filter.HasNotText, false)
	}
	if filter.Has != nil {
		selector += " >> internal:has=" + quote(filter.Has.selector)
	}
	if filter.HasNot != nil {
		selector += " >> internal:has-not=" + quote(filter.HasNot.selector)
	}
	if filter.Visible != nil {
		selector += fmt.Sprintf(" >> visible=%t", *filter.Visible)
	}
	return l.withSelector(selector)
}

// And is the elements both locators find.
func (l *Locator) And(other *Locator) *Locator {
	return l.withSelector(l.selector + " >> internal:and=" + quote(other.selector))
}

// Or is the elements either locator finds.
func (l *Locator) Or(other *Locator) *Locator {
	return l.withSelector(l.selector + " >> internal:or=" + quote(other.selector))
}

// ContentFrame is the frame of this <iframe> locator, as Playwright's
// contentFrame.
func (l *Locator) ContentFrame() *FrameLocator {
	return &FrameLocator{frame: l.frame, selector: l.selector}
}

// FrameLocator is the frame of the <iframe> of selector inside the locator.
func (l *Locator) FrameLocator(selector string) *FrameLocator {
	return &FrameLocator{frame: l.frame, selector: l.selector + " >> " + selector}
}

// GetByRole is Frame.GetByRole inside the locator.
func (l *Locator) GetByRole(role string, options RoleOptions) *Locator {
	return l.Locator(options.selector(role))
}

// GetByText is Frame.GetByText inside the locator.
func (l *Locator) GetByText(text any, exact bool) *Locator { return l.Locator(byText(text, exact)) }

// GetByLabel is Frame.GetByLabel inside the locator.
func (l *Locator) GetByLabel(text any, exact bool) *Locator { return l.Locator(byLabel(text, exact)) }

// GetByPlaceholder is Frame.GetByPlaceholder inside the locator.
func (l *Locator) GetByPlaceholder(text any, exact bool) *Locator {
	return l.Locator(byAttribute("placeholder", text, exact))
}

// GetByAltText is Frame.GetByAltText inside the locator.
func (l *Locator) GetByAltText(text any, exact bool) *Locator {
	return l.Locator(byAttribute("alt", text, exact))
}

// GetByTitle is Frame.GetByTitle inside the locator.
func (l *Locator) GetByTitle(text any, exact bool) *Locator {
	return l.Locator(byAttribute("title", text, exact))
}

// GetByTestID is Frame.GetByTestID inside the locator.
func (l *Locator) GetByTestID(id string) *Locator { return l.Locator(byTestID(id)) }

// Command runs method with the selector, strictly, the locator's options and
// params, a JSON object (nil for none).
func (l *Locator) Command(method string, params any, done func(json.RawMessage, error)) {
	all, err := l.params(params, true)
	if err != nil {
		failed(done, err)
		return
	}
	members := object{}
	if l.frame.id != "" {
		members["frame"] = l.frame.id
	}
	command(l.frame.state.view, method, all, members, done)
}

func (l *Locator) params(params any, strict bool) (object, error) {
	all, _ := options(l.options)
	extra, err := options(params)
	if err != nil {
		return nil, err
	}
	for k, v := range extra {
		all[k] = v
	}
	all["selector"], all["strict"] = l.selector, strict
	return all, nil
}

func (l *Locator) run(method string, params object, strict bool, done func(answer, error)) {
	all, err := l.params(params, strict)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	l.frame.run(method, all, done)
}

func (l *Locator) act(method string, params object, done func(error)) {
	l.run(method, params, true, unit(done))
}

// Click clicks the element.
func (l *Locator) Click(done func(error)) { l.act("Frame.click", nil, done) }

// Dblclick double-clicks the element.
func (l *Locator) Dblclick(done func(error)) { l.act("Frame.dblclick", nil, done) }

// Hover moves the mouse over the element.
func (l *Locator) Hover(done func(error)) { l.act("Frame.hover", nil, done) }

// Tap taps the element.
func (l *Locator) Tap(done func(error)) { l.act("Frame.tap", nil, done) }

// Check checks a checkbox or radio button.
func (l *Locator) Check(done func(error)) { l.act("Frame.check", nil, done) }

// Uncheck unchecks a checkbox.
func (l *Locator) Uncheck(done func(error)) { l.act("Frame.uncheck", nil, done) }

// SetChecked checks or unchecks.
func (l *Locator) SetChecked(checked bool, done func(error)) {
	if checked {
		l.Check(done)
	} else {
		l.Uncheck(done)
	}
}

// Fill sets a field's value.
func (l *Locator) Fill(value string, done func(error)) {
	l.act("Frame.fill", object{"value": value}, done)
}

// Clear empties a field.
func (l *Locator) Clear(done func(error)) { l.Fill("", done) }

// Press presses key ("Enter", "Control+A") in the element.
func (l *Locator) Press(key string, done func(error)) { l.act("Frame.press", object{"key": key}, done) }

// PressSequentially types text into the element, a key at a time.
func (l *Locator) PressSequentially(text string, done func(error)) {
	l.act("Frame.type", object{"text": text}, done)
}

// Type is PressSequentially (Playwright's older name).
func (l *Locator) Type(text string, done func(error)) { l.PressSequentially(text, done) }

// Focus focuses the element.
func (l *Locator) Focus(done func(error)) { l.act("Frame.focus", nil, done) }

// Blur takes the focus from the element.
func (l *Locator) Blur(done func(error)) { l.act("Frame.blur", nil, done) }

// SelectOption is an option of a <select> to choose: by ValueOrLabel, Value,
// Label or Index (the first one set).
type SelectOption struct {
	ValueOrLabel string
	Value        string
	Label        string
	Index        *int
}

func (o SelectOption) protocol() object {
	switch {
	case o.ValueOrLabel != "":
		return object{"valueOrLabel": o.ValueOrLabel}
	case o.Value != "":
		return object{"value": o.Value}
	case o.Label != "":
		return object{"label": o.Label}
	case o.Index != nil:
		return object{"index": *o.Index}
	}
	return object{"valueOrLabel": ""}
}

// SelectOption selects the options; done gets the values selected.
func (l *Locator) SelectOption(options []SelectOption, done func([]string, error)) {
	chosen := []object{}
	for _, option := range options {
		chosen = append(chosen, option.protocol())
	}
	l.run("Frame.selectOption", object{"options": chosen}, true, then(done, func(a answer) ([]string, error) {
		return stringList(a["values"]), nil
	}))
}

// SetInputFiles gives an <input type=file> the files at paths.
func (l *Locator) SetInputFiles(paths []string, done func(error)) {
	l.act("Frame.setInputFiles", object{"localPaths": absolutes(paths)}, done)
}

// FilePayload is a file of the application's, for SetInputFilePayloads.
type FilePayload struct {
	Name     string
	MimeType string
	Buffer   []byte
}

// SetInputFilePayloads gives an <input type=file> the application's files.
func (l *Locator) SetInputFilePayloads(files []FilePayload, done func(error)) {
	payloads := []object{}
	for _, file := range files {
		payloads = append(payloads, object{"name": file.Name, "mimeType": file.MimeType,
			"buffer": base64.StdEncoding.EncodeToString(file.Buffer)})
	}
	l.act("Frame.setInputFiles", object{"payloads": payloads}, done)
}

// DispatchEvent dispatches an event of kind ("click") with eventInit, JSON.
func (l *Locator) DispatchEvent(kind string, eventInit any, done func(error)) {
	init, err := jsonText(eventInit)
	if err != nil {
		failedUnit(done, err)
		return
	}
	l.act("Frame.dispatchEvent", object{"type": kind, "eventInit": init}, done)
}

// DragTo drags the element onto target's.
func (l *Locator) DragTo(target *Locator, done func(error)) {
	params, _ := options(l.options)
	params["source"], params["target"], params["strict"] = l.selector, target.selector, true
	l.frame.run("Frame.dragAndDrop", params, unit(done))
}

// TextContent is the element's textContent (nil for none).
func (l *Locator) TextContent(done func(*string, error)) {
	l.run("Frame.textContent", nil, true, stringValue(done))
}

// InnerText is the element's innerText.
func (l *Locator) InnerText(done func(string, error)) {
	l.run("Frame.innerText", nil, true, textValue(done))
}

// InnerHTML is the element's innerHTML.
func (l *Locator) InnerHTML(done func(string, error)) {
	l.run("Frame.innerHTML", nil, true, textValue(done))
}

// GetAttribute is the element's attribute name (nil for none).
func (l *Locator) GetAttribute(name string, done func(*string, error)) {
	l.run("Frame.getAttribute", object{"name": name}, true, stringValue(done))
}

// InputValue is a field's value.
func (l *Locator) InputValue(done func(string, error)) {
	l.run("Frame.inputValue", nil, true, textValue(done))
}

// IsVisible says whether the element is visible.
func (l *Locator) IsVisible(done func(bool, error)) {
	l.run("Frame.isVisible", nil, true, boolValue(done))
}

// IsHidden says whether the element is hidden.
func (l *Locator) IsHidden(done func(bool, error)) {
	l.run("Frame.isHidden", nil, true, boolValue(done))
}

// IsEnabled says whether the element is enabled.
func (l *Locator) IsEnabled(done func(bool, error)) {
	l.run("Frame.isEnabled", nil, true, boolValue(done))
}

// IsDisabled says whether the element is disabled.
func (l *Locator) IsDisabled(done func(bool, error)) {
	l.run("Frame.isDisabled", nil, true, boolValue(done))
}

// IsChecked says whether the element is checked.
func (l *Locator) IsChecked(done func(bool, error)) {
	l.run("Frame.isChecked", nil, true, boolValue(done))
}

// IsEditable says whether the element is editable.
func (l *Locator) IsEditable(done func(bool, error)) {
	l.run("Frame.isEditable", nil, true, boolValue(done))
}

// Count is the number of elements the locator finds (not strict).
func (l *Locator) Count(done func(int, error)) {
	l.run("Frame.queryCount", nil, false, then(done, func(a answer) (int, error) {
		n, _ := a.number("value")
		return int(n), nil
	}))
}

// All is a locator for each element it finds now.
func (l *Locator) All(done func([]*Locator, error)) {
	l.Count(func(count int, err error) {
		if err != nil {
			failed(done, err)
			return
		}
		all := []*Locator{}
		for i := 0; i < count; i++ {
			all = append(all, l.Nth(i))
		}
		if done != nil {
			done(all, nil)
		}
	})
}

// AllTextContents is the textContent of each element it finds.
func (l *Locator) AllTextContents(done func([]string, error)) {
	l.EvaluateAll("es => es.map(e => e.textContent || '')", nil, func(value json.RawMessage, err error) {
		if done != nil {
			done(stringList(value), err)
		}
	})
}

// AllInnerTexts is the innerText of each element it finds.
func (l *Locator) AllInnerTexts(done func([]string, error)) {
	l.EvaluateAll("es => es.map(e => e.innerText)", nil, func(value json.RawMessage, err error) {
		if done != nil {
			done(stringList(value), err)
		}
	})
}

// Evaluate is the function expression called with the element and arg.
func (l *Locator) Evaluate(expression string, arg any, done func(json.RawMessage, error)) {
	withElement(l, func(element *ElementHandle, finish func(json.RawMessage, error)) {
		element.Evaluate(expression, arg, finish)
	}, done)
}

// EvaluateAll is the function expression called with every element it finds
// and arg.
func (l *Locator) EvaluateAll(expression string, arg any, done func(json.RawMessage, error)) {
	value, err := jsonValue(arg)
	if err != nil {
		failed(done, err)
		return
	}
	l.frame.run("Frame.evalOnSelectorAll", object{"selector": l.selector, "expression": expression,
		"isFunction": true, "arg": value}, rawValue(done))
}

// withElement runs use on the element, then lets its handle go.
func withElement[T any](l *Locator, use func(*ElementHandle, func(T, error)), done func(T, error)) {
	l.ElementHandle(func(element *ElementHandle, err error) {
		if err != nil {
			failed(done, err)
			return
		}
		use(element, func(value T, err error) {
			element.Dispose(nil)
			if done != nil {
				done(value, err)
			}
		})
	})
}

// ElementHandle is a handle to the element, once it is attached.
func (l *Locator) ElementHandle(done func(*ElementHandle, error)) {
	l.run("Frame.waitForSelector", object{"state": "attached"}, true, then(done, func(a answer) (*ElementHandle, error) {
		element := elementOf(l.frame, a["element"])
		if element == nil {
			return nil, fail("no element")
		}
		return element, nil
	}))
}

// ElementHandles is a handle to each element it finds.
func (l *Locator) ElementHandles(done func([]*ElementHandle, error)) {
	l.frame.QuerySelectorAll(l.selector, done)
}

// WaitFor waits for state: "attached", "detached", "visible" (""), "hidden".
func (l *Locator) WaitFor(state string, done func(error)) {
	params := object{"omitReturnValue": true}
	if state != "" {
		params["state"] = state
	}
	l.act("Frame.waitForSelector", params, done)
}

// AriaSnapshot is the element's accessibility tree, as YAML.
func (l *Locator) AriaSnapshot(done func(string, error)) {
	l.run("Frame.ariaSnapshot", nil, true, then(done, func(a answer) (string, error) { return a.str("snapshot"), nil }))
}

// BoundingBox is the element's box, nil when it is not shown.
func (l *Locator) BoundingBox(done func(*BoundingBox, error)) {
	withElement(l, func(element *ElementHandle, finish func(*BoundingBox, error)) { element.BoundingBox(finish) }, done)
}

// Screenshot is a picture of the element, with Page.Screenshot's options.
func (l *Locator) Screenshot(options any, done func([]byte, error)) {
	withElement(l, func(element *ElementHandle, finish func([]byte, error)) { element.Screenshot(options, finish) }, done)
}

// ScreenshotPixels is the element's pixels, unencoded, with
// Page.ScreenshotPixels's options.
func (l *Locator) ScreenshotPixels(options any, done func(*zebkit.ScreenshotPixels, error)) {
	withElement(l, func(element *ElementHandle, finish func(*zebkit.ScreenshotPixels, error)) {
		element.ScreenshotPixels(options, finish)
	}, done)
}

// ScrollIntoViewIfNeeded scrolls the element into view.
func (l *Locator) ScrollIntoViewIfNeeded(done func(error)) {
	withElement(l, func(element *ElementHandle, finish func(struct{}, error)) {
		element.ScrollIntoViewIfNeeded(func(err error) { finish(struct{}{}, err) })
	}, func(_ struct{}, err error) {
		if done != nil {
			done(err)
		}
	})
}

func stringList(value json.RawMessage) []string {
	var items []any
	_ = json.Unmarshal(value, &items)
	out := make([]string, 0, len(items))
	for _, item := range items {
		s, _ := item.(string)
		out = append(out, s)
	}
	return out
}

// FrameLocator is Playwright's FrameLocator: the frame of an <iframe>
// selector.
type FrameLocator struct {
	frame    *Frame
	selector string
}

// Locator finds selector in the frame.
func (f *FrameLocator) Locator(selector string) *Locator {
	return f.frame.Locator(f.selector + " >> internal:control=enter-frame >> " + selector)
}

// FrameLocator is the frame of the <iframe> of selector in the frame.
func (f *FrameLocator) FrameLocator(selector string) *FrameLocator {
	return &FrameLocator{frame: f.frame, selector: f.selector + " >> internal:control=enter-frame >> " + selector}
}

// Owner is the <iframe> itself.
func (f *FrameLocator) Owner() *Locator { return f.frame.Locator(f.selector) }

// GetByRole is Frame.GetByRole in the frame.
func (f *FrameLocator) GetByRole(role string, options RoleOptions) *Locator {
	return f.Locator(options.selector(role))
}

// GetByText is Frame.GetByText in the frame.
func (f *FrameLocator) GetByText(text any, exact bool) *Locator {
	return f.Locator(byText(text, exact))
}

// GetByLabel is Frame.GetByLabel in the frame.
func (f *FrameLocator) GetByLabel(text any, exact bool) *Locator {
	return f.Locator(byLabel(text, exact))
}

// GetByPlaceholder is Frame.GetByPlaceholder in the frame.
func (f *FrameLocator) GetByPlaceholder(text any, exact bool) *Locator {
	return f.Locator(byAttribute("placeholder", text, exact))
}

// GetByAltText is Frame.GetByAltText in the frame.
func (f *FrameLocator) GetByAltText(text any, exact bool) *Locator {
	return f.Locator(byAttribute("alt", text, exact))
}

// GetByTitle is Frame.GetByTitle in the frame.
func (f *FrameLocator) GetByTitle(text any, exact bool) *Locator {
	return f.Locator(byAttribute("title", text, exact))
}

// GetByTestID is Frame.GetByTestID in the frame.
func (f *FrameLocator) GetByTestID(id string) *Locator { return f.Locator(byTestID(id)) }
