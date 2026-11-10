//go:build windows

package zebkit

/*
#include <zebkit/win_sandbox.h>
*/
import "C"

import "os"

// On Windows the program is also the engine's sandboxed children: Chromium's
// sandbox there needs the process that starts them and the children to be one
// executable (docs/WINDOWS.md). A child runs the engine's part here, before the
// program's own code, and exits. Link zebkit_sandbox.lib (bindings/golang/README.md).
func init() {
	var code C.int
	if C.zebkit_win_startup(&code) {
		os.Exit(int(code))
	}
}
