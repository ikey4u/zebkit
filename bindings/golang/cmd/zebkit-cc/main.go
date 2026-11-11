// zebkit-cc is the C compiler for cross-building a Go program with zebkit
// for Windows, from Linux or macOS: clang for the MSVC target, as the engine
// is built, linking with lld-link and the static C runtime that
// zebkit_sandbox.lib needs. Go's cgo speaks to a MinGW gcc; zebkit-cc drops
// the flags only MinGW knows and passes the rest on.
//
//	go install github.com/ikey4u/zebkit/bindings/golang/cmd/zebkit-cc@<version>
//	GOOS=windows GOARCH=amd64 CGO_ENABLED=1 CC=zebkit-cc ZEBKIT_DIR=... go build ...
//
// When it links, it adds the 8 MiB main stack the engine's renderer needs
// (chrome.exe's), unless the arguments set one, and, with ZEBKIT_DIR, the
// engine's libraries: CGO_LDFLAGS then needs nothing of zebkit's.
//
// Environment:
//
//	ZEBKIT_CLANG      the clang to run (default: clang on PATH)
//	ZEBKIT_SYSROOT    the Windows SDK laid out for clang (Chromium's
//	                  --winsysroot, scripts/windows-toolchain.sh); one path,
//	                  spaces and all
//	ZEBKIT_DIR        the engine's package: zebkit.dll.lib and
//	                  zebkit_sandbox.lib are linked from there
//	ZEBKIT_CC_FLAGS   more flags for clang, split on spaces
//	GOARCH            amd64 (the default) or arm64, as go build sets it
package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// The MinGW libraries Go's linker asks for, which the MSVC target has in its
// own C runtime.
var mingwLibraries = map[string]bool{
	"mingwex": true, "mingw32": true, "mingwthrd": true, "gcc": true, "gcc_eh": true, "gcc_s": true,
	"moldname": true, "msvcrt": true, "pthread": true,
}

// mingwOnly says whether arg is one of Go's flags for a MinGW gcc that clang
// for MSVC refuses or lld-link does not take: every GNU ld option (-Wl,--...,
// -Wl,-T,..., -Wl,-B...), the MinGW libraries and gcc's own switches.
// lld-link's own options (-Wl,/...) pass.
func mingwOnly(arg string) bool {
	switch arg {
	case "-mthreads", "-mconsole", "-mwindows", "-municode", "-static", "-static-libgcc", "-shared-libgcc", "-rdynamic":
		return true
	}
	if strings.HasPrefix(arg, "-Wl,") {
		option := strings.TrimPrefix(arg, "-Wl,")
		return strings.HasPrefix(option, "-")
	}
	if library, ok := strings.CutPrefix(arg, "-l"); ok {
		return mingwLibraries[library]
	}
	return false
}

// target is clang's target for GOARCH.
func target() string {
	switch os.Getenv("GOARCH") {
	case "arm64":
		return "aarch64-pc-windows-msvc"
	case "386":
		return "i686-pc-windows-msvc"
	}
	return "x86_64-pc-windows-msvc"
}

// linking says whether the arguments link (no -c, -E or -S).
func linking(args []string) bool {
	for _, arg := range args {
		if arg == "-c" || arg == "-E" || arg == "-S" || arg == "-M" || arg == "-MM" {
			return false
		}
	}
	return true
}

func main() {
	clang := os.Getenv("ZEBKIT_CLANG")
	if clang == "" {
		clang = "clang"
	}
	args := []string{"--target=" + target(), "-fms-runtime-lib=static", "-fuse-ld=lld"}
	if sysroot := os.Getenv("ZEBKIT_SYSROOT"); sysroot != "" {
		args = append(args, "-Xmicrosoft-windows-sys-root", sysroot)
	}
	args = append(args, strings.Fields(os.Getenv("ZEBKIT_CC_FLAGS"))...)
	stack := false
	for _, arg := range os.Args[1:] {
		if strings.HasPrefix(strings.ToUpper(arg), "-WL,/STACK:") {
			stack = true
		}
		if !mingwOnly(arg) {
			args = append(args, arg)
		}
	}
	if linking(os.Args[1:]) {
		if !stack {
			args = append(args, "-Wl,/STACK:0x800000")
		}
		if dir := os.Getenv("ZEBKIT_DIR"); dir != "" {
			args = append(args, filepath.Join(dir, "zebkit.dll.lib"), filepath.Join(dir, "zebkit_sandbox.lib"))
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
