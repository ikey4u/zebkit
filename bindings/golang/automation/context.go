package automation

import (
	"encoding/json"
	"fmt"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// BrowserContext is a context of its own (NewContext): its own profile,
// cookies, storage, permissions and cache, and its pages.
type BrowserContext struct {
	id    string
	pages []*Page
}

// NewContext is browser.newContext(): options are Playwright's as the
// protocol names them, JSON ({"viewport": {...}, "userAgent": ...}; nil for
// none).
func NewContext(opts any, done func(*BrowserContext, error)) {
	params, err := options(opts)
	if err != nil {
		failed(done, err)
		return
	}
	run(nil, "Browser.newContext", params, then(done, func(a answer) (*BrowserContext, error) {
		id, ok := a.text("context")
		if !ok {
			return nil, fail("no context")
		}
		return &BrowserContext{id: id}, nil
	}))
}

// ID is the context's id ("x1").
func (c *BrowserContext) ID() string { return c.id }

// Pages is the pages NewPage made, open or not.
func (c *BrowserContext) Pages() []*Page {
	mustEngineThread()
	return append([]*Page(nil), c.pages...)
}

// NewPage is a new page of the context: an off-screen view under --headless.
func (c *BrowserContext) NewPage(done func(*Page, error)) {
	c.run("BrowserContext.newPage", nil, then(done, func(a answer) (*Page, error) {
		id, ok := a.text("page")
		if !ok {
			return nil, fail("no page")
		}
		view, err := zebkit.ViewByPageID(id)
		if err != nil {
			return nil, fail("the page is closed")
		}
		page := NewPage(view)
		c.pages = append(c.pages, page)
		return page, nil
	}))
}

func (c *BrowserContext) run(method string, params object, done func(answer, error)) {
	if params == nil {
		params = object{}
	}
	params["context"] = c.id
	run(nil, method, params, done)
}

// Command runs one of the context's commands ("BrowserContext.cookies",
// ...) with params, a JSON object (nil for none), and the context's id.
func (c *BrowserContext) Command(method string, params any, done func(json.RawMessage, error)) {
	command(nil, method, params, object{"context": c.id}, done)
}

// Cookies is the context's cookies, of urls (none: all).
func (c *BrowserContext) Cookies(urls []string, done func([]Cookie, error)) {
	c.run("BrowserContext.cookies", cookieURLs(urls), then(done, cookieList))
}

// AddCookies adds cookies to the context.
func (c *BrowserContext) AddCookies(cookies []Cookie, done func(error)) {
	c.run("BrowserContext.addCookies", object{"cookies": cookieInput(cookies)}, unit(done))
}

// ClearCookies removes the context's cookies, of name, domain and path ("":
// any).
func (c *BrowserContext) ClearCookies(name, domain, path string, done func(error)) {
	c.run("BrowserContext.clearCookies", clearParams(name, domain, path), unit(done))
}

// GrantPermissions grants permissions to origin ("": every origin).
func (c *BrowserContext) GrantPermissions(permissions []string, origin string, done func(error)) {
	c.run("BrowserContext.grantPermissions", permissionParams(permissions, origin), unit(done))
}

// ClearPermissions takes back the permissions granted.
func (c *BrowserContext) ClearPermissions(done func(error)) {
	c.run("BrowserContext.clearPermissions", nil, unit(done))
}

// SetGeolocation is {"latitude", "longitude", "accuracy"}, JSON; nil for
// none.
func (c *BrowserContext) SetGeolocation(geolocation any, done func(error)) {
	contextCommand(nil, &c.id, "BrowserContext.setGeolocation", "geolocation", geolocation, done)
}

// SetHTTPCredentials is {"username", "password", "origin"}, JSON; nil for
// none.
func (c *BrowserContext) SetHTTPCredentials(credentials any, done func(error)) {
	contextCommand(nil, &c.id, "BrowserContext.setHTTPCredentials", "httpCredentials", credentials, done)
}

// SetOffline takes the context off the network, or back.
func (c *BrowserContext) SetOffline(offline bool, done func(error)) {
	c.run("BrowserContext.setOffline", object{"offline": offline}, unit(done))
}

// SetExtraHTTPHeaders adds headers to every request of the context.
func (c *BrowserContext) SetExtraHTTPHeaders(headers []Header, done func(error)) {
	c.run("BrowserContext.setExtraHTTPHeaders", object{"headers": headerList(headers)}, unit(done))
}

// StorageState is the context's cookies and storage, as Playwright's
// storageState.
func (c *BrowserContext) StorageState(done func(json.RawMessage, error)) {
	c.Command("BrowserContext.storageState", nil, done)
}

// Close closes the context and its pages.
func (c *BrowserContext) Close(done func(error)) {
	c.run("BrowserContext.close", nil, func(_ answer, err error) {
		if err == nil {
			c.pages = nil
		}
		if done != nil {
			done(err)
		}
	})
}

// ---- expect ------------------------------------------------------------------------

// Assertion is Playwright's assertions, retried in the page until they hold
// or their timeout (5 s unless Timeout) passes; a failed one is an error
// saying what was received.
type Assertion struct {
	locator *Locator
	frame   *Frame
	not     bool
	timeout float64
	message string
}

// Expect is expect(locator).
func Expect(locator *Locator) *Assertion { return &Assertion{locator: locator, timeout: 5000} }

// ExpectPage is expect(page): its title and URL.
func ExpectPage(page *Page) *Assertion { return &Assertion{frame: page.MainFrame(), timeout: 5000} }

// Not is the opposite assertion.
func (e *Assertion) Not() *Assertion {
	copied := *e
	copied.not = !e.not
	return &copied
}

// Timeout is the assertion with timeout, in milliseconds.
func (e *Assertion) Timeout(milliseconds float64) *Assertion {
	copied := *e
	copied.timeout = milliseconds
	return &copied
}

// Message is the assertion saying message when it fails.
func (e *Assertion) Message(message string) *Assertion {
	copied := *e
	copied.message = message
	return &copied
}

// Check runs Frame.expect with expression ("to.have.text", "to.be.visible",
// ...) and its options, a JSON object (the protocol's: expectedText,
// expectedNumber, ...).
func (e *Assertion) Check(expression string, opts any, done func(error)) {
	params, err := with(opts, object{"expression": expression, "isNot": e.not, "timeout": e.timeout})
	if err != nil {
		failedUnit(done, err)
		return
	}
	shown, frame := "page", e.frame
	if e.locator != nil {
		shown, frame = e.locator.selector, e.locator.frame
		params["selector"] = e.locator.selector
	}
	not, message := e.not, e.message
	frame.run("Frame.expect", params, func(a answer, err error) {
		if err == nil {
			var matches bool
			if json.Unmarshal(a["matches"], &matches) == nil && matches == not {
				err = e.failure(a, shown, expression, not, message)
			}
		}
		if done != nil {
			done(err)
		}
	})
}

func (e *Assertion) failure(a answer, shown, expression string, not bool, message string) error {
	if message != "" {
		return fail(message)
	}
	prefix, timedOut := "", ""
	if not {
		prefix = "not."
	}
	if a.boolean("timedOut") {
		timedOut = " (timed out)"
	}
	received := string(a["received"])
	if received == "" {
		received = "null"
	}
	return fail(fmt.Sprintf("expect(%s).%s%s failed%s: received %s %s", shown, prefix, expression, timedOut, received, a.str("errorMessage")))
}

func (e *Assertion) texts(expression string, expected []any, substring, normalize *bool, more object, done func(error)) {
	list := []object{}
	for _, text := range expected {
		list = append(list, expectedText(text, substring, normalize))
	}
	params := object{"expectedText": list}
	for k, v := range more {
		params[k] = v
	}
	e.Check(expression, params, done)
}

// ToHaveText: the element's text is expected (a string, whitespace
// normalized, or a *regexp.Regexp).
func (e *Assertion) ToHaveText(expected any, done func(error)) {
	e.texts("to.have.text", []any{expected}, Bool(false), Bool(true), nil, done)
}

// ToHaveTexts: the texts of each element it finds, in order.
func (e *Assertion) ToHaveTexts(expected []any, done func(error)) {
	e.texts("to.have.text.array", expected, Bool(false), Bool(true), nil, done)
}

// ToContainText: the element's text has expected.
func (e *Assertion) ToContainText(expected any, done func(error)) {
	e.texts("to.have.text", []any{expected}, Bool(true), Bool(true), nil, done)
}

// ToHaveValue: a field's value is value.
func (e *Assertion) ToHaveValue(value any, done func(error)) {
	e.texts("to.have.value", []any{value}, nil, nil, nil, done)
}

// ToHaveAttribute: the element's attribute name is value.
func (e *Assertion) ToHaveAttribute(name string, value any, done func(error)) {
	e.texts("to.have.attribute.value", []any{value}, nil, nil, object{"expressionArg": name}, done)
}

// ToHaveClass: the element's class is expected.
func (e *Assertion) ToHaveClass(expected any, done func(error)) {
	e.texts("to.have.class", []any{expected}, nil, nil, nil, done)
}

// ToHaveCount: the locator finds count elements.
func (e *Assertion) ToHaveCount(count int, done func(error)) {
	e.Check("to.have.count", object{"expectedNumber": count}, done)
}

// ToHaveTitle: the page's title is title.
func (e *Assertion) ToHaveTitle(title any, done func(error)) {
	e.texts("to.have.title", []any{title}, nil, Bool(true), nil, done)
}

// ToHaveURL: the page's URL is url.
func (e *Assertion) ToHaveURL(url any, done func(error)) {
	e.texts("to.have.url", []any{url}, nil, nil, nil, done)
}

// ToMatchAriaSnapshot: the element's accessibility tree matches expected,
// YAML.
func (e *Assertion) ToMatchAriaSnapshot(expected string, done func(error)) {
	e.Check("to.match.aria", object{"expectedValue": expected}, done)
}

// ToBeVisible: the element is visible.
func (e *Assertion) ToBeVisible(done func(error)) { e.Check("to.be.visible", nil, done) }

// ToBeHidden: the element is hidden.
func (e *Assertion) ToBeHidden(done func(error)) { e.Check("to.be.hidden", nil, done) }

// ToBeEnabled: the element is enabled.
func (e *Assertion) ToBeEnabled(done func(error)) { e.Check("to.be.enabled", nil, done) }

// ToBeDisabled: the element is disabled.
func (e *Assertion) ToBeDisabled(done func(error)) { e.Check("to.be.disabled", nil, done) }

// ToBeChecked: the element is checked.
func (e *Assertion) ToBeChecked(done func(error)) { e.Check("to.be.checked", nil, done) }

// ToBeEditable: the element is editable.
func (e *Assertion) ToBeEditable(done func(error)) { e.Check("to.be.editable", nil, done) }

// ToBeFocused: the element has the focus.
func (e *Assertion) ToBeFocused(done func(error)) { e.Check("to.be.focused", nil, done) }

// ToBeEmpty: the element has no content.
func (e *Assertion) ToBeEmpty(done func(error)) { e.Check("to.be.empty", nil, done) }

// ToBeAttached: the element is in the document.
func (e *Assertion) ToBeAttached(done func(error)) { e.Check("to.be.attached", nil, done) }

// ToBeInViewport: the element is in the viewport.
func (e *Assertion) ToBeInViewport(done func(error)) { e.Check("to.be.in.viewport", nil, done) }
