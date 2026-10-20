// URL schemes of the application's own (zebkit_scheme_*, scheme.cc): known
// in every process, answered in the browser process by the application.

#ifndef ZEBKIT_ENGINE_SCHEME_H_
#define ZEBKIT_ENGINE_SCHEME_H_

#include <string>
#include <string_view>
#include <vector>

#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace base {
class CommandLine;
}

namespace zebkit {

// The switch that carries the schemes to child processes, and that an
// application may give itself: --zebkit-scheme=app,standard,secure;asset,...
inline constexpr char kSchemeSwitch[] = "zebkit-scheme";

struct Scheme {
  std::string name;
  unsigned flags = 0;
};

// The schemes of this process: registered by the application, and from the
// command line.
const std::vector<Scheme>& Schemes();

// For ContentClient::AddAdditionalSchemes(), in every process.
void AddSchemes(content::ContentClient::Schemes* schemes);

// What Blink is told of them, in a renderer.
void RegisterSchemesInRenderer();

// What a child process is started with.
void AppendSchemeSwitch(base::CommandLine* command_line);

// The browser process: the schemes pages may use, and their loaders.
void AllowSchemes();
bool IsApplicationScheme(std::string_view scheme);
mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateSchemeLoaderFactory(
    const std::string& scheme);
void AddSchemeLoaderFactories(
    content::ContentBrowserClient::NonNetworkURLLoaderFactoryMap* factories);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_SCHEME_H_
