// Connections opened ahead of the requests that use them (preconnect.cc).

#ifndef ZEBKIT_ENGINE_PRECONNECT_H_
#define ZEBKIT_ENGINE_PRECONNECT_H_

class GURL;
namespace content {
class BrowserContext;
}

namespace zebkit {
// @count connections to @url's origin in @profile's network, keyed as a
// top-level navigation to @url is.
void Preconnect(content::BrowserContext* profile, const GURL& url, int count);
// The startup setting "preconnect" (--zebkit-preconnect), for the engine's
// profile as it starts.
void PreconnectAtStartup(content::BrowserContext* profile);
}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PRECONNECT_H_
