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
	"runtime"
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

// evaluate runs script and waits for its JSON result; "timeout" when none
// came.
func evaluate(view *zebkit.View, script string) string {
	result := "timeout"
	done := false
	view.Evaluate(script, func(value json.RawMessage, err error) {
		if err != nil {
			result, done = "error: "+err.Error(), true
			return
		}
		result, done = string(value), true
	})
	wait(func() bool { return done }, 15)
	return result
}

// mustView is a view, or the end of the checks.
func mustView(view *zebkit.View, err error) *zebkit.View {
	if err != nil {
		fmt.Println("FAIL a view:", err)
		os.Exit(1)
	}
	return view
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

	check(zebkit.RegisterScheme("goapp", zebkit.SchemeStandard|zebkit.SchemeSecure|zebkit.SchemeFetch) == nil, "RegisterScheme")
	// Before the engine has started, what it would never answer is answered.
	var listenErr, callErr error
	zebkit.ListenAutomation(0, func(_ string, err error) { listenErr = err })
	zebkit.CallAutomation(nil, "Browser.newContext", nil, func(_ json.RawMessage, err error) { callErr = err })
	_, pageErr := zebkit.ViewByPageID("p1")
	check(listenErr == zebkit.ErrNotStarted && callErr == zebkit.ErrNotStarted && pageErr == zebkit.ErrNotStarted,
		"automation before the engine starts answers ErrNotStarted", listenErr, callErr, pageErr)
	if err := zebkit.Init(os.Args[1:]...); err != nil {
		fmt.Println("FAIL zebkit.Init:", err)
		os.Exit(1)
	}
	check(zebkit.Version() != "" && strings.Contains(zebkit.EngineVersion(), "Chromium"), "versions", zebkit.Version(), zebkit.EngineVersion())

	zebkit.HandleScheme("goapp", func(request *zebkit.Request) {
		if strings.HasSuffix(request.URL(), "/dropped") {
			return // let go unanswered: answered 404 once collected
		}
		request.Respond(200, "Content-Type: text/html", []byte("<title>From Go</title>"))
		check(request.Respond(200, "", nil) == zebkit.ErrClosed && request.URL() == "", "a scheme request is answered once")
	})

	// Grants and functions before the page: its zebkit object is made with it.
	view := mustView(zebkit.NewView("", 400, 300))
	view.Grant(strings.TrimSuffix(url, "/"), zebkit.AllowApp, "")
	view.Bind("twice", func(call *zebkit.Call) {
		var n float64
		if err := call.ArgsInto(&n); err != nil {
			call.Reject(err.Error())
			return
		}
		call.ResolveValue(n * 2)
	})
	view.Bind("greet", func(call *zebkit.Call) {
		var name string
		call.ArgsInto(&name)
		call.ResolveValue("hello, " + name)
		check(call.ResolveValue("again") == zebkit.ErrClosed, "a call is answered once")
	})
	view.Bind("later", func(call *zebkit.Call) {
		// Answered from another goroutine, as the C API allows.
		go func() {
			call.ResolveValue("from a goroutine")
		}()
	})
	view.Bind("gone", func(call *zebkit.Call) { call.ResolveValue(1) })
	view.Bind("gone", nil)
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
	got = evaluate(view, "zebkit.app.greet('Go')")
	check(got == `"hello, Go"`, "a call answered with a Go string (ArgsInto, ResolveValue)", got)
	got = evaluate(view, "zebkit.app.later()")
	check(got == `"from a goroutine"`, "a call answered from another goroutine", got)
	got = evaluate(view, "typeof zebkit.app.gone === 'function' ? zebkit.app.gone().then(() => 'answered', e => 'rejected') : 'removed'")
	check(got == `"removed"` || got == `"rejected"`, "Bind(name, nil) removes the function", got)

	var png []byte
	view.Capture(func(bytes []byte, err error) { png = bytes })
	check(wait(func() bool { return png != nil }, 15) && string(png[1:4]) == "PNG", "Capture")

	_, displaysErr := zebkit.Displays()
	environment, environmentErr := zebkit.Environment()
	check(displaysErr == nil && environmentErr == nil && environment["colorScheme"] != nil, "Displays and Environment",
		displaysErr, environmentErr, environment)
	check(zebkit.SetAppBadge("2") == nil && zebkit.SetAppProgress(0.5) == nil && zebkit.SetAppProgress(3) != nil, "badge and progress")
	token, err := zebkit.PreventSleep("check", false)
	check(err == nil && zebkit.AllowSleep(token) == nil && zebkit.AllowSleep(token) != nil, "PreventSleep and AllowSleep")
	check(zebkit.OpenExternal("javascript:alert(1)") != nil, "OpenExternal refuses javascript:")
	_, err = zebkit.NewTray([]byte("not a png"), "x")
	check(err != nil, "a tray refuses what is not a PNG")
	check(zebkit.SetAppMenu([]map[string]any{{"title": "File", "items": []map[string]any{{"title": "Autosave", "id": "autosave", "checked": true}}}},
		func(string) {}) == nil && zebkit.UpdateAppMenuItem("autosave", map[string]any{"checked": false}) == nil, "the menu, changed in place")
	check(zebkit.SetAppMenu(`[{"title": "Edit", "items": [{"role": "copy"}]}]`, nil) == nil, "a role-only menu, no function, raw JSON")
	check(zebkit.SetAppMenu("not json", func(string) {}) != nil, "a menu that is not JSON is refused")
	menu, err := zebkit.AppMenu()
	check(err == nil && len(menu) == 1 && menu[0]["title"] == "Edit", "AppMenu keeps the menu accepted last", menu, err)
	zebkit.OnPowerEvent(func(map[string]any) {})
	zebkit.OnPowerEvent(nil)

	view.OnPermission(func(request *zebkit.PermissionRequest) { request.Respond(zebkit.PermissionDenied, false) })
	check(activated(view, "navigator.geolocation ? new Promise(r => navigator.geolocation.getCurrentPosition(() => r('yes'), e => r(e.code))) : 'none'") == "1",
		"OnPermission denies geolocation")

	view.Focus()
	view.OnEyeDropper(nil)
	view.OnEyeDropper(func(request *zebkit.EyeDropperRequest) { request.Respond("#123456") })
	check(activated(view, "new EyeDropper().open().then(r => r.sRGBHex, e => e.name)") == `"#123456"`, "OnEyeDropper")
	view.OnShare(func(request *zebkit.ShareRequest) { request.Respond(true) })
	check(activated(view, "navigator.share({title: 't'}).then(() => 'shared', e => e.name)") == `"shared"`, "OnShare")
	kinds := []string{}
	view.OnDeviceChooser(func(request *zebkit.DeviceChooserRequest) {
		kinds = append(kinds, fmt.Sprint(request.Metadata()["kind"]))
		request.Respond("")
	})
	check(activated(view, "navigator.usb.requestDevice({filters: []}).then(() => 'x', e => e.name)") == `"NotFoundError"` &&
		len(kinds) == 1 && kinds[0] == "usb", "OnDeviceChooser", kinds)
	var filters any
	view.OnDeviceChooser(func(request *zebkit.DeviceChooserRequest) {
		filters = request.Metadata()["filters"]
		request.Respond("")
	})
	activated(view, "navigator.serial.requestPort({filters: [{usbVendorId: 0x1234, usbProductId: 0x5678}]}).then(() => 'x', e => e.name)")
	check(fmt.Sprint(filters) == "[map[productId:22136 vendorId:4660]]", "OnDeviceChooser: serial filters as vendorId and productId", filters)
	view.OnMediaAccess(func(request *zebkit.MediaAccessRequest) { request.Respond(true, "", "view") })
	check(activated(view, "navigator.mediaDevices.getDisplayMedia({video: true}).then(s => s.getVideoTracks()[0].getSettings().displaySurface, e => e.name)") == `"browser"`,
		"OnMediaAccess chooses the view for getDisplayMedia()")

	navigated := false
	var navigation map[string]any
	view.OnNavigationResult(func(event map[string]any) { navigation = event })
	view.OnNavigation(func(request *zebkit.NavigationRequest) {
		navigated = true
		request.Respond(zebkit.NavigationContinue, "")
	})
	view.Load("goapp://main/")
	check(wait(func() bool { return view.Title() == "From Go" }, 15) && navigated, "OnNavigation, and a page from a Go scheme handler", view.Title())
	_, hasAction := navigation["action"]
	check(wait(func() bool { return navigation != nil }, 5) && navigation["state"] == "continued" && navigation["reason"] == "answered" && !hasAction,
		"OnNavigationResult: continued, answered", navigation)
	view.OnNavigationResult(nil)

	// A scheme request let go unanswered is answered 404 once collected.
	dropped := "timeout"
	view.Evaluate("fetch('goapp://main/dropped').then(r => r.status, e => e.name)", func(value json.RawMessage, err error) {
		dropped = string(value)
	})
	wait(func() bool { runtime.GC(); return dropped != "timeout" }, 15)
	check(dropped == "404", "a scheme request let go unanswered is answered 404", dropped)

	// A view the page closes (window.close()), not the application: its
	// View is told and is closed; its methods then do nothing.
	closing := mustView(zebkit.NewView("data:text/html,<title>Closing</title>", 200, 100))
	closedByPage := false
	closing.OnClose(func() { closedByPage = true })
	wait(func() bool { return closing.Title() == "Closing" }, 10)
	closing.Evaluate("window.close()", nil)
	check(wait(func() bool { return closedByPage }, 10) && closing.IsClosed(), "a view closed by its page's window.close()")
	var afterErr error
	closing.Evaluate("1", func(_ json.RawMessage, err error) { afterErr = err })
	closing.Load("about:blank")
	closing.OnLoad(func(zebkit.LoadEvent, string) {})
	closing.Close()
	_, windowErr := closing.Window()
	check(afterErr == zebkit.ErrClosed && windowErr == zebkit.ErrClosed && closing.URL() == "" && closing.Title() == "",
		"a closed view's methods return ErrClosed or do nothing", afterErr, windowErr)

	// Post: from another goroutine, onto the engine's thread.
	posted := make(chan bool, 1)
	go func() {
		if zebkit.Post(func() { posted <- view.Title() == "From Go" }) != nil {
			posted <- false
		}
	}()
	var postedOK bool
	check(wait(func() bool {
		select {
		case postedOK = <-posted:
			return true
		default:
			return false
		}
	}, 10) && postedOK, "Post runs on the engine's thread")

	// A view's method from another thread panics, rather than run there.
	panicked := make(chan bool, 1)
	go func() {
		defer func() { panicked <- recover() != nil }()
		view.Title()
	}()
	check(<-panicked, "a view's method off the engine's thread panics")
	go func() {
		defer func() { panicked <- recover() != nil }()
		zebkit.SetAppBadge("")
	}()
	check(<-panicked, "a package function that reaches the engine panics off its thread")

	// A view attached to itself is refused.
	check(view.Attach(view, 0, 0, 10, 10) != nil, "Attach refuses the view as its own parent")

	// Shutdown inside a callback is refused, and the engine runs on.
	var nestedErr error
	nestedDone := false
	check(zebkit.Post(func() { nestedErr, nestedDone = zebkit.Shutdown(), true }) == nil, "Post")
	check(wait(func() bool { return nestedDone }, 10) && nestedErr != nil && zebkit.Post(func() {}) == nil &&
		evaluate(view, "1 + 1") == "2", "Shutdown inside a callback is refused, and the engine runs on", nestedErr)

	// Connection reuse (connections.go).
	connections()

	// JavaScript libraries (library.go).
	libraries()

	view.Close()
	check(zebkit.Shutdown() == nil, "Shutdown")
	check(zebkit.Post(func() {}) == zebkit.ErrNotStarted, "Post after Shutdown is refused")
	fmt.Printf("%d of %d checks passed\n", checks-failures, checks)
	if failures > 0 {
		os.Exit(1)
	}
}
