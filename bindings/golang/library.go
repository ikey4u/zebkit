package zebkit

/*
#include <stdint.h>
#include <stdlib.h>
#include <zebkit/library.h>

void zk_library_told(zebkit_library *library, zebkit_library_code code, const char *json, const zebkit_part *parts,
                     size_t count, void *data);
void zk_library_host(zebkit_library *library, uint64_t request, const char *name, const char *args,
                     const zebkit_part *parts, size_t count, void *data);
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"runtime"
	"sort"
	"sync"
	"time"
	"unsafe"
)

// JavaScript libraries as normal libraries (<zebkit/library.h>,
// docs/JS_LIBRARIES.md): a bundled ES module loaded once, in a V8 isolate
// on a thread of its own, and called from Go.
//
//	md, err := zebkit.LoadLibrary("libs/markdown", nil, nil)
//	md.Call("render", json.RawMessage(`["# Title"]`), nil, 0, func(r zebkit.LibraryResult) {
//		fmt.Println(string(r.Value), r.Err)
//	})
//
// Values are JSON and parts: {"$part": n} in the JSON stands for Parts[n],
// a Uint8Array in the library. LoadLibrary and Close are for the engine's
// thread; Call, CallChan and Cancel for any goroutine. Results come on the
// engine's thread.
//
// The library calls the application's functions, given to
// LoadLibraryWithHost, as `await host.call(name, ...args)` (import host from
// 'zebkit:host'):
//
//	lib, err := zebkit.LoadLibraryWithHost("libs/report", nil, map[string]zebkit.LibraryHostFunc{
//		"progress": func(args json.RawMessage, parts [][]byte) (json.RawMessage, [][]byte, error) {
//			fmt.Println("progress", string(args))
//			return nil, nil, nil
//		},
//	}, nil)

// LibraryCode says how a library's call or load ended.
type LibraryCode int

const (
	LibraryOK           LibraryCode = 0
	LibraryThrown       LibraryCode = 1  // the function threw, or its promise was rejected
	LibraryLoadFailed   LibraryCode = 2  // the library did not load
	LibraryNoFunction   LibraryCode = 3  // no exported function of that name
	LibraryBadArguments LibraryCode = 4  // not a JSON array, a bad {"$part": n}, too deep or large
	LibraryBadResult    LibraryCode = 5  // a value JSON and parts cannot carry, or too large
	LibraryTimeout      LibraryCode = 6  // past the call's deadline
	LibraryCancelled    LibraryCode = 7  // Cancel
	LibraryQueueFull    LibraryCode = 8  // as many calls wait as the load's "queue"
	LibraryOutOfMemory  LibraryCode = 9  // the library's heap passed "heapLimitMB"; it has ended
	LibraryGone         LibraryCode = 10 // the library ended or was closed first
)

var libraryCodeNames = [...]string{"OK", "THROWN", "LOAD_FAILED", "NO_FUNCTION", "BAD_ARGUMENTS", "BAD_RESULT",
	"TIMEOUT", "CANCELLED", "QUEUE_FULL", "OUT_OF_MEMORY", "GONE"}

func (c LibraryCode) String() string {
	if c >= 0 && int(c) < len(libraryCodeNames) {
		return libraryCodeNames[c]
	}
	return fmt.Sprintf("LibraryCode(%d)", int(c))
}

// LibraryError is why a call or the load failed: the code, and what the
// library threw (its Name, Message and Stack) or what the engine says.
type LibraryError struct {
	Code    LibraryCode
	Message string
	Name    string
	Stack   string
}

func (e *LibraryError) Error() string {
	if e.Name != "" {
		return fmt.Sprintf("zebkit: library %s: %s: %s", e.Code, e.Name, e.Message)
	}
	return fmt.Sprintf("zebkit: library %s: %s", e.Code, e.Message)
}

// LibraryResult is a call's outcome: Value (JSON) and Parts, or Err.
type LibraryResult struct {
	Value json.RawMessage
	Parts [][]byte
	Err   *LibraryError
}

// Library is a loaded JavaScript library.
type Library struct {
	mu     sync.RWMutex
	raw    *C.zebkit_library
	closed bool
	host   C.uintptr_t // its host functions' handle, or 0
}

// LibraryHostFunc is an application function a library calls as
// host.call(name, ...args): args is the JSON array of the arguments, parts
// their bytes ({"$part": n}). It runs on a goroutine of its own; what it
// returns answers the library once: the value (JSON; nil is null) and its
// parts, or an error, which rejects host.call() with an Error of its message.
type LibraryHostFunc func(args json.RawMessage, parts [][]byte) (json.RawMessage, [][]byte, error)

type libraryHost struct {
	library   *Library
	functions map[string]LibraryHostFunc
}

func copyParts(parts *C.zebkit_part, count C.size_t) [][]byte {
	if count == 0 {
		return nil
	}
	list := unsafe.Slice(parts, int(count))
	copied := make([][]byte, len(list))
	for i, part := range list {
		copied[i] = goBytes(unsafe.Pointer(part.data), part.size)
		if copied[i] == nil {
			copied[i] = []byte{}
		}
	}
	return copied
}

func libraryResult(code C.int, text *C.char, parts *C.zebkit_part, count C.size_t) LibraryResult {
	value := goString(text)
	if LibraryCode(code) != LibraryOK {
		var info struct{ Message, Name, Stack string }
		_ = json.Unmarshal([]byte(value), &info)
		return LibraryResult{Err: &LibraryError{Code: LibraryCode(code), Message: info.Message, Name: info.Name,
			Stack: info.Stack}}
	}
	return LibraryResult{Value: json.RawMessage(value), Parts: copyParts(parts, count)}
}

//export goZkLibraryTold
func goZkLibraryTold(code C.int, text *C.char, parts *C.zebkit_part, count C.size_t, h C.uintptr_t) {
	if f, ok := lookup[func(LibraryResult)](h); ok {
		f(libraryResult(code, text, parts, count))
	}
}

//export goZkLibraryHost
func goZkLibraryHost(request C.uint64_t, name *C.char, args *C.char, parts *C.zebkit_part, count C.size_t,
	h C.uintptr_t) {
	host, ok := lookup[*libraryHost](h)
	if !ok {
		return
	}
	function, known := host.functions[goString(name)]
	argsCopy, partsCopy := json.RawMessage(goString(args)), copyParts(parts, count)
	go func() {
		if !known {
			host.library.answer(uint64(request), nil, nil, fmt.Errorf("the application has no function %q", goString(name)))
			return
		}
		value, valueParts, err := function(argsCopy, partsCopy)
		host.library.answer(uint64(request), value, valueParts, err)
	}()
}

// optionalJSONText is options as JSON text, by jsonArg's rule.
func optionalJSONText(value any) ([]byte, error) {
	switch v := value.(type) {
	case string:
		return []byte(v), nil
	case []byte:
		return v, nil
	case json.RawMessage:
		return v, nil
	}
	return json.Marshal(value)
}

// answer answers the library's host.call() request, once; nothing once closed.
func (l *Library) answer(request uint64, value json.RawMessage, parts [][]byte, failure error) {
	l.mu.RLock()
	defer l.mu.RUnlock()
	if l.closed {
		return
	}
	code := C.zebkit_library_code(LibraryOK)
	if failure != nil {
		code = C.zebkit_library_code(LibraryThrown)
		value, _ = json.Marshal(map[string]string{"message": failure.Error(), "name": "Error"})
		parts = nil
	}
	var text *C.char
	if len(value) > 0 {
		text = C.CString(string(value))
		defer free(text)
	}
	var pinner runtime.Pinner
	defer pinner.Unpin()
	first, n := pinParts(&pinner, parts)
	C.zebkit_library_answer(l.raw, C.uint64_t(request), code, text, first, n)
}

// pinParts is parts as the C API takes them, pinned while it reads them.
func pinParts(pinner *runtime.Pinner, parts [][]byte) (*C.zebkit_part, C.size_t) {
	list := make([]C.zebkit_part, len(parts))
	for i, part := range parts {
		if len(part) > 0 {
			pinner.Pin(unsafe.SliceData(part))
			list[i].data = (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(part)))
		}
		list[i].size = C.size_t(len(part))
	}
	if len(list) == 0 {
		return nil, 0
	}
	pinner.Pin(&list[0])
	return &list[0], C.size_t(len(list))
}

// LoadLibrary loads the library at path (a folder with index.mjs, or one
// .mjs file) and returns at once; calls wait for the load. options is
// zebkit_library_load()'s JSON ("timeoutMs", "queue", "heapLimitMB",
// "maxBytes", "network", "instances", "codeCache"): nil, a string, []byte,
// json.RawMessage, or a value to encode. ready, if not nil, is told once, on
// the engine's thread: the exported functions' names, or why the load
// failed (a *LibraryError).
func LoadLibrary(path string, options any, ready func(functions []string, err error)) (*Library, error) {
	return LoadLibraryWithHost(path, options, nil, ready)
}

// LoadLibraryWithHost is LoadLibrary with the application's functions the
// library calls as host.call(name, ...args) (options' "host" is their names).
// One not answered in the library's "timeoutMs" is a TimeoutError there.
func LoadLibraryWithHost(path string, options any, host map[string]LibraryHostFunc,
	ready func(functions []string, err error)) (*Library, error) {
	onEngineThread()
	if !started.Load() {
		return nil, ErrNotStarted
	}
	if len(host) > 0 {
		merged := map[string]any{}
		if options != nil {
			text, err := optionalJSONText(options)
			if err != nil {
				return nil, err
			}
			if err := json.Unmarshal(text, &merged); err != nil {
				return nil, &Error{"the options are not a JSON object"}
			}
		}
		names := make([]string, 0, len(host))
		for name := range host {
			names = append(names, name)
		}
		sort.Strings(names)
		merged["host"] = names
		options = merged
	}
	coptions, err := optionalJSON(options)
	if err != nil {
		return nil, err
	}
	defer free(coptions)
	cpath := cString(path)
	defer free(cpath)
	h := onceHandle(func(result LibraryResult) {
		if ready == nil {
			return
		}
		if result.Err != nil {
			ready(nil, result.Err)
			return
		}
		var loaded struct{ Functions []string }
		if err := json.Unmarshal(result.Value, &loaded); err != nil {
			ready(nil, err)
			return
		}
		ready(loaded.Functions, nil)
	})
	var message *C.char
	raw := C.zebkit_library_load(cpath, coptions, C.zebkit_library_func(C.zk_library_told), data(h), &message)
	if raw == nil {
		release(h)
		return nil, takeError(message, "the library was not loaded")
	}
	library := &Library{raw: raw}
	if len(host) > 0 {
		functions := make(map[string]LibraryHostFunc, len(host))
		for name, function := range host {
			functions[name] = function
		}
		library.host = handle(&libraryHost{library: library, functions: functions})
		C.zebkit_library_set_host(raw, C.zebkit_library_host_func(C.zk_library_host), data(library.host))
	}
	return library, nil
}

// ErrLibraryClosed is what a call of a closed library returns.
var ErrLibraryClosed = &Error{"the library is closed"}

// Call calls the library's exported function with args (a JSON array; nil
// is []) and parts, which are copied before it returns. timeout: 0 the
// library's ("timeoutMs"), < 0 none. done is told exactly once, on the
// engine's thread. The call's number is for Cancel. Any goroutine.
func (l *Library) Call(function string, args json.RawMessage, parts [][]byte, timeout time.Duration,
	done func(LibraryResult)) (uint64, error) {
	l.mu.RLock()
	defer l.mu.RUnlock()
	if l.closed {
		return 0, ErrLibraryClosed
	}
	cfunction := cString(function)
	defer free(cfunction)
	var cargs *C.char
	if len(args) > 0 {
		cargs = C.CString(string(args))
		defer free(cargs)
	}
	var pinner runtime.Pinner
	defer pinner.Unpin()
	first, n := pinParts(&pinner, parts)
	ms := int64(0)
	if timeout < 0 {
		ms = -1
	} else if timeout > 0 {
		ms = max(1, timeout.Milliseconds())
	}
	h := onceHandle(func(result LibraryResult) {
		if done != nil {
			done(result)
		}
	})
	id := C.zebkit_library_call(l.raw, cfunction, cargs, first, n, C.int64_t(ms),
		C.zebkit_library_func(C.zk_library_told), data(h))
	if id == 0 {
		release(h)
		return 0, ErrLibraryClosed
	}
	return uint64(id), nil
}

// CallChan is Call with a channel the result is sent on, once.
func (l *Library) CallChan(function string, args json.RawMessage, parts [][]byte,
	timeout time.Duration) (uint64, <-chan LibraryResult, error) {
	results := make(chan LibraryResult, 1)
	id, err := l.Call(function, args, parts, timeout, func(result LibraryResult) { results <- result })
	if err != nil {
		return 0, nil, err
	}
	return id, results, nil
}

// Cancel tells the call LibraryCancelled, unless it was told already; once
// the library is closed it does nothing. Any goroutine.
func (l *Library) Cancel(call uint64) {
	l.mu.RLock()
	defer l.mu.RUnlock()
	if !l.closed {
		C.zebkit_library_cancel(l.raw, C.uint64_t(call))
	}
}

// Close ends the library: calls not told yet are told LibraryGone. The
// engine's thread.
func (l *Library) Close() {
	onEngineThread()
	l.mu.Lock()
	defer l.mu.Unlock()
	if !l.closed {
		l.closed = true
		C.zebkit_library_close(l.raw)
		release(l.host)
	}
}
