//go:build windows

package zebkit

/*
#include <stdlib.h>
#include <zebkit/win_sandbox.h>

// On Windows the program is also the engine's sandboxed children: Chromium's
// sandbox there needs the process that starts them and the children to be one
// executable (docs/WINDOWS.md). <zebkit/win_sandbox.h> asks for
// zebkit_win_startup() first in main(); a Go program's first is here, a C
// initializer (.CRT$XCU), which the C runtime's startup runs before it calls
// Go's main, and so before the Go runtime starts. A child runs the engine's
// part and exits in it, as a C program's child returns from main; the parent
// goes on into Go. Linked from zebkit_sandbox.lib (bindings/golang/README.md),
// whose own initializers are the C++ library's, which run before this.
__attribute__((constructor)) static void zk_win_startup(void) {
	int code = 0;
	if (zebkit_win_startup(&code))
		exit(code);
}
*/
import "C"

// Why not Go's init(), as before 0.5.0: that runs once the Go runtime has
// started, in every child too. The runtime's threads would run in each
// renderer and GPU process, the init functions of packages initialized before
// this one with them, and its vectored exception handlers would be there: the
// last of them ends the process for an exception it did not raise that another
// handler has already resolved, such as V8's WebAssembly trap handler's, which
// a renderer resolves on an out-of-bounds access of a WebAssembly memory. A C
// initializer runs before all of that, so a child is what it is in a C
// program; only C initializers of other cgo code linked in (C++ static
// constructors) may run before it.
