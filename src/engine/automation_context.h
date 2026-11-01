// zebkit's automation, what Playwright's BrowserContext and
// APIRequestContext do for a page (docs/AUTOMATION.md): its profile's
// cookies, requests through the engine's network, the media features it
// emulates. Each answers once with a result or an error.

#ifndef ZEBKIT_ENGINE_AUTOMATION_CONTEXT_H_
#define ZEBKIT_ENGINE_AUTOMATION_CONTEXT_H_

#include "url/gurl.h"
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace zebkit {

// httpCredentials of a command: the protocol's list, or one as the C API takes it.
base::ListValue HttpCredentialsOf(const base::DictValue& params);
// browserContext.ts's findMatchingHttpCredentials(): the first for @url's origin.
const base::DictValue* MatchingHttpCredentials(const base::ListValue& credentials,
                                               const GURL& url);

using ContextDone =
    base::OnceCallback<void(base::Value result, const std::string& error)>;

// BrowserContext.cookies: {cookies}, of "urls" when given.
void ContextCookies(content::BrowserContext* profile,
                    const base::DictValue& params,
                    ContextDone done);
// BrowserContext.addCookies: "cookies", each with url, or domain and path.
void ContextAddCookies(content::BrowserContext* profile,
                       const base::DictValue& params,
                       ContextDone done);
// BrowserContext.clearCookies: all, or those of "name", "domain", "path".
void ContextClearCookies(content::BrowserContext* profile,
                         const base::DictValue& params,
                         ContextDone done);
// APIRequestContext.fetch: {url, method, headers, postData (base64),
// timeout, maxRedirects}, through the profile's network: {response: {url,
// status, statusText, headers, body (base64)}}.
void ContextFetch(content::BrowserContext* profile,
                  const base::DictValue& params,
                  ContextDone done);
// Page.emulateMedia: colorScheme, reducedMotion, contrast, forcedColors
// ("no-override" for the system's); "media" is the frames' agents'
// (automation.cc).
void ContextEmulateMedia(content::WebContents* contents,
                         const base::DictValue& params,
                         ContextDone done);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_CONTEXT_H_
