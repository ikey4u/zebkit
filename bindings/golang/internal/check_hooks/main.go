// The Go binding's checks of the engine's hooks, settings, contexts, the
// script runtime and CDP, against a real engine. Exit 0 when every check
// passes.
//
//	go run ./internal/check_hooks --no-sandbox --headless
package main

import (
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	goruntime "runtime"
	"strings"
	"sync/atomic"
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
	return condition()
}

// evaluate runs script and waits for its JSON result; "timeout" when none
// came, "error: ..." for what the page threw.
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

func mustView(view *zebkit.View, err error) *zebkit.View {
	if err != nil {
		fail("a view: " + err.Error())
	}
	return view
}

// directory is the checks' scratch directory, removed however they end.
var directory string

// fail ends the checks, as a failure.
func fail(why string) {
	fmt.Println("FAIL " + why)
	os.RemoveAll(directory)
	os.Exit(1)
}

// activated runs script once the evaluation's user activation has reached
// the browser.
func activated(view *zebkit.View, script string) string {
	return evaluate(view, "new Promise(r => setTimeout(r, 200)).then(() => "+script+")")
}

func load(view *zebkit.View, url string) bool {
	view.Load(url)
	return wait(func() bool { return evaluate(view, "location.href + document.readyState") == `"`+url+`complete"` }, 15)
}

// scratchParent is where the checks' scratch directory goes: beside the
// profile (--user-data-dir) when there is one, else the system's.
func scratchParent() string {
	for _, arg := range os.Args[1:] {
		if dir, ok := strings.CutPrefix(arg, "--user-data-dir="); ok {
			return filepath.Dir(dir)
		}
	}
	return ""
}

