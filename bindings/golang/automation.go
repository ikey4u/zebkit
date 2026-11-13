package zebkit

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"unsafe"
)

// Playwright's API run inside the engine (<zebkit/automation.h>,
// docs/AUTOMATION.md), at its low level: commands as Playwright's protocol
// names them, on a view's page, without CDP or Node. Parameters are JSON
// objects (the package's rule); results are the command's JSON object. The
// typed API (Page, Locator, events, routes, expect, ...) is the automation
// package, github.com/ikey4u/zebkit/bindings/golang/automation, built on
// these.

// AutomationCommands lists the commands this engine has.
func AutomationCommands() ([]string, error) {
	onEngineThread()
	var names []string
	return names, takeJSON(C.zebkit_automation_commands(), &names, "no commands")
}

// automationView is the raw view of a command, nil for the browser's; an
// error for a closed view, or before the engine has started (when the
// engine would never answer).
func automationView(view *View) (*C.zebkit_view, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	if view == nil {
		onEngineThread()
		return nil, nil
	}
	raw, ok := view.raw()
	if !ok {
		return nil, ErrClosed
	}
	return raw, nil
}

// CallAutomation runs Playwright's command method on view's page with
// params, a JSON object (nil for {}); done gets the result, or Playwright's
// error. A nil view runs a command of the browser's (Browser.newContext).
func CallAutomation(view *View, method string, params any, done func(result json.RawMessage, err error)) {
	raw, err := automationView(view)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	cparams, err := optionalJSON(params)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	defer free(cparams)
	cmethod := cString(method)
	defer free(cmethod)
	C.zebkit_automation_call(raw, cmethod, cparams, C.zebkit_automation_func(C.zk_result), data(resultOnce(done)))
}

// ScreenshotPixels is a screenshot unencoded: Height rows of Stride bytes,
// RGBA, 8 bits each, straight alpha.
type ScreenshotPixels struct {
	Pixels                []byte
	Width, Height, Stride int
}

// CallAutomationPixels takes "Page.screenshot" (or "ElementHandle.screenshot")
// as pixels instead of a PNG; params as the command's, JSON.
func CallAutomationPixels(view *View, method string, params any, done func(*ScreenshotPixels, error)) {
	raw, ok := view.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	cparams, err := optionalJSON(params)
	if err != nil {
		if done != nil {
			done(nil, err)
		}
		return
	}
	defer free(cparams)
	h := onceHandle(func(pixels []byte, width, height, stride int, failure string, isError bool) {
		if done == nil {
			return
		}
		if isError {
			done(nil, &Error{failure})
		} else {
			done(&ScreenshotPixels{pixels, width, height, stride}, nil)
		}
	})
	cmethod := cString(method)
	defer free(cmethod)
	C.zebkit_automation_screenshot_pixels(raw, cmethod, cparams, C.zebkit_automation_pixels_func(C.zk_pixels), data(h))
}

// OnAutomationEvent calls function with the page's events as Playwright's
// protocol has them ("Frame.navigated", "BrowserContext.dialog",
// "Page.close", ...) and their parameters; the last ones can come as the
// view closes, after OnClose's function. nil stops. One function a view: it
// replaces the one of the automation package's Page for the view.
func (v *View) OnAutomationEvent(function func(event string, params json.RawMessage)) {
	v.hook("automation-event", function, function, unsafe.Pointer(C.zk_automation_event),
		func(raw *C.zebkit_view, f, d unsafe.Pointer) {
			C.zebkit_automation_on_event(raw, C.zebkit_automation_event_func(f), d)
		})
}

// ViewByPageID is the view of a page id ("p2", from BrowserContext.newPage):
// the same View each time.
func ViewByPageID(id string) (*View, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	cid := cString(id)
	defer free(cid)
	view := newView(C.zebkit_automation_page(cid))
	if view == nil {
		return nil, refused("no such page")
	}
	return view, nil
}

// AutomationPageID is the view's page id.
func (v *View) AutomationPageID() string {
	if raw, ok := v.raw(); ok {
		return takeString(C.zebkit_automation_page_id(raw))
	}
	return ""
}

// ListenAutomation serves Playwright's own protocol on a WebSocket on
// 127.0.0.1:port (0 picks one), what chromium.connect(url) connects to. done
// gets the url, which is the credential: give it only to what should drive
// the engine. Before the engine has started, done gets ErrNotStarted.
func ListenAutomation(port int, done func(url string, err error)) {
	if !started.Load() {
		if done != nil {
			done("", ErrNotStarted)
		}
		return
	}
	onEngineThread()
	h := onceHandle(func(url, failure string, isError bool) {
		if done == nil {
			return
		}
		if isError || url == "" {
			if failure == "" {
				failure = "the endpoint did not listen"
			}
			done("", &Error{failure})
		} else {
			done(url, nil)
		}
	})
	C.zebkit_automation_listen(C.int(port), C.zebkit_automation_listen_func(C.zk_listen), data(h))
}
