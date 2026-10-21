// zebkit.* calls from a page over the engine's own scheme (call.cc).

#ifndef ZEBKIT_ENGINE_CALL_H_
#define ZEBKIT_ENGINE_CALL_H_

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace zebkit {

// The scheme of the calls, zebkit://call: known in every process, and
// loaded only from frames, by the factory of each.
inline constexpr char kCallScheme[] = "zebkit";

// The loader factory of zebkit://call for one frame.
mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateCallLoaderFactory(
    int render_process_id,
    int render_frame_id);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_CALL_H_
