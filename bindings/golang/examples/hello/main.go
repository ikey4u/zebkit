// A window with a page that calls a Go function.
//
//	go run ./examples/hello
package main

import (
	"fmt"
	"os"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

func main() {
	os.Exit(zebkit.Main(func() {
		view := zebkit.NewView("data:text/html,<title>Hello</title><h1 id=out>...</h1>"+
			"<script>zebkit.app.greet('Go').then(r => out.textContent = r)</script>", 800, 600)
		view.Grant("*", zebkit.AllowApp, "")
		view.Bind("greet", func(call *zebkit.Call) {
			call.Resolve(fmt.Sprintf("\"hello from %s\"", call.Args))
		})
		view.OnClose(zebkit.Quit)
	}, os.Args[1:]...))
}
