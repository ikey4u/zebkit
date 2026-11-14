package main

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// libraryPage checks a JavaScript library in a page of its own (the backend
// "page", docs/JS_LIBRARIES.md) through the same LoadLibrary and calls: the
// fixture tests/engine/library_page_fixture measures text in a canvas, and
// JSON and parts go both ways. tests/engine/library_page.py has every case.
func libraryPage() {
	media := os.Getenv("ZEBKIT_MEDIA_DIR")
	if media == "" {
		fmt.Println("skip library page: no ZEBKIT_MEDIA_DIR")
		return
	}
	fixture := filepath.Join(media, "..", "..", "library_page_fixture")
	var functions []string
	ready := false
	lib, err := zebkit.LoadLibrary(fixture, map[string]any{"backend": "page", "width": 500, "height": 400},
		func(names []string, err error) { functions, ready = names, true })
	check(err == nil, "LoadLibrary with the backend page", err)
	if err != nil {
		return
	}
	defer lib.Close()
	check(wait(func() bool { return ready }, 30) && len(functions) > 10, "library page: ready, the exported functions", functions)
	call := func(function, args string, parts [][]byte) zebkit.LibraryResult {
		_, results, err := lib.CallChan(function, json.RawMessage(args), parts, 0)
		if err != nil {
			return zebkit.LibraryResult{Err: &zebkit.LibraryError{Code: -1, Message: err.Error()}}
		}
		var r zebkit.LibraryResult
		got := false
		wait(func() bool {
			select {
			case r = <-results:
				got = true
			default:
			}
			return got
		}, 30)
		if !got {
			r.Err = &zebkit.LibraryError{Code: -1, Message: "no result"}
		}
		return r
	}
	var measured struct{ Width float64 }
	r := call("measure", `["Hello, world", "16px sans-serif"]`, nil)
	json.Unmarshal(r.Value, &measured)
	check(r.Err == nil && measured.Width > 20, "library page: a canvas measures text", string(r.Value), r.Err)
	var info struct{ Width, Height int }
	r = call("page", "", nil)
	json.Unmarshal(r.Value, &info)
	check(r.Err == nil && info.Width == 500 && info.Height == 400, "library page: width and height", string(r.Value), r.Err)
	r = call("echo", `[{"b":{"$part":0}},"日本語 🦓"]`, [][]byte{{1, 2, 255}})
	check(r.Err == nil && string(r.Value) == `[{"b":{"$part":0}},"日本語 🦓"]` && len(r.Parts) == 1 && string(r.Parts[0]) == "\x01\x02\xff",
		"library page: JSON, non-ASCII text and a part echoed", string(r.Value), r.Err)
	r = call("thrower", "", nil)
	check(r.Err != nil && r.Err.Code == zebkit.LibraryThrown && r.Err.Name == "RangeError", "library page: THROWN", r.Err)
}
