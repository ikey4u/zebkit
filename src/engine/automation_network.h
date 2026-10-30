// zebkit's automation, the network (docs/AUTOMATION.md): a page's requests
// through a URL loader factory in front of the network's, as Chrome's
// webRequest has one. Playwright's request, response, requestFinished and
// requestFailed events, and its routes: a request whose URL matches a
// pattern is held until it is continued (maybe changed), fulfilled with a
// response of the client's or aborted.

#ifndef ZEBKIT_ENGINE_AUTOMATION_NETWORK_H_
#define ZEBKIT_ENGINE_AUTOMATION_NETWORK_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace network {
class URLLoaderFactoryBuilder;
}  // namespace network

namespace zebkit {

// The network of the pages with automation, from ContentBrowserClient's
// WillCreateURLLoaderFactory(): a factory of @frame's page goes through
// zebkit's when the page has automation.
void InterceptURLLoaderFactory(content::RenderFrameHost* frame,
                               network::URLLoaderFactoryBuilder& builder);

// What automation.cc does with a page's network.
class Network {
 public:
  using EventFunc =
      base::RepeatingCallback<void(const std::string& event, base::DictValue)>;

  // The page's network, made with its first automation command; events go
  // to @emit.
  static Network* For(content::WebContents* contents);
  static void Create(content::WebContents* contents, EventFunc emit);

  virtual ~Network() = default;

  // The latest intercepted main-resource response for a navigation URL.
  virtual std::optional<base::DictValue> NavigationResponse(const std::string& url) = 0;

  // Page.setNetworkInterceptionPatterns: [{glob}, {regexSource,
  // regexFlags}]; empty for none.
  virtual bool SetPatterns(const base::ListValue& patterns,
                           std::string* error) = 0;
  // Page.setWebSocketInterceptionPatterns: the WebSockets whose URL
  // matches are routed (automation.cc); and whether @url is one.
  virtual bool SetWebSocketPatterns(const base::ListValue& patterns,
                                    std::string* error) = 0;
  virtual bool RoutesWebSocket(const std::string& url) const = 0;

  // Page.setExtraHTTPHeaders: [{name, value}].
  virtual void SetExtraHeaders(const base::ListValue& headers) = 0;
  // BrowserContext.setOffline: requests fail as without a network.
  virtual void SetOffline(bool offline) = 0;

  // Route.continue ({url, method, headers, postData}), Route.fulfill
  // ({status, headers, body, isBase64}), Route.abort ({errorCode}): false
  // when the route is not held.
  virtual bool Continue(const std::string& route,
                        const base::DictValue& overrides) = 0;
  virtual bool Fulfill(const std::string& route,
                       const base::DictValue& response) = 0;
  virtual bool Abort(const std::string& route, const std::string& error) = 0;

  // Response.body: the body of request @request's response, when it has
  // come (up to 10 MiB, of the last 200 responses); nullopt otherwise.
  virtual std::optional<std::string> Body(const std::string& request) = 0;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_NETWORK_H_