func main() {
	var err error
	directory, err = os.MkdirTemp(scratchParent(), "zebkit-go-hooks-")
	if err != nil {
		fmt.Println("FAIL a scratch directory:", err)
		os.Exit(1)
	}
	defer os.RemoveAll(directory)

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		fmt.Fprint(w, "<title>Page</title><input type=file id=f><a id=mail href='mailto:someone@example.com'>mail</a>")
	})
	mux.HandleFunc("/data", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "from the server") })
	mux.HandleFunc("/target", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "redirected") })
	mux.HandleFunc("/download", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Disposition", "attachment; filename=notes.txt")
		fmt.Fprint(w, "downloaded bytes")
	})
	mux.HandleFunc("/private", func(w http.ResponseWriter, r *http.Request) {
		user, pass, ok := r.BasicAuth()
		if !ok || user != "alice" || pass != "secret" {
			w.Header().Set("WWW-Authenticate", `Basic realm="go"`)
			w.WriteHeader(401)
			fmt.Fprint(w, "<title>Denied</title>")
			return
		}
		fmt.Fprint(w, "<title>Signed in</title>")
	})
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	go http.Serve(listener, mux)
	base := fmt.Sprintf("http://%s", listener.Addr())

	page := func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, "<title>Secure</title>") }
	tlsServer := httptest.NewTLSServer(http.HandlerFunc(page))
	defer tlsServer.Close()
	clientServer := httptest.NewUnstartedServer(http.HandlerFunc(page))
	clientServer.TLS = &tls.Config{ClientAuth: tls.RequestClientCert}
	clientServer.StartTLS()
	defer clientServer.Close()

	// Before the engine starts.
	var wakeups atomic.Int64
	check(zebkit.SetWakeup(func(time.Duration) { wakeups.Add(1) }) == nil, "SetWakeup before starting")
	check(zebkit.SetCookieEncryptionKey(make([]byte, 31)) != nil, "a cookie key of the wrong size is refused")
	check(zebkit.SetCookieStorage(zebkit.CookieStorageFile, "") == nil, "SetCookieStorage before starting")
	_, err = zebkit.NewView("", 10, 10)
	check(err == zebkit.ErrNotStarted && zebkit.Post(func() {}) == zebkit.ErrNotStarted, "views and Post before the engine starts are refused")
	if err := zebkit.InitWithSettings(map[string]any{}, os.Args[1:]...); err != nil {
		fail("InitWithSettings: " + err.Error())
	}
	check(zebkit.SetWakeup(func(time.Duration) {}) != nil, "SetWakeup after starting is refused")
	view := mustView(zebkit.NewView("", 640, 480))
	check(load(view, base+"/"), "a page loads")
	check(wakeups.Load() > 0, "the engine asked for pumps (SetWakeup)", wakeups.Load())
	check(view.TargetID() != "", "TargetID")

	// Settings.
	check(view.SetSettings(map[string]any{"colorScheme": "dark"}) == nil &&
		evaluate(view, "matchMedia('(prefers-color-scheme: dark)').matches") == "true", "SetSettings: a dark page")
	settings, err := view.Settings()
	check(err == nil && settings["colorScheme"] == "dark", "Settings", settings, err)
	check(view.SetSettings(`{"colorScheme": null}`) == nil, "a setting reset, as JSON text")
	settings, err = view.Settings()
	check(err == nil && len(settings) == 0, "the setting is gone", settings)
	check(view.SetSettings(map[string]any{"noSuchSetting": 1}) != nil, "an unknown setting is refused")
	check(view.SetSettings(nil) != nil && view.SetSettings("not json") != nil, "settings that are null or not JSON are refused")
	var runtime map[string]any
	check(view.SetRuntimeSettings(map[string]any{"downloads": "deny"}) == nil, "SetRuntimeSettings")
	runtime, err = view.RuntimeSettings()
	explicit, _ := runtime["explicit"].(map[string]any)
	check(err == nil && explicit["downloads"] == "deny", "runtime settings", runtime)
	check(view.SetRuntimeSettings(map[string]any{"downloads": nil}) == nil, "a runtime setting reset")
	engine, err := zebkit.EngineSettings()
	check(err == nil && engine["effective"] != nil && zebkit.SetEngineSettings(map[string]any{}) == nil, "engine settings")

	// The permission table.
	check(view.SetPermission(base, "geolocation", zebkit.PermissionGranted) == nil &&
		view.Permission(base, "geolocation") == zebkit.PermissionGranted, "SetPermission and Permission")
	check(view.ResetPermissions("") == nil && view.Permission(base, "geolocation") != zebkit.PermissionGranted, "ResetPermissions")
	check(view.SetPermissionTimeout(5000) == nil && view.SetPermissionTimeout(0) != nil, "SetPermissionTimeout's range")
	permissionResults := 0
	view.OnPermissionResult(func(map[string]any) { permissionResults++ })
	view.OnPermission(func(r *zebkit.PermissionRequest) { r.Respond(zebkit.PermissionDenied, false) })
	activated(view, "new Promise(r => navigator.geolocation.getCurrentPosition(() => r(1), () => r(0)))")
	check(wait(func() bool { return permissionResults > 0 }, 5), "OnPermissionResult")
	view.OnPermission(nil)
	check(view.SetNavigationFailClosed(false) == nil && view.SetNavigationTimeout(5000) == nil, "navigation deadline settings")

	// The network: fulfil, redirect, block, a stream, events and bodies.
	// Nothing is asked until a filter says what: hold these, observe and read the others.
	check(view.SetNetworkFilter([]map[string]any{
		{"url": "**/fulfil"}, {"url": "**/moved"}, {"url": "**/blocked"}, {"url": "**/stream"}, {"url": "**/data"},
		{"url": "**/data", "mode": "observe"},
		{"url": "**/data", "mode": "observe", "stage": "response", "body": true},
	}) == nil, "SetNetworkFilter")
	check(view.SetNetworkFilter([]map[string]any{{"mode": "nonsense"}}) != nil, "a bad filter is refused")
	events, bodies := 0, 0
	outcomes := map[string]string{}
	view.OnNetworkEvent(func(event map[string]any) {
		events++
		url, _ := event["url"].(string)
		state, _ := event["state"].(string)
		if state != "" && event["reason"] == "answered" {
			outcomes[url[strings.LastIndex(url, "/")+1:]] = state
		}
		if _, ok := event["dropped"]; ok {
			if _, number := event["dropped"].(float64); !number {
				outcomes["dropped-not-a-number"] = fmt.Sprint(event["dropped"])
			}
		}
	})
	view.OnNetworkBody(func(id string, bytes []byte, end, truncated bool) { bodies += len(bytes) })
	view.OnNetwork(func(r *zebkit.NetworkRequest) {
		url, _ := r.Metadata()["url"].(string)
		switch {
		case strings.HasSuffix(url, "/fulfil"):
			r.Respond(map[string]any{"action": "fulfill", "status": 200,
				"headers": []map[string]string{{"name": "Content-Type", "value": "text/plain"}}}, []byte("from Go"))
		case strings.HasSuffix(url, "/moved"):
			r.Respond(map[string]any{"action": "redirect", "url": base + "/target"}, nil)
		case strings.HasSuffix(url, "/blocked"):
			r.Respond(map[string]any{"action": "block"}, nil)
		case strings.HasSuffix(url, "/stream"):
			stream, err := r.RespondStream(map[string]any{"status": 200,
				"headers": []map[string]string{{"name": "Content-Type", "value": "text/plain"}}})
			if err == nil {
				stream.Write([]byte("one "))
				stream.Write([]byte("two"))
				stream.End(false)
			}
		default:
			r.Respond(map[string]any{"action": "continue"}, nil)
		}
	})
	fetch := func(path string) string {
		return evaluate(view, "fetch('"+path+"').then(r => r.text(), e => 'failed')")
	}
	check(fetch("/fulfil") == `"from Go"`, "OnNetwork fulfils", fetch("/fulfil"))
	check(fetch("/moved") == `"redirected"`, "OnNetwork redirects")
	check(fetch("/blocked") == `"failed"`, "OnNetwork blocks")
	check(fetch("/stream") == `"one two"`, "RespondStream", fetch("/stream"))
	check(fetch("/data") == `"from the server"`, "OnNetwork continues")
	check(wait(func() bool { return events > 0 && bodies > 0 }, 5), "OnNetworkEvent and OnNetworkBody", events, bodies)
	check(wait(func() bool { return outcomes["data"] == "continued" }, 5) && outcomes["fulfil"] == "fulfilled" &&
		outcomes["moved"] == "redirected" && outcomes["blocked"] == "blocked" && outcomes["dropped-not-a-number"] == "",
		"the outcomes of held requests: continued, fulfilled, redirected, blocked", outcomes)
	check(view.SetNetworkTimeout(5000) == nil && view.SetNetworkFilterForWorkers(true) == nil && view.SetNetworkFilterForWorkers(false) == nil,
		"network settings")
	view.SetNetworkFilter([]any{})
	view.OnNetwork(nil)
	view.OnNetworkEvent(nil)
	view.OnNetworkBody(nil)

	// Downloads.
	var saved string
	progress := map[string]any{}
	view.OnDownloadProgress(func(event map[string]any) { progress = event })
	view.OnDownload(func(r *zebkit.DownloadRequest) {
		saved = filepath.Join(directory, "notes.txt")
		r.Save(saved)
	})
	view.Load(base + "/download")
	check(wait(func() bool { return progress["state"] == "complete" }, 15), "a download saved where the application said", progress)
	guid, _ := progress["guid"].(string)
	check(progress["outcome"] == "complete" && progress["savedPath"] == saved && progress["error"] == nil && guid != "",
		"its outcome: complete, savedPath, guid, no error", progress)
	bytes, _ := os.ReadFile(saved)
	check(string(bytes) == "downloaded bytes", "its bytes", string(bytes))
	cancelled := false
	view.OnDownload(func(r *zebkit.DownloadRequest) { cancelled = r.Cancel() == nil })
	view.Load(base + "/download")
	check(wait(func() bool { return cancelled }, 15), "a download cancelled")
	failure, _ := progress["error"].(map[string]any)
	check(wait(func() bool {
		failure, _ = progress["error"].(map[string]any)
		return progress["outcome"] == "cancelled"
	}, 15) &&
		failure != nil && failure["category"] == "cancelled" && progress["savedPath"] == nil,
		"its outcome: cancelled, with the error's category", progress)
	check(view.CancelDownload("no-such-download") != nil, "CancelDownload of none")
	// What was decided, on OnDownloadResult: saved or cancelled, and why.
	var results []map[string]any
	view.OnDownloadResult(func(event map[string]any) { results = append(results, event) })
	result := func(state, reason string) bool {
		return wait(func() bool {
			for _, r := range results {
				if r["state"] == state && r["reason"] == reason {
					return true
				}
			}
			return false
		}, 15)
	}
	view.OnDownload(func(r *zebkit.DownloadRequest) { r.Save(filepath.Join(directory, "again.txt")) })
	view.Load(base + "/download")
	check(result("saved", "answered"), "OnDownloadResult: saved, answered", results)
	view.OnDownload(func(r *zebkit.DownloadRequest) { r.Cancel() })
	view.Load(base + "/download")
	check(result("cancelled", "answered"), "OnDownloadResult: cancelled, answered", results)
	check(view.SetDownloadTimeout(50) == nil, "SetDownloadTimeout")
	view.OnDownload(func(r *zebkit.DownloadRequest) {})
	view.Load(base + "/download")
	check(result("cancelled", "timeout"), "OnDownloadResult: an unanswered download times out, cancelled", results)
	check(view.SetDownloadTimeout(30000) == nil, "SetDownloadTimeout back")
	held := false
	view.OnDownload(func(r *zebkit.DownloadRequest) { held = true })
	view.Load(base + "/download")
	check(wait(func() bool { return held }, 15), "a download held")
	view.OnDownload(func(r *zebkit.DownloadRequest) {})
	check(result("cancelled", "handler-changed"), "OnDownloadResult: the function replaced, cancelled", results)
	for _, r := range results {
		if _, ok := r["state"].(string); !ok || r["state"] == "save" {
			check(false, "every download result has a past-tense state", r)
			break
		}
	}
	view.OnDownloadResult(nil)
	view.OnDownload(nil)

	// A certificate the application accepts.
	asked := 0
	view.OnCertificate(func(r *zebkit.CertificateRequest) { asked++; r.Respond(true) })
	view.Load(tlsServer.URL + "/")
	check(wait(func() bool { return view.Title() == "Secure" }, 15) && asked > 0, "OnCertificate accepts a self-signed certificate", view.Title())

	// A server asking for a client certificate: none given.
	clientAsked := 0
	view.OnClientCertificate(func(r *zebkit.ClientCertificateRequest) { clientAsked++; r.None() })
	view.Load(clientServer.URL + "/")
	check(wait(func() bool { return clientAsked > 0 && view.Title() == "Secure" }, 15), "OnClientCertificate: none", clientAsked, view.Title())
	view.OnCertificate(nil)
	view.OnClientCertificate(nil)

	// HTTP authentication.
	var authResult map[string]any
	view.OnHTTPAuthResult(func(event map[string]any) { authResult = event })
	view.OnHTTPAuth(func(r *zebkit.HTTPAuthRequest) { r.Respond("alice", "secret") })
	view.Load(base + "/private")
	check(wait(func() bool { return view.Title() == "Signed in" }, 15), "OnHTTPAuth signs in", view.Title())
	check(wait(func() bool { return authResult != nil }, 5) && authResult["state"] == "provided" &&
		authResult["reason"] == "answered", "OnHTTPAuthResult: provided, answered", authResult)
	view.OnHTTPAuth(nil)
	view.OnHTTPAuthResult(nil)

	// A file chooser.
	file := filepath.Join(directory, "picked.txt")
	os.WriteFile(file, []byte("x"), 0o600)
	check(load(view, base+"/"), "the page again")
	view.Focus()
	view.OnFileChooser(func(r *zebkit.FileChooserRequest) { r.Respond([]string{file}) })
	evaluate(view, "window.picked = new Promise(r => f.onchange = () => r(f.files[0].name)); 1")
	activated(view, "f.click(), 1")
	check(evaluate(view, "window.picked") == `"picked.txt"`, "OnFileChooser gives a file")
	view.OnFileChooser(nil)

	// A link to another program.
	var external map[string]any
	view.OnExternalProtocol(func(event map[string]any) { external = event })
	activated(view, "mail.click(), 1")
	check(wait(func() bool { return external != nil }, 5) && external["scheme"] == "mailto", "OnExternalProtocol", external)

	// Keys, fullscreen, the console, scripts in a world.
	check(view.SetReservedKeys([]string{"Ctrl+Shift+KeyI"}) == nil && view.SetReservedKeys([]string{"nonsense+"}) != nil,
		"SetReservedKeys checks its keys")
	view.OnReservedKey(func(map[string]any) {})
	check(view.SetFullscreenPolicy(zebkit.FullscreenDeny) == nil, "SetFullscreenPolicy")
	denied := activated(view, "document.body.requestFullscreen().then(() => 'full', e => e.name)")
	check(denied == `"TypeError"`, "fullscreen denied, with the page's TypeError", denied)
	view.SetFullscreenPolicy(zebkit.FullscreenAllow)
	var console map[string]any
	view.OnConsole(func(event map[string]any) { console = event })
	evaluate(view, "console.log('hello from the page'), 1")
	check(wait(func() bool { return console != nil }, 5) && strings.Contains(fmt.Sprint(console), "hello from the page"), "OnConsole", console)
	check(view.AddWorldScript("", "window.fromWorld = 7", true) == nil && load(view, base+"/") &&
		evaluate(view, "window.fromWorld") == "7", "AddWorldScript")

	// Popups.
	check(view.SetPopupPolicy(zebkit.PopupBlock) == nil && view.PopupPolicy() == zebkit.PopupBlock, "SetPopupPolicy and PopupPolicy")
	blocked := false
	view.OnPopupBlocked(func(map[string]any) { blocked = true })
	activated(view, "window.open('/data'), 1")
	check(wait(func() bool { return blocked }, 5), "OnPopupBlocked")
	view.SetPopupPolicy(zebkit.PopupAllow)
	var opened *zebkit.View
	view.OnWindowOpen(func(v *zebkit.View, url string) { opened = v })
	activated(view, "window.open('/target'), 1")
	check(wait(func() bool { return opened != nil }, 10), "OnWindowOpen gives a view")
	if opened != nil {
		info, err := opened.WindowOpenInfo()
		check(err == nil && info["url"] != nil, "WindowOpenInfo", info, err)
		same, err := zebkit.ViewByPageID(opened.AutomationPageID())
		check(err == nil && same == opened, "one View for one view, however it is found", err)
		// The opener's page closes it.
		openedClosed := false
		opened.OnClose(func() { openedClosed = true })
		evaluate(opened, "window.close(), 1")
		check(wait(func() bool { return openedClosed }, 10) && opened.IsClosed(), "a popup closed by its page")
		opened.Close()
		check(opened.TargetID() == "" && opened.SetSettings(map[string]any{}) == zebkit.ErrClosed, "its methods after")
		calledClosed := false
		check(opened.SetUserAgentOverride(`{"userAgent":"x"}`, func(json.RawMessage, error) { calledClosed = true }) == zebkit.ErrClosed &&
			!calledClosed, "SetUserAgentOverride of a closed view: ErrClosed, done not called")
	}
	view.OnWindowOpen(nil)
	var newURL string
	view.OnNewWindow(func(url string) { newURL = url })
	activated(view, "window.open('/data'), 1")
	check(wait(func() bool { return strings.HasSuffix(newURL, "/data") }, 10), "OnNewWindow takes the URL", newURL)
	view.OnNewWindow(nil)
	check(view.SetPopupWindowOptions(nil) == nil, "SetPopupWindowOptions(nil)")

	// The user agent.
	var agentDone bool
	var agentErr error
	check(view.SetUserAgentOverride(map[string]any{"userAgent": "GoAgent/1"}, func(_ json.RawMessage, err error) { agentDone, agentErr = true, err }) == nil,
		"SetUserAgentOverride queued")
	check(wait(func() bool { return agentDone }, 10) && agentErr == nil && evaluate(view, "navigator.userAgent") == `"GoAgent/1"`,
		"the user agent overridden", agentErr)
	invalid := []error{}
	queued := view.SetUserAgentOverride("{not json", func(_ json.RawMessage, err error) { invalid = append(invalid, err) })
	zeroed := view.SetUserAgentOverride("{\"userAgent\":\"a\x00b\"}", func(_ json.RawMessage, err error) { invalid = append(invalid, err) })
	// An engine before 0.5.0 also returned false for unreadable options.
	check((queued == nil || errors.Is(queued, zebkit.ErrRefused)) && zeroed == nil && wait(func() bool { return len(invalid) == 2 }, 10) && invalid[0] != nil && invalid[1] != nil,
		"SetUserAgentOverride: options it cannot read are told once, to done", queued, zeroed, invalid)

	// Calls answered with the page's JSON as written.
	view.Grant(base, zebkit.AllowApp, "")
	view.Bind("raw", func(c *zebkit.Call) { c.ResolveJSON([]byte(`{"a":[1,2]}`)) })
	check(load(view, base+"/") && evaluate(view, "zebkit.app.raw().then(v => v.a[1])") == "2", "ResolveJSON")
	// Calls dropped: by Close, and by the garbage collector.
	var closeAgain error
	view.Bind("closed", func(c *zebkit.Call) { c.Close(); closeAgain = c.Close() })
	check(evaluate(view, "zebkit.app.closed().then(() => 'resolved', e => e.message)") == `"the application dropped the call"` &&
		closeAgain == zebkit.ErrClosed, "Call.Close rejects the page's promise, once", closeAgain)
	view.Bind("dropped", func(c *zebkit.Call) {})
	view.Evaluate("globalThis.dropped = 'pending'; zebkit.app.dropped().then(() => dropped = 'resolved', e => dropped = e.message), 1", nil)
	check(wait(func() bool {
		goruntime.GC()
		return evaluate(view, "dropped") == `"the application dropped the call"`
	}, 15), "a Call let go is dropped once collected", evaluate(view, "dropped"))
	view.Bind("closed", nil)
	view.Bind("dropped", nil)

	// The page's process gone, and started again.
	{
		crashing := mustView(zebkit.NewView(base+"/", 320, 240))
		var gone []zebkit.ProcessGone
		crashing.OnProcessGone(func(g zebkit.ProcessGone) { gone = append(gone, g) })
		check(load(crashing, base+"/"), "a page to crash")
		crashing.Load("chrome://crash")
		check(wait(func() bool { return len(gone) > 0 }, 15) && gone[0].Reason == "crashed" && !crashing.IsClosed(),
			"OnProcessGone: crashed, the view still open", gone)
		check(load(crashing, base+"/"), "a page loads again after the crash")
		crashing.OnProcessGone(nil)
		crashing.Load("chrome://crash")
		wait(func() bool { return false }, 1)
		check(len(gone) == 1, "OnProcessGone(nil): nothing told", gone)
		crashing.OnResponsiveness(func(bool) {})
		crashing.OnResponsiveness(nil)
		crashing.Close()
	}

	// The desktop, asked and told how it went.
	var desktopErrs []error
	desktopDone := 0
	zebkit.OpenExternalAsync("javascript:alert(1)", func(err error) { desktopDone++; desktopErrs = append(desktopErrs, err) })
	zebkit.ShowInFolderAsync(filepath.Join(directory, "no-such-file"), func(err error) { desktopDone++; desktopErrs = append(desktopErrs, err) })
	zebkit.ShowInFolderAsync("relative/path", func(err error) { desktopDone++; desktopErrs = append(desktopErrs, err) })
	check(desktopDone == 0, "the async desktop calls answer after they return")
	check(wait(func() bool { return desktopDone == 3 }, 10) && desktopErrs[0] != nil && desktopErrs[1] != nil && desktopErrs[2] != nil,
		"OpenExternalAsync of javascript:, ShowInFolderAsync of a missing file and a relative path: errors", desktopErrs)
	check(zebkit.OpenExternal("javascript:alert(1)") != nil, "OpenExternal of javascript: is refused")

	// Windows and the system's handles, without a display.
	view.SetSizeLimits(100, 100, true)
	view.SetBounds(0, 0, 320, 240)
	view.SetVisible(true)
	_, _, hasWindow := view.NativeWindow()
	check(!hasWindow == zebkit.IsHeadless() || hasWindow, "NativeWindow")
	exported := false
	if view.ExportNativeWindow(func(string, bool) { exported = true }) == nil {
		check(wait(func() bool { return exported }, 5), "ExportNativeWindow answers")
	}
	// A view that closes first is told, before OnClose's function.
	{
		closing := mustView(zebkit.NewView("", 100, 100))
		var order []string
		closing.OnClose(func() { order = append(order, "closed") })
		if closing.ExportNativeWindow(func(handle string, ok bool) { order = append(order, fmt.Sprint("export ", ok)) }) == nil {
			closing.Close()
			check(wait(func() bool { return len(order) == 2 }, 10) && order[0] == "export false" && order[1] == "closed",
				"ExportNativeWindow of a view that closes: ok false, before OnClose", order)
		}
	}
	check(view.OnAuthentication(func(map[string]any) {}) == nil && view.OnAuthentication(nil) == nil, "OnAuthentication set and removed")
	view.OnDrop(func([]string) {})
	view.OnDrop(nil)
	check(view.RunContextMenuCommand(999999, "copy") != nil, "RunContextMenuCommand of no menu is refused")
	check(view.ShowContextMenu(999999, nil, func(string) {}) != nil, "ShowContextMenu of no menu is refused")
	zebkit.OnAppOpenFiles(func([]string) {})
	zebkit.OnAppReopen(func() {})
	zebkit.OnAppQuitRequest(func() {})
	zebkit.OnAppOpenFiles(nil)
	zebkit.OnAppReopen(nil)
	zebkit.OnAppQuitRequest(nil)

	// A context of its own: cookies kept apart.
	evaluate(view, "document.cookie = 'k=main', 1")
	context, err := zebkit.NewContext()
	check(err == nil, "NewContext", err)
	other := mustView(context.NewView(base+"/", 320, 240))
	check(wait(func() bool { return evaluate(other, "document.readyState") == `"complete"` }, 15) &&
		evaluate(other, "document.cookie") == `""`, "a context's cookies are its own")
	otherClosed := false
	other.OnClose(func() { otherClosed = true })
	context.Close()
	check(wait(func() bool { return otherClosed }, 10) && other.IsClosed(), "a context's views close with it")
	_, err = context.NewView("", 10, 10)
	check(err == zebkit.ErrClosed, "a closed context makes no views", err)

	// Script outside any page.
	script, err := zebkit.NewScript("one")
	if err != nil {
		fail("NewScript: " + err.Error())
	}
	var scriptResult string
	script.Evaluate("zebkit.args.length + 40", func(value json.RawMessage, err error) { scriptResult = string(value) })
	check(wait(func() bool { return scriptResult != "" }, 10) && scriptResult == "41", "Script.Evaluate", scriptResult)
	module := filepath.Join(directory, "module.mjs")
	os.WriteFile(module, []byte("globalThis.ran = 1;"), 0o600)
	moduleDone := false
	var moduleErr error
	script.RunModule(module, func(err error) { moduleDone, moduleErr = true, err })
	check(wait(func() bool { return moduleDone }, 10) && moduleErr == nil, "Script.RunModule", moduleErr)
	script.Close()
	var closedErr error
	script.Evaluate("1", func(_ json.RawMessage, err error) { closedErr = err })
	check(closedErr == zebkit.ErrClosed, "a closed script answers ErrClosed")

	// CDP.
	port, err := zebkit.ListenCDP("", 0)
	check(err == nil && port > 0 && zebkit.CDPPort() == port, "CDPListen", err)
	var version map[string]any
	go func() {
		response, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/json/version", port))
		if err == nil {
			body, _ := io.ReadAll(response.Body)
			response.Body.Close()
			var value map[string]any
			json.Unmarshal(body, &value)
			version = value
		}
	}()
	check(wait(func() bool { return version != nil }, 10) && strings.Contains(fmt.Sprint(version["Browser"]), "Chrome"), "CDP's /json/version", version)
	check(strings.HasPrefix(zebkit.CDPURL(), "ws://"), "CDPURL", zebkit.CDPURL())
	zebkit.StopCDP()
	check(zebkit.CDPPort() == 0, "StopCDP")
	var asyncPort, asyncPort2 uint
	var asyncURL string
	var asyncErr, asyncErr2 error
	asyncDone := 0
	zebkit.ListenCDPAsync("", 0, func(port uint, url string, err error) { asyncDone++; asyncPort, asyncURL, asyncErr = port, url, err })
	zebkit.ListenCDPAsync("", 0, func(port uint, url string, err error) { asyncDone++; asyncPort2, asyncErr2 = port, err })
	check(asyncDone == 0, "ListenCDPAsync answers after it returns")
	check(wait(func() bool { return asyncDone == 2 }, 15) && asyncErr == nil && asyncErr2 == nil && asyncPort > 0 &&
		asyncPort2 == asyncPort && asyncPort == zebkit.CDPPort() && strings.HasPrefix(asyncURL, "ws://") && asyncURL == zebkit.CDPURL(),
		"ListenCDPAsync: two calls while it starts, one port and its URL", asyncPort, asyncPort2, asyncURL, asyncErr, asyncErr2)
	var otherErr error
	otherDone := false
	zebkit.ListenCDPAsync("", asyncPort+1, func(port uint, url string, err error) { otherDone, otherErr = true, err })
	check(wait(func() bool { return otherDone }, 10) && otherErr != nil, "ListenCDPAsync of another port while served: an error", otherErr)
	_, err = zebkit.ListenCDP("", asyncPort+1)
	check(err != nil, "ListenCDP of another port while served: an error")
	var stoppedErr error
	stoppedDone := false
	zebkit.StopCDP()
	zebkit.ListenCDPAsync("", 0, func(port uint, url string, err error) { stoppedDone, stoppedErr = true, err })
	zebkit.StopCDP()
	check(wait(func() bool { return stoppedDone }, 10) && stoppedErr != nil, "StopCDP answers a starting ListenCDPAsync with an error", stoppedErr)
	check(zebkit.CDPPort() == 0, "StopCDP again")

	view.Close()
	zebkit.Shutdown()
	fmt.Printf("%d of %d checks passed\n", checks-failures, checks)
	if failures > 0 {
		os.RemoveAll(directory)
		os.Exit(1)
	}
}
