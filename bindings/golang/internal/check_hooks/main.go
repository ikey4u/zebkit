// The Go binding's checks of the engine's hooks, settings, contexts, the
// script runtime and CDP, against a real engine. Exit 0 when every check
// passes.
//
//	go run ./internal/check_hooks --no-sandbox --headless
package main

import (
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
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
// the browser.
func activated(view *zebkit.View, script string) string {
	return evaluate(view, "new Promise(r => setTimeout(r, 200)).then(() => "+script+")")
}

func load(view *zebkit.View, url string) bool {
	view.Load(url)
	return wait(func() bool { return evaluate(view, "location.href + document.readyState") == `"`+url+`complete"` }, 15)
}

func main() {
	directory, _ := os.MkdirTemp("", "zebkit-go-hooks-")
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
	check(zebkit.SetWakeup(func(time.Duration) { wakeups.Add(1) }), "SetWakeup before starting")
	check(!zebkit.SetCookieEncryptionKey(make([]byte, 31)), "a cookie key of the wrong size is refused")
	check(zebkit.SetCookieStorage(zebkit.CookieStorageFile, ""), "SetCookieStorage before starting")
	if err := zebkit.InitWithSettings(map[string]any{}, os.Args[1:]...); err != nil {
		fmt.Println("FAIL InitWithSettings:", err)
		os.Exit(1)
	}
	view := zebkit.NewView("", 640, 480)
	check(load(view, base+"/"), "a page loads")
	check(wakeups.Load() > 0, "the engine asked for pumps (SetWakeup)", wakeups.Load())
	check(view.TargetID() != "", "TargetID")

	// Settings.
	check(view.SetSettings(map[string]any{"colorScheme": "dark"}) == nil &&
		evaluate(view, "matchMedia('(prefers-color-scheme: dark)').matches") == "true", "SetSettings: a dark page")
	check(view.Settings()["colorScheme"] == "dark", "Settings", view.Settings())
	check(view.SetSettings(map[string]any{"colorScheme": nil}) == nil && len(view.Settings()) == 0, "a setting reset")
	check(view.SetSettings(map[string]any{"noSuchSetting": 1}) != nil, "an unknown setting is refused")
	check(view.SetRuntimeSettings(map[string]any{"downloads": "deny"}) == nil &&
		view.RuntimeSettings()["explicit"].(map[string]any)["downloads"] == "deny", "runtime settings", view.RuntimeSettings())
	check(view.SetRuntimeSettings(map[string]any{"downloads": nil}) == nil, "a runtime setting reset")
	check(zebkit.EngineSettings()["effective"] != nil && zebkit.SetEngineSettings(map[string]any{}) == nil, "engine settings")

	// The permission table.
	check(view.SetPermission(base, "geolocation", zebkit.PermissionGranted) == nil &&
		view.Permission(base, "geolocation") == zebkit.PermissionGranted, "SetPermission and Permission")
	check(view.ResetPermissions("") == nil && view.Permission(base, "geolocation") != zebkit.PermissionGranted, "ResetPermissions")
	check(view.SetPermissionTimeout(5000) && !view.SetPermissionTimeout(0), "SetPermissionTimeout's range")
	permissionResults := 0
	view.OnPermissionResult(func(map[string]any) { permissionResults++ })
	view.OnPermission(func(r *zebkit.PermissionRequest) { r.Respond(zebkit.PermissionDenied, false) })
	activated(view, "new Promise(r => navigator.geolocation.getCurrentPosition(() => r(1), () => r(0)))")
	check(wait(func() bool { return permissionResults > 0 }, 5), "OnPermissionResult")
	view.OnPermission(nil)
	check(view.SetNavigationFailClosed(false) && view.SetNavigationTimeout(5000), "navigation deadline settings")

	// The network: fulfil, redirect, block, a stream, events and bodies.
	// Nothing is asked until a filter says what: hold these, observe and read the others.
	check(view.SetNetworkFilter([]map[string]any{
		{"url": "**/fulfil"}, {"url": "**/moved"}, {"url": "**/blocked"}, {"url": "**/stream"}, {"url": "**/data"},
		{"url": "**/data", "mode": "observe"},
		{"url": "**/data", "mode": "observe", "stage": "response", "body": true},
	}) == nil, "SetNetworkFilter")
	check(view.SetNetworkFilter([]map[string]any{{"mode": "nonsense"}}) != nil, "a bad filter is refused")
	events, bodies := 0, 0
	view.OnNetworkEvent(func(map[string]any) { events++ })
	view.OnNetworkBody(func(id string, bytes []byte, end, truncated bool) { bodies += len(bytes) })
	view.OnNetwork(func(r *zebkit.NetworkRequest) {
		url, _ := r.Metadata["url"].(string)
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
	check(view.SetNetworkTimeout(5000) && view.SetNetworkFilterForWorkers(true) && view.SetNetworkFilterForWorkers(false),
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
	bytes, _ := os.ReadFile(saved)
	check(string(bytes) == "downloaded bytes", "its bytes", string(bytes))
	cancelled := false
	view.OnDownload(func(r *zebkit.DownloadRequest) { cancelled = r.Cancel() == nil })
	view.Load(base + "/download")
	check(wait(func() bool { return cancelled }, 15), "a download cancelled")
	check(!view.CancelDownload("no-such-download"), "CancelDownload of none")
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
	view.OnHTTPAuth(func(r *zebkit.HTTPAuthRequest) { r.Respond("alice", "secret") })
	view.Load(base + "/private")
	check(wait(func() bool { return view.Title() == "Signed in" }, 15), "OnHTTPAuth signs in", view.Title())
	view.OnHTTPAuth(nil)

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
	check(view.SetFullscreenPolicy(zebkit.FullscreenDeny), "SetFullscreenPolicy")
	check(activated(view, "document.body.requestFullscreen().then(() => 'full', e => e.name)") != `"full"`, "fullscreen denied")
	view.SetFullscreenPolicy(zebkit.FullscreenAllow)
	var console map[string]any
	view.OnConsole(func(event map[string]any) { console = event })
	evaluate(view, "console.log('hello from the page'), 1")
	check(wait(func() bool { return console != nil }, 5) && strings.Contains(fmt.Sprint(console), "hello from the page"), "OnConsole", console)
	check(view.AddWorldScript("", "window.fromWorld = 7", true) == nil && load(view, base+"/") &&
		evaluate(view, "window.fromWorld") == "7", "AddWorldScript")

	// Popups.
	check(view.SetPopupPolicy(zebkit.PopupBlock) && view.PopupPolicy() == zebkit.PopupBlock, "SetPopupPolicy and PopupPolicy")
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
		check(opened.WindowOpenInfo()["url"] != nil, "WindowOpenInfo", opened.WindowOpenInfo())
		opened.Close()
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
	check(view.SetUserAgentOverride(map[string]any{"userAgent": "GoAgent/1"}, func(_ string, err error) { agentDone, agentErr = true, err }),
		"SetUserAgentOverride queued")
	check(wait(func() bool { return agentDone }, 10) && agentErr == nil && evaluate(view, "navigator.userAgent") == `"GoAgent/1"`,
		"the user agent overridden", agentErr)

	// Calls answered with the page's JSON as written.
	view.Grant(base, zebkit.AllowApp, "")
	view.Bind("raw", func(c *zebkit.Call) { c.ResolveJSON([]byte(`{"a":[1,2]}`)) })
	check(load(view, base+"/") && evaluate(view, "zebkit.app.raw().then(v => v.a[1])") == "2", "ResolveJSON")

	// Windows and the system's handles, without a display.
	view.SetSizeLimits(100, 100, true)
	view.SetBounds(0, 0, 320, 240)
	view.SetVisible(true)
	_, _, hasWindow := view.NativeWindow()
	check(!hasWindow == zebkit.IsHeadless() || hasWindow, "NativeWindow")
	exported := false
	if view.ExportNativeWindow(func(string, bool) { exported = true }) {
		check(wait(func() bool { return exported }, 5), "ExportNativeWindow answers")
	}
	check(view.OnAuthentication(func(map[string]any) {}) && view.OnAuthentication(nil), "OnAuthentication set and removed")
	view.OnDrop(func([]string) {})
	view.OnDrop(nil)
	check(view.ContextMenuRun(999999, "copy") != nil, "ContextMenuRun of no menu is refused")
	zebkit.AppOnOpenFiles(func([]string) {})
	zebkit.AppOnReopen(func() {})
	zebkit.AppOnQuitRequest(func() {})
	zebkit.AppOnOpenFiles(nil)
	zebkit.AppOnReopen(nil)
	zebkit.AppOnQuitRequest(nil)

	// A context of its own: cookies kept apart.
	evaluate(view, "document.cookie = 'k=main', 1")
	context := zebkit.NewContext()
	other := context.NewView(base+"/", 320, 240)
	check(wait(func() bool { return evaluate(other, "document.readyState") == `"complete"` }, 15) &&
		evaluate(other, "document.cookie") == `""`, "a context's cookies are its own")
	other.Close()
	context.Close()

	// Script outside any page.
	script := zebkit.NewScript("one")
	var scriptResult string
	script.Evaluate("zebkit.args.length + 40", func(value string, err error) { scriptResult = value })
	check(wait(func() bool { return scriptResult != "" }, 10) && scriptResult == "41", "Script.Evaluate", scriptResult)
	module := filepath.Join(directory, "module.mjs")
	os.WriteFile(module, []byte("globalThis.ran = 1;"), 0o600)
	moduleDone := false
	var moduleErr error
	script.RunModule(module, func(err error) { moduleDone, moduleErr = true, err })
	check(wait(func() bool { return moduleDone }, 10) && moduleErr == nil, "Script.RunModule", moduleErr)
	script.Destroy()

	// CDP.
	port, err := zebkit.CDPListen("", 0)
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
	zebkit.CDPStop()
	check(zebkit.CDPPort() == 0, "CDPStop")

	view.Close()
	zebkit.Shutdown()
	fmt.Printf("%d of %d checks passed\n", checks-failures, checks)
	if failures > 0 {
		os.Exit(1)
	}
}
