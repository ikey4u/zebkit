// A window with a page that calls a Go function.
//
//	go run ./examples/hello
package main

import (
	"os"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

func main() {
	os.Exit(zebkit.Main(func() {
		view, err := zebkit.NewView("data:text/html,<title>Hello</title><h1 id=out>...</h1>"+
			"<script>zebkit.app.greet('Go').then(r => out.textContent = r)</script>", 800, 600)
		if err != nil {
			zebkit.Quit()
			return
		}
		view.Grant("*", zebkit.AllowApp, "")
		view.Bind("greet", func(call *zebkit.Call) {
			var name string
			if err := call.ArgsInto(&name); err != nil {
				call.Reject(err.Error())
				return
			}
			call.ResolveValue("hello from " + name)
		})
		view.OnClose(zebkit.Quit)
	}, os.Args[1:]...))
}
