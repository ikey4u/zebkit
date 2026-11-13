package zebkit

/*
#include "bridge.h"
*/
import "C"

// Connection reuse (docs/CONNECTION_REUSE.md): the HTTP cache, connections
// opened ahead, and requests in a context's network.

// ClearHTTPCache empties the engine profile's HTTP cache (on disk under the
// profile unless the startup setting httpCache says otherwise) and forgets
// what the network learnt of servers (HTTP/2, alternative services, HSTS),
// so that nothing of the sites visited stays there; cookies and storage are
// not touched. done, when not nil, runs on the engine's thread once that is
// done. Context.ClearHTTPCache for a context's.
func ClearHTTPCache(done func()) error {
	if !started.Load() {
		return ErrNotStarted
	}
	onEngineThread()
	return clearHTTPCache(nil, done)
}

// ClearHTTPCache empties the context's HTTP cache, kept in memory, and what
// its network learnt of servers, as the function ClearHTTPCache.
func (c *Context) ClearHTTPCache(done func()) error {
	raw, ok := c.raw()
	if !ok {
		return ErrClosed
	}
	return clearHTTPCache(raw, done)
}

func clearHTTPCache(context *C.zebkit_context, done func()) error {
	h := onceHandle(func() {
		if done != nil {
			done()
		}
	})
	if !C.zebkit_clear_http_cache(context, C.zebkit_task_func(C.zk_task), data(h)) {
		release(h)
		return refused("the engine is stopping")
	}
	return nil
}

// Preconnect opens count (1 to 6) connections to url's origin in the engine
// profile's network, TLS included, for a page load or Fetch soon after; no
// request is sent, and one no request uses is closed after about 60 s. An
// error for a URL that is not http(s), a count out of range, or while the
// engine stops. Context.Preconnect for a context's; the startup setting
// preconnect does this as the engine starts.
func Preconnect(url string, count int) error {
	if !started.Load() {
		return ErrNotStarted
	}
	onEngineThread()
	return preconnect(nil, url, count)
}

// Preconnect opens count connections to url's origin in the context's
// network, as the function Preconnect.
func (c *Context) Preconnect(url string, count int) error {
	raw, ok := c.raw()
	if !ok {
		return ErrClosed
	}
	return preconnect(raw, url, count)
}

func preconnect(context *C.zebkit_context, url string, count int) error {
	if count < -1 || count > 7 {
		count = 7
	}
	curl := cString(url)
	defer free(curl)
	if !C.zebkit_preconnect(context, curl, C.int(count)) {
		return refused("not preconnected: an http(s) URL and a count from 1 to 6, with the engine running")
	}
	return nil
}

// Fetch is the function Fetch in the context's network: its cookies, its
// cache and the connections its pages opened (zebkit_fetch_in).
func (c *Context) Fetch(method, url, headers string, body []byte, done func(*FetchResponse)) error {
	raw, ok := c.raw()
	if !ok {
		return ErrClosed
	}
	cmethod := optionalString(method)
	defer free(cmethod)
	curl := cString(url)
	defer free(curl)
	cheaders := optionalString(headers)
	defer free(cheaders)
	h := onceHandle(func(response *FetchResponse) {
		if done != nil {
			done(response)
		}
	})
	C.zebkit_fetch_in(raw, cmethod, curl, cheaders, bytesArg(body), C.size_t(len(body)), C.zebkit_response_func(C.zk_fetch), data(h))
	return nil
}
