// Package automation is Playwright's API for a view's page, natively
// (<zebkit/automation.h>, docs/AUTOMATION.md): each command is one of
// Playwright's protocol, run inside the engine without CDP or a driver
// process. The names are those of Playwright's API (and of zebkit.automation
// in Python and zebkit::automation in Rust), in Go's case.
//
//	page := automation.NewPage(view)
//	page.Goto("https://example.com/", func(_ *automation.Response, err error) {
//		link := page.GetByRole("link", automation.RoleOptions{Name: "More information"})
//		link.Click(func(err error) {
//			automation.Expect(page.Locator("h1")).ToHaveText("Example Domains", func(err error) {
//				fmt.Println(err)
//				zebkit.Quit()
//			})
//		})
//	})
//
// It follows the zebkit package's rules. Every method is the engine thread's
// (it panics elsewhere), and so are the functions it calls: each command
// calls its done once, with the result or an error (an *zebkit.Error; a
// command whose timeout passed is one IsTimeout tells), and done may be nil.
// A command that answers nothing calls done(error).
//
// Values: what a page gives (an evaluation's result, a protocol event's
// parameters) is json.RawMessage. A value given to the page (an evaluation's
// arg, a BindingCall's result, an ExposeFunction's) is encoded with
// encoding/json, so a Go string is a JavaScript string; a json.RawMessage is
// JSON text as it is, and nil is null. A *JSHandle or *ElementHandle stands
// for its object in an evaluation's arg. Options, JSON objects, follow the
// zebkit package's one JSON rule (a string, []byte or json.RawMessage is JSON
// text as it is, any other value encoded, nil none); they are the protocol's,
// as Playwright names them ({"timeout": 300, "force": true}).
//
// Text to match (GetByText, Filter, the expect assertions) is a string, a
// substring ignoring case unless exact, or a *regexp.Regexp, sent to the page
// as JavaScript's (a leading (?i) is its "i" flag).
//
// The low level stays the zebkit package's: zebkit.CallAutomation and
// View.OnAutomationEvent run any command of Commands and see every event,
// and zebkit.ListenAutomation serves Playwright's own protocol (Listen).
package automation

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// Commands lists the engine's commands, by their protocol names.
func Commands() ([]string, error) { return zebkit.AutomationCommands() }

// Listen serves Playwright's own protocol on 127.0.0.1:port (0 picks one),
// what chromium.connect(url) of each Playwright client speaks; done gets the
// url, which is the credential to connect with (zebkit.ListenAutomation).
func Listen(port int, done func(url string, err error)) { zebkit.ListenAutomation(port, done) }

// IsTimeout says whether err is a command's whose timeout passed
// (Playwright's TimeoutError).
func IsTimeout(err error) bool {
	var e *zebkit.Error
	return errors.As(err, &e) && strings.HasPrefix(e.Message, "Timeout ")
}

// Bool is a *bool, for the options that may be left unset.
func Bool(value bool) *bool { return &value }

// Header is a request's or a response's header.
type Header struct {
	Name  string `json:"name"`
	Value string `json:"value"`
}

// ---- The package's own --------------------------------------------------------

func fail(message string) error { return &zebkit.Error{Message: message} }

// mustEngineThread panics off the engine's thread, as the zebkit package does.
func mustEngineThread() {
	if !zebkit.IsEngineThread() {
		panic("zebkit: called off the engine's thread; hand the work over with zebkit.Post")
	}
}

// object is a command's params: the protocol's names, values encoded with
// encoding/json (json.RawMessage as it is); nil values are left out.
type object map[string]any

// options is a JSON object given by the application (nil for none), as an
// object.
func options(value any) (object, error) {
	if value == nil {
		return object{}, nil
	}
	if o, ok := value.(object); ok {
		copied := object{}
		for k, v := range o {
			copied[k] = v
		}
		return copied, nil
	}
	var text []byte
	switch v := value.(type) {
	case string:
		text = []byte(v)
	case []byte:
		text = v
	case json.RawMessage:
		text = v
	default:
		var err error
		if text, err = json.Marshal(value); err != nil {
			return nil, err
		}
	}
	if len(text) == 0 || string(text) == "null" {
		return object{}, nil
	}
	var members map[string]json.RawMessage
	if err := json.Unmarshal(text, &members); err != nil {
		return nil, fmt.Errorf("zebkit: the options are not a JSON object: %w", err)
	}
	out := object{}
	for k, v := range members {
		out[k] = v
	}
	return out, nil
}

