// zebkit-cc is the C compiler for building a Go program with zebkit on
// Windows: clang for the MSVC target, as the engine is built, linking with
// lld-link and the static C runtime that zebkit_sandbox.lib needs. Go's cgo
// speaks to a MinGW gcc; zebkit-cc drops the flags only MinGW knows and passes
// the rest on.
//
//	go install github.com/ikey4u/zebkit/bindings/golang/cmd/zebkit-cc@main
//	set CC=zebkit-cc
//
// Environment:
//
//	ZEBKIT_CLANG      the clang to run (default: clang on PATH)
//	ZEBKIT_CC_FLAGS   more flags for it, split on spaces, such as
//	                  "-Xmicrosoft-windows-sys-root /path/to/vs_files" when
//	                  cross-building from Linux or macOS with the Windows SDK
package main

import (
	"os"
	"os/exec"
	"strings"
)

// Flags of Go's for a MinGW gcc that clang for MSVC refuses or lld-link does
// not take.
func mingwOnly(arg string) bool {
	switch arg {
	case "-mthreads", "-mconsole", "-mwindows", "-lmingwex", "-lmingw32", "-Wl,--start-group", "-Wl,--end-group":
		return true
	}
	return strings.HasPrefix(arg, "-Wl,-T,")
}

func main() {
	clang := os.Getenv("ZEBKIT_CLANG")
	if clang == "" {
		clang = "clang"
	}
	args := []string{"--target=x86_64-pc-windows-msvc", "-fms-runtime-lib=static", "-fuse-ld=lld"}
	args = append(args, strings.Fields(os.Getenv("ZEBKIT_CC_FLAGS"))...)
	for _, arg := range os.Args[1:] {
		if !mingwOnly(arg) {
			args = append(args, arg)
		}
	}
	cmd := exec.Command(clang, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	if err := cmd.Run(); err != nil {
		if exit, ok := err.(*exec.ExitError); ok {
			os.Exit(exit.ExitCode())
		}
		os.Stderr.WriteString("zebkit-cc: " + err.Error() + "\n")
		os.Exit(1)
	}
}
