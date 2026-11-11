package zebkit

/*
#include "bridge.h"
*/
import "C"

import "encoding/json"

// Script is JavaScript outside any page, on the engine's V8, with the
// zebkit object the way zebkit-js has it (docs/API.md). Close ends it: its
// timers stop; its views stay.
type Script struct {
	script *C.zebkit_script
	idle   slot
}

// NewScript makes a script that sees args as zebkit.args.
func NewScript(args ...string) (*Script, error) {
	if !started.Load() {
		return nil, ErrNotStarted
	}
	onEngineThread()
	argc, argv, done := cStrings(args)
	defer done()
	script := C.zebkit_script_new(argc, argv)
	if script == nil {
		return nil, refused("no script")
	}
	return &Script{script: script}, nil
}

func (s *Script) raw() (*C.zebkit_script, bool) {
	if s == nil || s.script == nil || !started.Load() {
		return nil, false
	}
	onEngineThread()
	return s.script, true
}

// RunModule runs the ES module at path, with the modules it imports relative
// to it; done gets nil once its evaluation ends, or what it threw.
func (s *Script) RunModule(path string, done func(err error)) {
	raw, ok := s.raw()
	if !ok {
		if done != nil {
			done(ErrClosed)
		}
		return
	}
	h := resultOnce(func(_ json.RawMessage, err error) {
		if done != nil {
			done(err)
		}
	})
	cpath := cString(path)
	defer free(cpath)
	C.zebkit_script_run_module(raw, cpath, C.zebkit_result_func(C.zk_result), data(h))
}

// Evaluate runs source as a classic script; done gets its value as JSON, or
// what it threw.
func (s *Script) Evaluate(source string, done func(result json.RawMessage, err error)) {
	raw, ok := s.raw()
	if !ok {
		if done != nil {
			done(nil, ErrClosed)
		}
		return
	}
	csource := cString(source)
	defer free(csource)
	C.zebkit_script_evaluate(raw, csource, C.zebkit_result_func(C.zk_result), data(resultOnce(done)))
}

// OnIdle calls function once nothing of the script is pending: no timer, no
// call in progress, no view open, and its module evaluated. nil stops.
func (s *Script) OnIdle(function func()) {
	raw, ok := s.raw()
	if !ok {
		return
	}
	if function == nil {
		C.zebkit_script_on_idle(raw, nil, nil)
		s.idle.clear()
		return
	}
	h := s.idle.set(function)
	C.zebkit_script_on_idle(raw, C.zebkit_task_func(C.zk_task), data(h))
}

// Close ends the script.
func (s *Script) Close() {
	raw, ok := s.raw()
	if !ok {
		return
	}
	C.zebkit_script_destroy(raw)
	s.script = nil
	s.idle.clear()
}