// with is options with members added (theirs win).
func with(value any, members object) (object, error) {
	out, err := options(value)
	if err != nil {
		return nil, err
	}
	for k, v := range members {
		out[k] = v
	}
	return out, nil
}

// jsonValue is value for the page: encoded with encoding/json, a
// json.RawMessage as it is; nil is null.
func jsonValue(value any) (json.RawMessage, error) {
	switch v := value.(type) {
	case nil:
		return json.RawMessage("null"), nil
	case json.RawMessage:
		if len(v) == 0 {
			return json.RawMessage("null"), nil
		}
		return v, nil
	}
	return json.Marshal(value)
}

// jsonText is value as JSON by the zebkit package's one rule: a string,
// []byte or json.RawMessage as it is, anything else encoded; nil is null.
func jsonText(value any) (json.RawMessage, error) {
	switch v := value.(type) {
	case string:
		if v == "" {
			return json.RawMessage("null"), nil
		}
		return json.RawMessage(v), nil
	case []byte:
		if len(v) == 0 {
			return json.RawMessage("null"), nil
		}
		return json.RawMessage(v), nil
	}
	return jsonValue(value)
}

func (o object) clean() object {
	for k, v := range o {
		if isNil(v) {
			delete(o, k)
		}
	}
	return o
}

func isNil(v any) bool {
	switch v := v.(type) {
	case nil:
		return true
	case *string:
		return v == nil
	case *bool:
		return v == nil
	case *int:
		return v == nil
	case *float64:
		return v == nil
	case json.RawMessage:
		return v == nil
	}
	return false
}

// answer is a command's result, read.
type answer map[string]json.RawMessage

func (a answer) text(key string) (string, bool) {
	var s string
	if raw, ok := a[key]; ok && string(raw) != "null" && json.Unmarshal(raw, &s) == nil {
		return s, true
	}
	return "", false
}

func (a answer) str(key string) string {
	s, _ := a.text(key)
	return s
}

func (a answer) optional(key string) *string {
	if s, ok := a.text(key); ok {
		return &s
	}
	return nil
}

func (a answer) boolean(key string) bool {
	var b bool
	_ = json.Unmarshal(a[key], &b)
	return b
}

func (a answer) number(key string) (float64, bool) {
	var n float64
	raw, ok := a[key]
	return n, ok && json.Unmarshal(raw, &n) == nil
}

func (a answer) object(key string) answer {
	var o answer
	_ = json.Unmarshal(a[key], &o)
	return o
}

func (a answer) list(key string) []json.RawMessage {
	var l []json.RawMessage
	_ = json.Unmarshal(a[key], &l)
	return l
}

func (a answer) value() json.RawMessage {
	if raw, ok := a["value"]; ok {
		return raw
	}
	return json.RawMessage("null")
}

func readAnswer(raw json.RawMessage) answer {
	var a answer
	_ = json.Unmarshal(raw, &a)
	if a == nil {
		a = answer{}
	}
	return a
}

// run runs method with params on view's page (nil: the browser's).
func run(view *zebkit.View, method string, params object, done func(answer, error)) {
	if params == nil {
		params = object{}
	}
	zebkit.CallAutomation(view, method, params.clean(), func(result json.RawMessage, err error) {
		if done == nil {
			return
		}
		if err != nil {
			done(nil, err)
			return
		}
		done(readAnswer(result), nil)
	})
}

// then is done for a command whose answer read reads.
func then[T any](done func(T, error), read func(answer) (T, error)) func(answer, error) {
	return func(a answer, err error) {
		if done == nil {
			return
		}
		var value T
		if err == nil {
			value, err = read(a)
		}
		done(value, err)
	}
}

// unit is done for a command that answers nothing.
func unit(done func(error)) func(answer, error) {
	return func(_ answer, err error) {
		if done != nil {
			done(err)
		}
	}
}

func rawValue(done func(json.RawMessage, error)) func(answer, error) {
	return then(done, func(a answer) (json.RawMessage, error) { return a.value(), nil })
}

func stringValue(done func(*string, error)) func(answer, error) {
	return then(done, func(a answer) (*string, error) { return a.optional("value"), nil })
}

func textValue(done func(string, error)) func(answer, error) {
	return then(done, func(a answer) (string, error) { return a.str("value"), nil })
}

func boolValue(done func(bool, error)) func(answer, error) {
	return then(done, func(a answer) (bool, error) { return a.boolean("value"), nil })
}

// failed calls done with err for a command that could not be sent.
func failed[T any](done func(T, error), err error) {
	if done != nil {
		var zero T
		done(zero, err)
	}
}

