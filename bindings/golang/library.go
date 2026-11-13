package zebkit

/*
#include <stdint.h>
#include <stdlib.h>
#include <zebkit/library.h>

void zk_library_told(zebkit_library *library, zebkit_library_code code, const char *json, const zebkit_part *parts,
                     size_t count, void *data);
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"runtime"
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
}

func libraryResult(code C.int, text *C.char, parts *C.zebkit_part, count C.size_t) LibraryResult {
	value := goString(text)
	if LibraryCode(code) != LibraryOK {
		var info struct{ Message, Name, Stack string }
		_ = json.Unmarshal([]byte(value), &info)
		return LibraryResult{Err: &LibraryError{Code: LibraryCode(code), Message: info.Message, Name: info.Name,
			Stack: info.Stack}}
	}
	result := LibraryResult{Value: json.RawMessage(value)}
	if count > 0 {
		list := unsafe.Slice(parts, int(count))
		result.Parts = make([][]byte, len(list))
		for i, part := range list {
			result.Parts[i] = goBytes(unsafe.Pointer(part.data), part.size)
			if result.Parts[i] == nil {
				result.Parts[i] = []byte{}
			}
		}
	}
	return result
}

//export goZkLibraryTold
func goZkLibraryTold(code C.int, text *C.char, parts *C.zebkit_part, count C.size_t, h C.uintptr_t) {
	if f, ok := lookup[func(LibraryResult)](h); ok {
		f(libraryResult(code, text, parts, count))
	}
}

// LoadLibrary loads the library at path (a folder with index.mjs, or one
// .mjs file) and returns at once; calls wait for the load. options is
// zebkit_library_load()'s JSON ("timeoutMs", "queue", "heapLimitMB",
// "maxBytes", "network"): nil, a string, []byte, json.RawMessage, or a value
// to encode. ready, if not nil, is told once, on the engine's thread: the
// exported functions' names, or why the load failed (a *LibraryError).
func LoadLibrary(path string, options any, ready func(functions []string, err error)) (*Library, error) {
	onEngineThread()
	if !started.Load() {
		return nil, ErrNotStarted
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
	return &Library{raw: raw}, nil
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
	list := make([]C.zebkit_part, len(parts))
	for i, part := range parts {
		if len(part) > 0 {
			pinner.Pin(unsafe.SliceData(part))
			list[i].data = (*C.uint8_t)(unsafe.Pointer(unsafe.SliceData(part)))
		}
		list[i].size = C.size_t(len(part))
	}
	var first *C.zebkit_part
	if len(list) > 0 {
		pinner.Pin(&list[0])
		first = &list[0]
	}
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
	id := C.zebkit_library_call(l.raw, cfunction, cargs, first, C.size_t(len(list)), C.int64_t(ms),
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
	}
}
