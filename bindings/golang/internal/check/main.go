// The Go binding's checks against a real engine: pages, calls, schemes, the
// page hooks and the system's services. Exit 0 when every check passes.
//
//	go run ./internal/check --no-sandbox --headless
package main

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"strings"
	"time"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

var checks, failures int

func check(ok bool, what string, detail ...any) {
	checks++
	if ok {
		fmt.Println("ok   " + what)
		return
	}
	failures++
	fmt.Println("FAIL "+what, detail)
}

func wait(condition func() bool, seconds float64) bool {
	end := time.Now().Add(time.Duration(seconds * float64(time.Second)))
	for time.Now().Before(end) {
		zebkit.Pump()
		if condition() {
			return true
		}
		time.Sleep(5 * time.Millisecond)
	}
	return false
}

// evaluate runs script and waits for its JSON result.
func evaluate(view *zebkit.View, script string) string {
	var result string
	done := false
	view.Evaluate(script, func(value string, err error) {
		if err != nil {
			value = "error: " + err.Error()
		}
		result, done = value, true
	})
	wait(func() bool { return done }, 15)
	return result
}

// activated runs script once the evaluation's user activation has reached
// the browser, as the device choosers and capture need.
func activated(view *zebkit.View, script string) string {
	return evaluate(view, "new Promise(r => setTimeout(r, 200)).then(() => "+script+")")
}

func main() {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	go http.Serve(listener, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, "<title>Hello</title><button>Go</button>")
	}))
	url := fmt.Sprintf("http://%s/", listener.Addr())

	zebkit.SchemeRegister("goapp", zebkit.SchemeStandard|zebkit.SchemeSecure|zebkit.SchemeFetch)
	if err := zebkit.Init(os.Args[1:]...); err != nil {
		fmt.Println("FAIL zebkit.Init:", err)
		os.Exit(1)
	}
	check(zebkit.Version() != "" && strings.Contains(zebkit.EngineVersion(), "Chromium"), "versions", zebkit.Version(), zebkit.EngineVersion())

	zebkit.SchemeHandle("goapp", func(request *zebkit.Request) {
		request.Respond(200, "Content-Type: text/html", []byte("<title>From Go</title>"))
	})

	// Grants and functions before the page: its zebkit object is made with it.
	view := zebkit.NewView("", 400, 300)
	view.Grant(strings.TrimSuffix(url, "/"), zebkit.AllowApp, "")
	view.Bind("twice", func(call *zebkit.Call) {
		var args []float64
		_ = json.Unmarshal([]byte(call.Args), &args)
		call.Resolve(fmt.Sprint(args[0] * 2))
	})
	loaded := false
	view.OnLoad(func(event zebkit.LoadEvent, at string) {
		if event == zebkit.LoadFinished && at == url {
			loaded = true
		}
	})
	view.Load(url)
	check(wait(func() bool { return loaded }, 15), "a page loads, told by OnLoad")
	check(evaluate(view, "document.title") == `"Hello"`, "Evaluate")
	check(strings.HasPrefix(evaluate(view, "nope()"), "error:"), "Evaluate reports the page's error")

	got := evaluate(view, "zebkit.app.twice(21)")
	check(got == "42", "a page calls a Go function", got)

	var png []byte
	view.Capture(func(bytes []byte, err error) { png = bytes })
	check(wait(func() bool { return png != nil }, 15) && string(png[1:4]) == "PNG", "Capture")

	check(len(zebkit.Displays()) >= 0 && zebkit.Environment() != nil, "Displays and Environment")
	check(zebkit.AppSetBadge("2") && zebkit.AppSetProgress(0.5) && !zebkit.AppSetProgress(3), "badge and progress")
	token, err := zebkit.PreventSleep("check", false)
	check(err == nil && zebkit.AllowSleep(token) && !zebkit.AllowSleep(token), "PreventSleep and AllowSleep")
	check(zebkit.OpenExternal("javascript:alert(1)") != nil, "OpenExternal refuses javascript:")
	_, err = zebkit.NewTray([]byte("not a png"), "x")
	check(err != nil, "a tray refuses what is not a PNG")
	check(zebkit.AppSetMenu([]map[string]any{{"title": "File", "items": []map[string]any{{"title": "Autosave", "id": "autosave", "checked": true}}}},
		func(string) {}) == nil && zebkit.AppUpdateMenuItem("autosave", map[string]any{"checked": false}) == nil, "the menu, changed in place")
	menu := zebkit.AppMenu()
	check(len(menu) == 1, "AppMenu", menu)
	zebkit.OnPowerEvent(func(map[string]any) {})
	zebkit.OnPowerEvent(nil)

	view.OnPermission(func(request *zebkit.PermissionRequest) { request.Respond(zebkit.PermissionDenied, false) })
	check(activated(view, "navigator.geolocation ? new Promise(r => navigator.geolocation.getCurrentPosition(() => r('yes'), e => r(e.code))) : 'none'") == "1",
		"OnPermission denies geolocation")

	view.Focus()
	view.OnEyeDropper(func(request *zebkit.EyeDropperRequest) { request.Respond("#123456") })
	check(activated(view, "new EyeDropper().open().then(r => r.sRGBHex, e => e.name)") == `"#123456"`, "OnEyeDropper")
	view.OnShare(func(request *zebkit.ShareRequest) { request.Respond(true) })
	check(activated(view, "navigator.share({title: 't'}).then(() => 'shared', e => e.name)") == `"shared"`, "OnShare")
	kinds := []string{}
	view.OnDeviceChooser(func(request *zebkit.DeviceChooserRequest) {
		kinds = append(kinds, fmt.Sprint(request.Metadata["kind"]))
		request.Respond("")
	})
	check(activated(view, "navigator.usb.requestDevice({filters: []}).then(() => 'x', e => e.name)") == `"NotFoundError"` &&
		len(kinds) == 1 && kinds[0] == "usb", "OnDeviceChooser", kinds)
	view.OnMediaAccess(func(request *zebkit.MediaAccessRequest) { request.Respond(true, "", "view") })
	check(activated(view, "navigator.mediaDevices.getDisplayMedia({video: true}).then(s => s.getVideoTracks()[0].getSettings().displaySurface, e => e.name)") == `"browser"`,
		"OnMediaAccess chooses the view for getDisplayMedia()")

	navigated := false
	view.OnNavigation(func(request *zebkit.NavigationRequest) {
		navigated = true
		request.Respond(zebkit.NavigationContinue, "")
	})
	view.Load("goapp://main/")
	check(wait(func() bool { return view.Title() == "From Go" }, 15) && navigated, "OnNavigation, and a page from a Go scheme handler", view.Title())

	view.Close()
	zebkit.Shutdown()
	fmt.Printf("%d of %d checks passed\n", checks-failures, checks)
	if failures > 0 {
		os.Exit(1)
	}
}