func failedUnit(done func(error), err error) {
	if done != nil {
		done(err)
	}
}

func headerList(headers []Header) []Header {
	if headers == nil {
		return []Header{}
	}
	return headers
}

// headersOf is the headers of a request or a response, their names in lower case.
func headersOf(a answer) []Header {
	var list []Header
	_ = json.Unmarshal(a["headers"], &list)
	for i := range list {
		list[i].Name = strings.ToLower(list[i].Name)
	}
	return list
}

func binary(a answer, key string) ([]byte, error) {
	text, ok := a.text(key)
	if !ok {
		return nil, fail("no " + key + " in the answer")
	}
	return decodeBase64(text)
}

func decodeBase64(text string) ([]byte, error) {
	text = strings.Map(func(r rune) rune {
		switch r {
		case '\n', '\r', ' ', '=':
			return -1
		case '-':
			return '+'
		case '_':
			return '/'
		}
		return r
	}, text)
	out, err := base64.RawStdEncoding.DecodeString(text)
	if err != nil {
		return nil, fail("invalid base64")
	}
	return out, nil
}

func writeFile(path string, data []byte) ([]byte, error) {
	if path != "" {
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return nil, err
		}
	}
	return data, nil
}

// absolute is path made absolute, as the engine takes files.
func absolute(path string) string {
	if abs, err := filepath.Abs(path); err == nil {
		return abs
	}
	return path
}

func absolutes(paths []string) []string {
	out := make([]string, 0, len(paths))
	for _, path := range paths {
		out = append(out, absolute(path))
	}
	return out
}

// takePath is options without its "path", and the path.
func takePath(value any) (object, string, error) {
	o, err := options(value)
	if err != nil {
		return nil, "", err
	}
	var path string
	if raw, ok := o["path"]; ok {
		switch p := raw.(type) {
		case string:
			path = p
		case json.RawMessage:
			_ = json.Unmarshal(p, &path)
		}
		delete(o, "path")
	}
	return o, path, nil
}

// quote is text as a JSON string, as Playwright's selectors are written.
func quote(text string) string {
	var out strings.Builder
	encoder := json.NewEncoder(&out)
	encoder.SetEscapeHTML(false)
	_ = encoder.Encode(text)
	return strings.TrimSuffix(out.String(), "\n")
}

// looksLikeFunction says whether expression reads as a function rather than
// an expression: function, (a, b) =>, a =>, each maybe async (as Python's
// binding tells them).
func looksLikeFunction(expression string) bool {
	text := strings.TrimSpace(expression)
	if rest, ok := strings.CutPrefix(text, "async"); ok && rest != "" && isSpace(rest[0]) {
		text = strings.TrimLeft(rest, " \t\r\n")
	}
	if rest, ok := strings.CutPrefix(text, "function"); ok && (rest == "" || !isWord(rest[0])) {
		return true
	}
	arrow := func(rest string) bool { return strings.HasPrefix(strings.TrimLeft(rest, " \t\r\n"), "=>") }
	// \(?[\w$\s,]*\)?\s*=>
	rest := strings.TrimPrefix(text, "(")
	rest = strings.TrimLeftFunc(rest, func(r rune) bool {
		return r < 128 && (isWord(byte(r)) || r == '$' || isSpace(byte(r)) || r == ',')
	})
	if arrow(strings.TrimPrefix(rest, ")")) {
		return true
	}
	// \([^)]*\)\s*=>
	if rest, ok := strings.CutPrefix(text, "("); ok {
		if at := strings.IndexByte(rest, ')'); at >= 0 {
			return arrow(rest[at+1:])
		}
	}
	return false
}

func isSpace(c byte) bool { return c == ' ' || c == '\t' || c == '\n' || c == '\r' }

func isWord(c byte) bool {
	return c == '_' || c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= 128
}

// evaluation is the params of an evaluation of expression with arg.
func evaluation(expression string, arg any) (object, error) {
	value, err := jsonValue(arg)
	if err != nil {
		return nil, err
	}
	params := object{"expression": expression, "arg": value}
	if looksLikeFunction(expression) {
		params["isFunction"] = true
	}
	return params, nil
}

// ---- Text, as Playwright's locatorUtils.ts makes selectors of it ------------

// textOf is a string or a *regexp.Regexp as JavaScript's regular
// expression: its source and whether it ignores case.
func regexOf(re *regexp.Regexp) (source string, ignoreCase bool) {
	source = re.String()
	if rest, ok := strings.CutPrefix(source, "(?i)"); ok {
		return rest, true
	}
	return source, false
}

