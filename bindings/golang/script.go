package zebkit

/*
#include <stdlib.h>
#include "bridge_a.h"
*/
import "C"

import (
	"runtime/cgo"
	"unsafe"
)

// Script is JavaScript outside any page, on the engine's V8, with the
// zebkit object the way zebkit-js has it (docs/API.md). Destroy ends it: its
// timers stop; its views stay.
type Script struct {
	script *C.zebkit_script
	idle   slot
}

// NewScript makes a script that sees args as zebkit.args.
func NewScript(args ...string) *Script {
	list := make([]*C.char, len(args)+1)
	for i, arg := range args {
		list[i] = cString(arg)
	}
	defer func() {
		for _, arg := range list[:len(args)] {
			free(arg)
		}
	}()
	array := C.malloc(C.size_t(len(list)) * C.size_t(unsafe.Sizeof(uintptr(0))))
	defer C.free(array)
	copy(unsafe.Slice((**C.char)(array), len(list)), list)
	return &Script{script: C.zebkit_script_new(C.int(len(args)), (**C.char)(array))}
}

func resultHandle(done func(string, error)) C.uintptr_t {
	var h C.uintptr_t
	h = handle(func(result, failure string, failed bool) {
		cgo.Handle(h).Delete()
		if done == nil {
			return
		}
		if failed {
			done("", &Error{failure})
		} else {
			done(result, nil)
		}
	})
	return h
}

// RunModule runs the ES module at path, with the modules it imports relative
// to it; done gets nil once its evaluation ends, or what it threw.
func (s *Script) RunModule(path string, done func(err error)) {
	h := resultHandle(func(_ string, err error) {
		if done != nil {
			done(err)
		}
	})
	cpath := cString(path)
	defer free(cpath)
	C.zebkit_script_run_module(s.script, cpath, C.zebkit_result_func(C.zk_result), dataA(h))
}

// Evaluate runs source as a classic script; done gets its value as JSON, or
// what it threw.
func (s *Script) Evaluate(source string, done func(result string, err error)) {
	h := resultHandle(done)
	csource := cString(source)
	defer free(csource)
	C.zebkit_script_evaluate(s.script, csource, C.zebkit_result_func(C.zk_result), dataA(h))
}

// OnIdle calls function once nothing of the script is pending: no timer, no
// call in progress, no view open, and its module evaluated.
func (s *Script) OnIdle(function func()) {
	if function == nil {
		C.zebkit_script_on_idle(s.script, nil, nil)
		s.idle.clear()
		return
	}
	var h C.uintptr_t
	h = s.idle.set(func() {
		function()
	})
	C.zebkit_script_on_idle(s.script, C.zebkit_task_func(C.zk_task), dataA(h))
}

// Destroy ends the script.
func (s *Script) Destroy() {
	if s.script == nil {
		return
	}
	C.zebkit_script_destroy(s.script)
	s.script = nil
	s.idle.clear()
}
