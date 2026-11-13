package automation

import (
	"regexp"
	"testing"
)

func TestGlobsMatchAsPlaywrights(t *testing.T) {
	cases := []struct {
		glob, url string
		expected  bool
	}{
		{"**/data.json", "http://127.0.0.1:8000/data.json", true},
		{"**/data.json", "http://127.0.0.1:8000/a/b/data.json", true},
		{"**/data.json", "http://127.0.0.1:8000/data.jsonp", false},
		{"http://a.test/*.js", "http://a.test/x.js", true},
		{"http://a.test/*.js", "http://a.test/b/x.js", false},
		{"**/*.{png,jpg}", "https://x.test/i/a.jpg", true},
		{"**/*.{png,jpg}", "https://x.test/i/a.gif", false},
		{"**", "anything at all", true},
		{"https://x.test/a?b", "https://x.test/a?b", true},
		{"https://x.test/a?b", "https://x.test/aXb", false},
		{"https://x.test/\\*", "https://x.test/*", true},
		{"https://x.test/**/z", "https://x.test/z", true},
		{"https://x.test/**/z", "https://x.test/yz", false},
		{"https://x.test/**/z", "https://x.test/y/z", true},
	}
	for _, c := range cases {
		if got := newGlob(c.glob).matches(c.url); got != c.expected {
			t.Errorf("%s %s: %v", c.glob, c.url, got)
		}
	}
}

func TestBase64(t *testing.T) {
	for _, text := range []string{"Zm9vYmFy", "Zm8=", "Zm8", "AP+A"} {
		if _, err := decodeBase64(text); err != nil {
			t.Errorf("%s: %v", text, err)
		}
	}
	if b, _ := decodeBase64("Zm8="); string(b) != "fo" {
		t.Errorf("Zm8=: %q", b)
	}
	if _, err := decodeBase64("!!"); err == nil {
		t.Error("invalid base64 decoded")
	}
}

func TestFunctionsAreToldFromExpressions(t *testing.T) {
	for _, f := range []string{"(b) => b.tagName", "es => es.map(e => e)", "async () => 1", "function () { return 1 }"} {
		if !looksLikeFunction(f) {
			t.Errorf("%s is a function", f)
		}
	}
	for _, e := range []string{"document.title", "1 + 2", "fetch('a').then(r => r.json())"} {
		if looksLikeFunction(e) {
			t.Errorf("%s is not a function", e)
		}
	}
}

func TestSelectors(t *testing.T) {
	if got := (RoleOptions{Name: "Go", Checked: Bool(true), Level: 2}).selector("button"); got != `internal:role=button[checked=true][level=2][name="Go"i]` {
		t.Error(got)
	}
	if got := textSelector(regexp.MustCompile(`(?i)a/b`), false); got != `/a\/b/i` {
		t.Error(got)
	}
	if got := byText("I agree", true); got != `internal:text="I agree"s` {
		t.Error(got)
	}
}