func textString(text any) string {
	switch t := text.(type) {
	case string:
		return t
	case fmt.Stringer:
		return t.String()
	}
	return fmt.Sprint(text)
}

func textSelector(text any, exact bool) string {
	if re, ok := text.(*regexp.Regexp); ok {
		source, ignoreCase := regexOf(re)
		flags := ""
		if ignoreCase {
			flags = "i"
		}
		return "/" + strings.ReplaceAll(source, "/", "\\/") + "/" + flags
	}
	if exact {
		return quote(textString(text)) + "s"
	}
	return quote(textString(text)) + "i"
}

func attributeSelector(text any, exact bool) string {
	if _, ok := text.(*regexp.Regexp); ok {
		return textSelector(text, exact)
	}
	value := textString(text)
	escaped := "\"" + strings.ReplaceAll(strings.ReplaceAll(value, "\\", "\\\\"), "\"", "\\\"") + "\""
	if exact {
		return escaped + "s"
	}
	return escaped + "i"
}

// expectedText is text as Frame.expect's expectedText has it.
func expectedText(text any, substring, normalize *bool) object {
	entry := object{}
	if re, ok := text.(*regexp.Regexp); ok {
		source, ignoreCase := regexOf(re)
		entry["regexSource"] = source
		if ignoreCase {
			entry["regexFlags"] = "i"
		} else {
			entry["regexFlags"] = ""
		}
	} else {
		entry["string"] = textString(text)
	}
	if substring != nil {
		entry["matchSubstring"] = *substring
	}
	if normalize != nil {
		entry["normalizeWhiteSpace"] = *normalize
	}
	return entry
}

// RoleOptions are GetByRole's, as Playwright's: Name is text to match (a
// string or a *regexp.Regexp; nil for any), exactly when Exact; Level 0 is
// any.
type RoleOptions struct {
	Name          any
	Exact         bool
	Checked       *bool
	Disabled      *bool
	Selected      *bool
	Expanded      *bool
	IncludeHidden *bool
	Level         int
	Pressed       *bool
}

func (o RoleOptions) selector(role string) string {
	var out strings.Builder
	out.WriteString("internal:role=" + role)
	for _, flag := range []struct {
		key   string
		value *bool
	}{{"checked", o.Checked}, {"disabled", o.Disabled}, {"selected", o.Selected}, {"expanded", o.Expanded}, {"include-hidden", o.IncludeHidden}} {
		if flag.value != nil {
			fmt.Fprintf(&out, "[%s=%t]", flag.key, *flag.value)
		}
	}
	if o.Level > 0 {
		fmt.Fprintf(&out, "[level=%d]", o.Level)
	}
	if o.Name != nil {
		fmt.Fprintf(&out, "[name=%s]", attributeSelector(o.Name, o.Exact))
	}
	if o.Pressed != nil {
		fmt.Fprintf(&out, "[pressed=%t]", *o.Pressed)
	}
	return out.String()
}

// Filter is Locator.Filter's: text it has or has not (a string or a
// *regexp.Regexp; nil for none), locators it has or has not inside, and
// whether it is visible.
type Filter struct {
	HasText    any
	HasNotText any
	Has        *Locator
	HasNot     *Locator
	Visible    *bool
}

// ---- Times ------------------------------------------------------------------------

// clockParams is a Clock time (milliseconds since the epoch as a number, a
// date as JavaScript reads one as a string, or a time.Time).
func clockParams(value any, ticks bool) (object, error) {
	head := "time"
	if ticks {
		head = "ticks"
	}
	switch v := value.(type) {
	case string:
		return object{head + "String": v}, nil
	case interface{ UnixMilli() int64 }:
		return object{head + "Number": float64(v.UnixMilli())}, nil
	case int:
		return object{head + "Number": float64(v)}, nil
	case int64:
		return object{head + "Number": float64(v)}, nil
	case float64:
		return object{head + "Number": v}, nil
	case interface{ Milliseconds() int64 }:
		return object{head + "Number": float64(v.Milliseconds())}, nil
	}
	return nil, fail(fmt.Sprintf("zebkit: a clock time is a number of milliseconds, a string or a time.Time, not %T", value))
}

// milliseconds is a timeout for the protocol: nil for 0 (the default).
func milliseconds(timeout float64) any {
	if timeout == 0 || math.IsNaN(timeout) {
		return nil
	}
	return timeout
}
