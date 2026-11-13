package main

import (
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// libraries checks JavaScript libraries (<zebkit/library.h>, library.go):
// the fixture tests/engine/library_fixture, found beside ZEBKIT_MEDIA_DIR
// (tests/engine/pages/media). Callbacks and channels; each code; fetch()
// refused, and allowed to a local server. tests/engine/library.py has the C
// API's every case.
func libraries() {
	media := os.Getenv("ZEBKIT_MEDIA_DIR")
	if media == "" {
		fmt.Println("skip libraries: no ZEBKIT_MEDIA_DIR")
		return
	}
	here := filepath.Join(media, "..", "..")
	fixture := filepath.Join(here, "library_fixture")
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	server := &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "hello")
	})}
	go server.Serve(listener)
	defer server.Close()
	base := "http://" + listener.Addr().String()

	_, err = zebkit.LoadLibrary(filepath.Join(here, "no-such-library"), nil, nil)
	check(err != nil, "LoadLibrary: a bad path is an error", err)
	var functions []string
	readyDone := false
	lib, err := zebkit.LoadLibrary(fixture, nil, func(names []string, err error) { functions, readyDone = names, true })
	check(err == nil, "LoadLibrary", err)
	if err != nil {
		return
	}
	defer lib.Close()
	check(wait(func() bool { return readyDone }, 10) && len(functions) > 5, "library: ready, the exported functions", functions)

	// result waits for a channel's result, pumping.
	result := func(results <-chan zebkit.LibraryResult) (r zebkit.LibraryResult) {
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
		return
	}
	call := func(l *zebkit.Library, function, args string, parts [][]byte, timeout time.Duration) zebkit.LibraryResult {
		_, results, err := l.CallChan(function, json.RawMessage(args), parts, timeout)
		if err != nil {
			return zebkit.LibraryResult{Err: &zebkit.LibraryError{Code: -1, Message: err.Error()}}
		}
		return result(results)
	}
	codeOf := func(r zebkit.LibraryResult) zebkit.LibraryCode {
		if r.Err != nil {
			return r.Err.Code
		}
		return zebkit.LibraryOK
	}

	args := `[{"a":[1,null,true],"b":{"$part":0}},"日本語 🦓"]`
	r := call(lib, "echo", args, [][]byte{{1, 2, 255}}, 0)
	var same, want any
	json.Unmarshal(r.Value, &same)
	json.Unmarshal([]byte(args), &want)
	check(r.Err == nil && fmt.Sprint(same) == fmt.Sprint(want) && len(r.Parts) == 1 && string(r.Parts[0]) == "\x01\x02\xff",
		"library: JSON, non-ASCII text and a part echoed", string(r.Value), r.Err)
	r = call(lib, "bytes", `[300]`, nil, 0)
	check(r.Err == nil && len(r.Parts) == 1 && len(r.Parts[0]) == 300 && r.Parts[0][299] == 43, "library: a Uint8Array returned is a part", r.Err)
	// The callback form, from another goroutine.
	var told *zebkit.LibraryResult
	go func() {
		lib.Call("double", json.RawMessage(`[21]`), nil, 0, func(r zebkit.LibraryResult) { told = &r })
	}()
	check(wait(func() bool { return told != nil }, 10) && string(told.Value) == "42", "library: Call from another goroutine, a callback")
	r = call(lib, "thrower", "", nil, 0)
	check(codeOf(r) == zebkit.LibraryThrown && r.Err.Name == "RangeError" && r.Err.Message == "too far",
		"library: THROWN, the library's name and message", r.Err)
	for function, code := range map[string]zebkit.LibraryCode{"rejecter": zebkit.LibraryThrown,
		"missing": zebkit.LibraryNoFunction, "badResult": zebkit.LibraryBadResult} {
		check(codeOf(call(lib, function, "", nil, 0)) == code, "library: "+function+" is "+code.String())
	}
	_, first, _ := lib.CallChan("later", json.RawMessage(`[1, 200]`), nil, 0)
	queued, queuedResult, _ := lib.CallChan("double", json.RawMessage(`[1]`), nil, 0)
	lib.Cancel(queued)
	check(codeOf(result(queuedResult)) == zebkit.LibraryCancelled && codeOf(result(first)) == zebkit.LibraryOK,
		"library: Cancel of a waiting call")
	_, slow, _ := lib.CallChan("later", json.RawMessage(`[1, 2000]`), nil, 100*time.Millisecond)
	_, next, _ := lib.CallChan("double", json.RawMessage(`[2]`), nil, 0)
	check(codeOf(result(slow)) == zebkit.LibraryTimeout && string(result(next).Value) == "4",
		"library: a waiting call past its deadline is TIMEOUT, the next runs")
	check(codeOf(call(lib, "get", `["`+base+`/"]`, nil, 0)) == zebkit.LibraryThrown, "library: fetch() refused without a grant")
	net, _ := zebkit.LoadLibrary(fixture, map[string]any{"network": []string{base}}, nil)
	r = call(net, "get", `["`+base+`/"]`, nil, 0)
	check(r.Err == nil && string(r.Value) == `{"status":200,"body":"hello"}`, "library: fetch() to a granted origin", string(r.Value), r.Err)
	net.Close()
	small, _ := zebkit.LoadLibrary(fixture, `{"queue": 1}`, nil)
	_, running, _ := small.CallChan("later", json.RawMessage(`[0, 300]`), nil, 0)
	wait(func() bool { return false }, 0.3)
	_, waiting, _ := small.CallChan("double", json.RawMessage(`[1]`), nil, 0)
	_, full, _ := small.CallChan("double", json.RawMessage(`[1]`), nil, 0)
	check(codeOf(result(full)) == zebkit.LibraryQueueFull && codeOf(result(running)) == zebkit.LibraryOK &&
		codeOf(result(waiting)) == zebkit.LibraryOK, "library: QUEUE_FULL")
	small.Close()
	doomed, _ := zebkit.LoadLibrary(fixture, nil, nil)
	_, spinning, _ := doomed.CallChan("spin", nil, nil, 200*time.Millisecond)
	_, after, _ := doomed.CallChan("double", json.RawMessage(`[1]`), nil, 0)
	check(codeOf(result(spinning)) == zebkit.LibraryTimeout && codeOf(result(after)) == zebkit.LibraryGone,
		"library: a running script past its deadline is TIMEOUT, then GONE")
	doomed.Close()
	hungry, _ := zebkit.LoadLibrary(fixture, `{"heapLimitMB": 32}`, nil)
	check(codeOf(call(hungry, "grow", "", nil, -1)) == zebkit.LibraryOutOfMemory, "library: heap exhaustion is OUT_OF_MEMORY")
	hungry.Close()
	var loadErr error
	loadDone := false
	forbidden, _ := zebkit.LoadLibrary(filepath.Join(here, "library_forbidden"), nil, func(_ []string, err error) { loadErr, loadDone = err, true })
	check(wait(func() bool { return loadDone }, 10) && loadErr != nil && loadErr.(*zebkit.LibraryError).Code == zebkit.LibraryLoadFailed,
		"library: an import outside the folder fails the load", loadErr)
	forbidden.Close()
	closing, _ := zebkit.LoadLibrary(fixture, nil, nil)
	_, pendingA, _ := closing.CallChan("later", json.RawMessage(`[1, 1000]`), nil, 0)
	_, pendingB, _ := closing.CallChan("double", json.RawMessage(`[1]`), nil, 0)
	closing.Close()
	_, err = closing.Call("double", nil, nil, 0, nil)
	check(codeOf(result(pendingA)) == zebkit.LibraryGone && codeOf(result(pendingB)) == zebkit.LibraryGone &&
		err == zebkit.ErrLibraryClosed, "library: Close with calls pending: GONE; a call after it is refused")
}
