// A page's context menu (docs/SYSTEM_INTEGRATION.md, section 8): the default
// items for what was right-clicked, their commands run by the engine against
// the context it captured (so a page cannot name what is saved or opened),
// and the menu shown with the system's look: NSMenu on macOS, Chromium's
// views menu elsewhere. Without a window nothing is shown; commands still run.

#ifndef ZEBKIT_ENGINE_CONTEXT_MENU_H_
#define ZEBKIT_ENGINE_CONTEXT_MENU_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/global_routing_id.h"

struct zebkit_view;

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace zebkit {

struct ContextMenuEntry {
  std::string id;      // a default command ("copy") or the application's own
  std::string title;
  bool enabled = true;
  bool separator = false;
};

// What a right click captured, kept for its menu's commands.
struct CapturedContextMenu {
  uint64_t id = 0;
  content::GlobalRenderFrameHostId frame;
  content::ContextMenuParams params;
};

// The default items for @params in @contents.
std::vector<ContextMenuEntry> DefaultContextMenu(content::WebContents* contents,
                                                 const content::ContextMenuParams& params);
base::ListValue ContextMenuValue(const std::vector<ContextMenuEntry>& entries);
// Keeps @params for the menu's commands; its id.
uint64_t CaptureContextMenu(zebkit_view* view, content::RenderFrameHost& frame,
                            const content::ContextMenuParams& params);
const CapturedContextMenu* FindContextMenu(zebkit_view* view, uint64_t id);
// Runs the default command @command against @menu. False for a command that
// is not the engine's, or not enabled for what was right-clicked.
bool RunContextCommand(zebkit_view* view, const CapturedContextMenu& menu, std::string_view command);
// Shows @entries where @menu was asked for; a default item chosen runs, an
// application's goes to @chosen ("" when the menu closes with nothing).
// False without a window.
bool PresentContextMenu(zebkit_view* view, const CapturedContextMenu& menu,
                        std::vector<ContextMenuEntry> entries,
                        base::OnceCallback<void(std::string)> chosen);
// Platform presenters (context_menu_views.cc, context_menu_mac.mm).
bool PresentNativeMenu(zebkit_view* view, content::RenderFrameHost* frame, const gfx::Point& point,
                       std::vector<ContextMenuEntry> entries, base::OnceCallback<void(std::string)> chosen);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_CONTEXT_MENU_H_
