package zebkit

/*
#include "bridge.h"
*/
import "C"

import "unsafe"

// OnResponsiveness calls function with false when the page stops answering
// input (Chromium's hang monitor, as Chrome's "page unresponsive" dialog,
// which zebkit does not show, and Electron's unresponsive event), and with
// true once it answers again; only the changes are told, asynchronously. It
// is told for a shown view with no debugger attached (a CDP client, the
// automation API or a Playwright endpoint holds it), after input or a
// navigation has waited on the page (15 seconds by default;
// <zebkit/zebkit.h> has the switches). A page whose process goes is told by
// OnProcessGone instead. nil stops.
func (v *View) OnResponsiveness(function func(responsive bool)) {
	var value func(string)
	if function != nil {
		value = jsonFunc(func(event struct {
			Responsive bool `json:"responsive"`
		}) {
			function(event.Responsive)
		})
	}
	v.hook("responsiveness", function, value, unsafe.Pointer(C.zk_view_text), func(raw *C.zebkit_view, f, d unsafe.Pointer) {
		C.zebkit_view_on_responsiveness(raw, C.zebkit_process_func(f), d)
	})
}
