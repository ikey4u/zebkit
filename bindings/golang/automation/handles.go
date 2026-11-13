package automation

import (
	"encoding/json"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// BoundingBox is a box in CSS pixels.
type BoundingBox struct {
	X, Y, Width, Height float64
}

func handleOf(frame *Frame, raw json.RawMessage) *JSHandle {
	value := readAnswer(raw)
	id, ok := value.text("handle")
	if !ok {
		return nil
	}
	var element bool
	if e, ok := value["element"]; ok && string(e) != "null" && string(e) != "false" {
		element = true
	}
	return &JSHandle{frame: frame, id: id, preview: value.str("preview"), element: element}
}

func elementOf(frame *Frame, raw json.RawMessage) *ElementHandle {
	if handle := handleOf(frame, raw); handle != nil {
		return &ElementHandle{handle}
	}
	return nil
}

func elementsOf(frame *Frame, list []json.RawMessage) []*ElementHandle {
	out := []*ElementHandle{}
	for _, raw := range list {
		if element := elementOf(frame, raw); element != nil {
			out = append(out, element)
		}
	}
	return out
}

// JSHandle is a JavaScript object of the page, kept until Dispose. In an
// evaluation's arg it stands for its object.
type JSHandle struct {
	frame   *Frame
	id      string
	preview string
	element bool
}

// ID is the handle's id ("h3").
func (h *JSHandle) ID() string { return h.id }

// Preview is how the engine shows it ("JSHandle@object", "div#out").
func (h *JSHandle) Preview() string { return h.preview }

// String is the handle's preview.
func (h *JSHandle) String() string { return "JSHandle(" + h.id + " " + h.preview + ")" }

// MarshalJSON is the handle as an argument of an evaluation: {"handle": id}.
func (h *JSHandle) MarshalJSON() ([]byte, error) { return json.Marshal(object{"handle": h.id}) }

// AsElement is the handle as an element's, nil when it is not one.
func (h *JSHandle) AsElement() *ElementHandle {
	if !h.element {
		return nil
	}
	return &ElementHandle{h}
}

func (h *JSHandle) run(name string, params object, done func(answer, error)) {
	if params == nil {
		params = object{}
	}
	params["handle"] = h.id
	prefix := "JSHandle."
	if h.element {
		prefix = "ElementHandle."
	}
	run(h.frame.state.view, prefix+name, params, done)
}

// Evaluate is the function expression called with the handle's object and
// arg.
func (h *JSHandle) Evaluate(expression string, arg any, done func(json.RawMessage, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	h.run("evaluateExpression", params, rawValue(done))
}

// EvaluateHandle is Evaluate's value as a handle.
func (h *JSHandle) EvaluateHandle(expression string, arg any, done func(*JSHandle, error)) {
	params, err := evaluation(expression, arg)
	if err != nil {
		failed(done, err)
		return
	}
	h.run("evaluateExpressionHandle", params, then(done, func(a answer) (*JSHandle, error) {
		return handleOf(h.frame, a["handle"]), nil
	}))
}

// GetProperty is the object's property name, as a handle.
func (h *JSHandle) GetProperty(name string, done func(*JSHandle, error)) {
	h.run("getProperty", object{"name": name}, then(done, func(a answer) (*JSHandle, error) {
		return handleOf(h.frame, a["handle"]), nil
	}))
}

// Property is a property of an object, as GetProperties gives it.
type Property struct {
	Name  string
	Value *JSHandle
}

// GetProperties is the object's own properties, as handles.
func (h *JSHandle) GetProperties(done func([]Property, error)) {
	h.run("getPropertyList", nil, then(done, func(a answer) ([]Property, error) {
		out := []Property{}
		for _, raw := range a.list("properties") {
			property := readAnswer(raw)
			out = append(out, Property{property.str("name"), handleOf(h.frame, property["value"])})
		}
		return out, nil
	}))
}

// JSONValue is the object as JSON.
func (h *JSHandle) JSONValue(done func(json.RawMessage, error)) {
	h.run("jsonValue", nil, rawValue(done))
}

// Dispose lets the object go.
func (h *JSHandle) Dispose(done func(error)) { h.run("dispose", nil, unit(done)) }

// ElementHandle is an element of the page: a JSHandle with Playwright's
// ElementHandle API.
type ElementHandle struct {
	*JSHandle
}

func (e *ElementHandle) act(name string, params any, done func(error)) {
	o, err := options(params)
	if err != nil {
		failedUnit(done, err)
		return
	}
	e.run(name, o, unit(done))
}

// Command runs an ElementHandle command ("click", "fill", ...) with params,
// a JSON object (nil for none).
func (e *ElementHandle) Command(name string, params any, done func(json.RawMessage, error)) {
	command(e.frame.state.view, "ElementHandle."+name, params, object{"handle": e.id}, done)
}

// Click clicks the element; options as Playwright's (nil for none).
func (e *ElementHandle) Click(options any, done func(error)) { e.act("click", options, done) }

// Dblclick double-clicks the element.
func (e *ElementHandle) Dblclick(options any, done func(error)) { e.act("dblclick", options, done) }

// Hover moves the mouse over the element.
func (e *ElementHandle) Hover(options any, done func(error)) { e.act("hover", options, done) }

// Tap taps the element.
func (e *ElementHandle) Tap(options any, done func(error)) { e.act("tap", options, done) }

// Check checks a checkbox or radio button.
func (e *ElementHandle) Check(options any, done func(error)) { e.act("check", options, done) }

// Uncheck unchecks a checkbox.
func (e *ElementHandle) Uncheck(options any, done func(error)) { e.act("uncheck", options, done) }

// Fill sets a field's value.
func (e *ElementHandle) Fill(value string, done func(error)) {
	e.act("fill", object{"value": value}, done)
}

// Press presses key in the element.
func (e *ElementHandle) Press(key string, done func(error)) { e.act("press", object{"key": key}, done) }

// Type types text into the element, a key at a time.
func (e *ElementHandle) Type(text string, done func(error)) {
	e.act("type", object{"text": text}, done)
}

// Focus focuses the element.
func (e *ElementHandle) Focus(done func(error)) { e.act("focus", nil, done) }

// SelectText selects the element's text.
func (e *ElementHandle) SelectText(done func(error)) { e.act("selectText", nil, done) }

// ScrollIntoViewIfNeeded scrolls the element into view.
func (e *ElementHandle) ScrollIntoViewIfNeeded(done func(error)) {
	e.act("scrollIntoViewIfNeeded", nil, done)
}

// TextContent is the element's textContent (nil for none).
func (e *ElementHandle) TextContent(done func(*string, error)) {
	e.run("textContent", nil, stringValue(done))
}

// InnerText is the element's innerText.
func (e *ElementHandle) InnerText(done func(string, error)) { e.run("innerText", nil, textValue(done)) }

// InnerHTML is the element's innerHTML.
func (e *ElementHandle) InnerHTML(done func(string, error)) { e.run("innerHTML", nil, textValue(done)) }

// GetAttribute is the element's attribute name (nil for none).
func (e *ElementHandle) GetAttribute(name string, done func(*string, error)) {
	e.run("getAttribute", object{"name": name}, stringValue(done))
}

// InputValue is a field's value.
func (e *ElementHandle) InputValue(done func(string, error)) {
	e.run("inputValue", nil, textValue(done))
}

// IsVisible says whether the element is visible.
func (e *ElementHandle) IsVisible(done func(bool, error)) { e.run("isVisible", nil, boolValue(done)) }

// IsHidden says whether the element is hidden.
func (e *ElementHandle) IsHidden(done func(bool, error)) { e.run("isHidden", nil, boolValue(done)) }

// IsEnabled says whether the element is enabled.
func (e *ElementHandle) IsEnabled(done func(bool, error)) { e.run("isEnabled", nil, boolValue(done)) }

// IsChecked says whether the element is checked.
func (e *ElementHandle) IsChecked(done func(bool, error)) { e.run("isChecked", nil, boolValue(done)) }

// BoundingBox is the element's box, nil when it is not shown.
func (e *ElementHandle) BoundingBox(done func(*BoundingBox, error)) {
	e.run("boundingBox", nil, then(done, func(a answer) (*BoundingBox, error) {
		value := a.object("value")
		width, ok := value.number("width")
		if !ok {
			return nil, nil
		}
		x, _ := value.number("x")
		y, _ := value.number("y")
		height, _ := value.number("height")
		return &BoundingBox{x, y, width, height}, nil
	}))
}

// Screenshot is a picture of the element, with Page.Screenshot's options.
func (e *ElementHandle) Screenshot(options any, done func([]byte, error)) {
	params, path, err := takePath(options)
	if err != nil {
		failed(done, err)
		return
	}
	e.run("screenshot", params, then(done, func(a answer) ([]byte, error) {
		data, err := binary(a, "binary")
		if err != nil {
			return nil, err
		}
		return writeFile(path, data)
	}))
}

// ScreenshotPixels is the element's pixels, unencoded.
func (e *ElementHandle) ScreenshotPixels(options any, done func(*zebkit.ScreenshotPixels, error)) {
	params, err := with(options, object{"handle": e.id})
	if err != nil {
		failed(done, err)
		return
	}
	zebkit.CallAutomationPixels(e.frame.state.view, "ElementHandle.screenshot", params, done)
}

// QuerySelector is the first element of selector inside this one, nil for
// none.
func (e *ElementHandle) QuerySelector(selector string, done func(*ElementHandle, error)) {
	e.run("querySelector", object{"selector": selector}, then(done, func(a answer) (*ElementHandle, error) {
		return elementOf(e.frame, a["element"]), nil
	}))
}

// QuerySelectorAll is every element of selector inside this one.
func (e *ElementHandle) QuerySelectorAll(selector string, done func([]*ElementHandle, error)) {
	e.run("querySelectorAll", object{"selector": selector}, then(done, func(a answer) ([]*ElementHandle, error) {
		return elementsOf(e.frame, a.list("elements")), nil
	}))
}

// WaitForElementState waits for state: "visible", "hidden", "stable",
// "enabled", "disabled", "editable"; timeout in milliseconds, 0 for the
// default.
func (e *ElementHandle) WaitForElementState(state string, timeout float64, done func(error)) {
	e.act("waitForElementState", object{"state": state, "timeout": milliseconds(timeout)}, done)
}

// ContentFrame is the frame of an <iframe> element, nil for another.
func (e *ElementHandle) ContentFrame(done func(*Frame, error)) {
	e.run("contentFrame", nil, then(done, func(a answer) (*Frame, error) {
		id, ok := a.text("frame")
		if !ok {
			return nil, nil
		}
		return &Frame{state: e.frame.state, id: id}, nil
	}))
}

// SetInputFiles gives an <input type=file> the files at paths.
func (e *ElementHandle) SetInputFiles(paths []string, done func(error)) {
	e.act("setInputFiles", object{"localPaths": absolutes(paths)}, done)
}
