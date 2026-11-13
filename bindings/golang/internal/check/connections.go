package main

import (
	"fmt"
	"net"
	"net/http"
	"sync/atomic"

	zebkit "github.com/ikey4u/zebkit/bindings/golang"
)

// connections checks connection reuse (docs/CONNECTION_REUSE.md): Preconnect
// opens connections and sends nothing, a context's Fetch sends its own
// cookies, ClearHTTPCache is done. A local server counts what it accepted.
func connections() {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	var accepted, requests atomic.Int32
	server := &http.Server{
		Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			requests.Add(1)
			if r.URL.Path == "/set" {
				http.SetCookie(w, &http.Cookie{Name: "go", Value: "context", Path: "/", MaxAge: 3600})
			}
			cookie := ""
			if c, err := r.Cookie("go"); err == nil {
				cookie = c.Name + "=" + c.Value
			}
			fmt.Fprint(w, cookie)
		}),
		ConnState: func(_ net.Conn, state http.ConnState) {
			if state == http.StateNew {
				accepted.Add(1)
			}
		},
	}
	go server.Serve(listener)
	defer server.Close()
	base := fmt.Sprintf("http://%s", listener.Addr())

	check(zebkit.Preconnect("ftp://127.0.0.1/", 1) != nil && zebkit.Preconnect(base, 0) != nil && zebkit.Preconnect(base, 7) != nil,
		"Preconnect refuses what is not http(s) and a count out of 1 to 6")
	check(zebkit.Preconnect(base, 2) == nil, "Preconnect")
	context, err := zebkit.NewContext()
	if err != nil {
		check(false, "NewContext", err)
		return
	}
	check(context.Preconnect(base, 1) == nil, "Context.Preconnect")
	check(wait(func() bool { return accepted.Load() >= 3 }, 10) && requests.Load() == 0,
		"Preconnect: three connections, no request", accepted.Load(), requests.Load())

	fetch := func(in *zebkit.Context, path string) *zebkit.FetchResponse {
		var got *zebkit.FetchResponse
		var err error
		if in == nil {
			err = zebkit.Fetch("", base+path, "", nil, func(r *zebkit.FetchResponse) { got = r })
		} else {
			err = in.Fetch("", base+path, "", nil, func(r *zebkit.FetchResponse) { got = r })
		}
		if err != nil || !wait(func() bool { return got != nil }, 15) {
			return &zebkit.FetchResponse{}
		}
		return got
	}
	check(fetch(context, "/set").Status == 200, "Context.Fetch sets a cookie")
	inside, outside := fetch(context, "/cookie"), fetch(nil, "/cookie")
	check(string(inside.Body) == "go=context", "Context.Fetch sends the context's cookie", string(inside.Body))
	check(outside.Status == 200 && string(outside.Body) == "", "Fetch does not send the context's", string(outside.Body))

	cleared, contextCleared := false, false
	check(zebkit.ClearHTTPCache(func() { cleared = true }) == nil && context.ClearHTTPCache(func() { contextCleared = true }) == nil &&
		wait(func() bool { return cleared && contextCleared }, 15), "ClearHTTPCache and Context.ClearHTTPCache are done")
	context.Close()
	check(context.Preconnect(base, 1) == zebkit.ErrClosed && context.ClearHTTPCache(nil) == zebkit.ErrClosed &&
		context.Fetch("", base, "", nil, nil) == zebkit.ErrClosed, "a closed context refuses Preconnect, ClearHTTPCache and Fetch")
}
