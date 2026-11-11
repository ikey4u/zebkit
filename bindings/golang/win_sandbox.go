//go:build windows

package zebkit

/*
#include <zebkit/win_sandbox.h>
*/
import "C"

import "os"

// On Windows the program is also the engine's sandboxed children: Chromium's
// sandbox there needs the process that starts them and the children to be one
// executable (docs/WINDOWS.md). A child runs the engine's part here and exits,
// before main.main, but not first in the process as <zebkit/win_sandbox.h>
// asks: the Go runtime has started, and the init functions of the packages
// initialized before this one (those zebkit does not import may come first)
// have run, in every child too. Keep those free of side effects (files,
// network, threads that matter), or put them in packages that import zebkit.
// Link zebkit_sandbox.lib (bindings/golang/README.md).
func init() {
	var code C.int
	if C.zebkit_win_startup(&code) {
		os.Exit(int(code))
	}
}
