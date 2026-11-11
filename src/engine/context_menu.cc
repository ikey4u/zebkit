#include "zebkit/engine/context_menu.h"

#include <string.h>

#include <memory>
#include <utility>

#include "base/json/json_reader.h"
#include "base/strings/utf_string_conversions.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_request_utils.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "third_party/blink/public/common/context_menu_data/edit_flags.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/hooks.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

using Flags = blink::ContextMenuDataEditFlags;
using Media = blink::mojom::ContextMenuDataMediaType;

constexpr size_t kKept = 8;

constexpr net::NetworkTrafficAnnotationTag kSaveImage = net::DefineNetworkTrafficAnnotation(
    "zebkit_context_menu_save", R"(
      semantics {
        sender: "zebkit's context menu"
        description: "Saves an image the person right-clicked, as Chrome's Save Image As does."
        trigger: "The person chooses Save Image As in a page's context menu."
        data: "The image's URL, with the page's cookies."
        destination: WEBSITE
      }
      policy { cookies_allowed: YES setting: "None." policy_exception_justification: "None." })");

ContextMenuEntry Entry(const char* id, const char* title, bool enabled = true) {
  return {id, title, enabled, false};
}

ContextMenuEntry Separator() {
  ContextMenuEntry entry;
  entry.separator = true;
  return entry;
}

bool Has(const content::ContextMenuParams& params, int flag) {
  return params.edit_flags & flag;
}

void Copy(const std::u16string& text) {
  ui::ScopedClipboardWriter(ui::ClipboardBuffer::kCopyPaste).WriteText(text);
}

}  // namespace

std::vector<ContextMenuEntry> DefaultContextMenu(content::WebContents* contents,
                                                 const content::ContextMenuParams& params) {
  std::vector<ContextMenuEntry> entries;
  auto add_separator = [&] {
    if (!entries.empty() && !entries.back().separator)
      entries.push_back(Separator());
  };
  if (params.link_url.is_valid() && !params.link_url.is_empty()) {
    entries.push_back(Entry("openLink", "Open Link in New Window"));
    entries.push_back(Entry("copyLink", "Copy Link Address"));
  }
  if (params.media_type == Media::kImage && params.src_url.is_valid()) {
    add_separator();
    entries.push_back(Entry("saveImage", "Save Image As\xE2\x80\xA6"));
    entries.push_back(Entry("copyImage", "Copy Image"));
    entries.push_back(Entry("copyImageAddress", "Copy Image Address"));
  }
  if (params.is_editable) {
    add_separator();
    entries.push_back(Entry("undo", "Undo", Has(params, Flags::kCanUndo)));
    entries.push_back(Entry("redo", "Redo", Has(params, Flags::kCanRedo)));
    entries.push_back(Separator());
    entries.push_back(Entry("cut", "Cut", Has(params, Flags::kCanCut)));
    entries.push_back(Entry("copy", "Copy", Has(params, Flags::kCanCopy)));
    entries.push_back(Entry("paste", "Paste", Has(params, Flags::kCanPaste)));
    entries.push_back(Entry("selectAll", "Select All", Has(params, Flags::kCanSelectAll)));
  } else if (!params.selection_text.empty()) {
    add_separator();
    entries.push_back(Entry("copy", "Copy"));
  }
  if (entries.empty()) {
    content::NavigationController& controller = contents->GetController();
    entries.push_back(Entry("back", "Back", controller.CanGoBack()));
    entries.push_back(Entry("forward", "Forward", controller.CanGoForward()));
    entries.push_back(Entry("reload", "Reload"));
  }
  return entries;
}

base::ListValue ContextMenuValue(const std::vector<ContextMenuEntry>& entries) {
  base::ListValue list;
  for (const ContextMenuEntry& entry : entries) {
    base::DictValue value;
    if (entry.separator) {
      value.Set("separator", true);
    } else {
      value.Set("id", entry.id);
      value.Set("title", entry.title);
      value.Set("enabled", entry.enabled);
    }
    list.Append(std::move(value));
  }
  return list;
}

uint64_t CaptureContextMenu(zebkit_view* view, content::RenderFrameHost& frame,
                            const content::ContextMenuParams& params) {
  auto& kept = view->context_menus_;
  if (kept.size() >= kKept)
    kept.erase(kept.begin());
  CapturedContextMenu menu;
  menu.id = view->next_context_menu_++;
  menu.frame = frame.GetGlobalId();
  menu.params = params;
  kept.push_back(std::move(menu));
  return kept.back().id;
}

const CapturedContextMenu* FindContextMenu(zebkit_view* view, uint64_t id) {
  for (const CapturedContextMenu& menu : view->context_menus_)
    if (menu.id == id)
      return &menu;
  return nullptr;
}

bool RunContextCommand(zebkit_view* view, const CapturedContextMenu& menu, std::string_view command) {
  content::WebContents* contents = view->web_contents();
  if (!contents)
    return false;
  // Only what the menu offered for what was right-clicked, and enabled.
  bool offered = false;
  for (const ContextMenuEntry& entry : DefaultContextMenu(contents, menu.params))
    offered |= !entry.separator && entry.id == command && entry.enabled;
  if (!offered)
    return false;
  const content::ContextMenuParams& params = menu.params;
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(menu.frame);
  if (command == "back") {
    contents->GetController().GoBack();
  } else if (command == "forward") {
    contents->GetController().GoForward();
  } else if (command == "reload") {
    contents->GetController().Reload(content::ReloadType::NORMAL, true);
  } else if (command == "undo") {
    contents->Undo();
  } else if (command == "redo") {
    contents->Redo();
  } else if (command == "cut") {
    contents->Cut();
  } else if (command == "copy") {
    contents->Copy();
  } else if (command == "paste") {
    contents->Paste();
  } else if (command == "selectAll") {
    contents->SelectAll();
  } else if (command == "copyLink") {
    Copy(base::UTF8ToUTF16(params.link_url.spec()));
  } else if (command == "copyImageAddress") {
    Copy(base::UTF8ToUTF16(params.src_url.spec()));
  } else if (command == "copyImage") {
    if (!frame || !frame->IsActive())
      return false;
    frame->CopyImageAt(params.x, params.y);
  } else if (command == "openLink") {
    // A new window as a click with the middle button asks for one: the popup
    // policy and the application's new-window hooks decide.
    content::OpenURLParams open(params.link_url, content::Referrer(params.page_url, params.referrer_policy),
                                WindowOpenDisposition::NEW_FOREGROUND_TAB, ui::PAGE_TRANSITION_LINK, false);
    if (frame)
      open.initiator_origin = frame->GetLastCommittedOrigin();
    contents->OpenURL(open, {});
  } else if (command == "saveImage") {
    // Through the download hook (zebkit_view_on_download), as a download.
    auto download = content::DownloadRequestUtils::CreateDownloadForWebContentsMainFrame(
        contents, params.src_url, kSaveImage);
    download->set_prompt(true);
    download->set_content_initiated(false);
    contents->GetBrowserContext()->GetDownloadManager()->DownloadUrl(std::move(download));
  } else {
    return false;
  }
  return true;
}

bool PresentContextMenu(zebkit_view* view, const CapturedContextMenu& menu,
                        std::vector<ContextMenuEntry> entries,
                        base::OnceCallback<void(std::string)> chosen) {
  if (zebkit_is_headless() || HasNoDisplay(view->web_contents()))
    return false;
  content::RenderFrameHost* frame = content::RenderFrameHost::FromID(menu.frame);
  if (!frame || !frame->IsActive())
    return false;
  const uint64_t id = menu.id;
  return PresentNativeMenu(
      view, frame, gfx::Point(menu.params.x, menu.params.y), std::move(entries),
      base::BindOnce(
          [](base::WeakPtr<zebkit_view> view, uint64_t id, base::OnceCallback<void(std::string)> chosen,
             std::string item) {
            if (!view)
              return;
            // A default command runs against what was captured; anything else
            // is the application's.
            if (const CapturedContextMenu* menu = FindContextMenu(view.get(), id);
                menu && RunContextCommand(view.get(), *menu, item))
              return;
            if (chosen)
              std::move(chosen).Run(std::move(item));
          },
          view->GetWeakPtr(), id, std::move(chosen)));
}

}  // namespace zebkit

extern "C" {

bool zebkit_view_context_menu_run(zebkit_view* view, uint64_t menu_id, const char* command, char** error) {
  auto fail = [&](const char* message) {
    if (error)
      *error = strdup(message);
    return false;
  };
  if (error)
    *error = nullptr;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) || !zebkit::LiveView(view) || !view->web_contents())
    return fail("a live view on the engine's thread");
  const zebkit::CapturedContextMenu* menu = zebkit::FindContextMenu(view, menu_id);
  if (!menu)
    return fail("no such menu: the last few right clicks are kept");
  if (!zebkit::RunContextCommand(view, *menu, command ? command : ""))
    return fail("not a command this menu offered, or not enabled for what was right-clicked");
  return true;
}

bool zebkit_view_context_menu_show(zebkit_view* view, uint64_t menu_id, const char* items_json,
                                   zebkit_context_menu_item_func func, void* data, char** error) {
  auto fail = [&](const char* message) {
    if (error)
      *error = strdup(message);
    return false;
  };
  if (error)
    *error = nullptr;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) || !zebkit::LiveView(view) || !view->web_contents())
    return fail("a live view on the engine's thread");
  const zebkit::CapturedContextMenu* menu = zebkit::FindContextMenu(view, menu_id);
  if (!menu)
    return fail("no such menu: the last few right clicks are kept");
  std::vector<zebkit::ContextMenuEntry> entries;
  if (!items_json) {
    entries = zebkit::DefaultContextMenu(view->web_contents(), menu->params);
  } else {
    std::optional<base::Value> parsed = base::JSONReader::Read(items_json, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_list() || parsed->GetList().size() > 256)
      return fail("items: a JSON array of at most 256 {id, title, enabled} or {separator: true}");
    // Default ids keep their own title and state unless given.
    const auto defaults = zebkit::DefaultContextMenu(view->web_contents(), menu->params);
    for (const base::Value& value : parsed->GetList()) {
      const base::DictValue* item = value.GetIfDict();
      if (!item)
        return fail("items: each is an object");
      zebkit::ContextMenuEntry entry;
      if (item->FindBool("separator").value_or(false)) {
        entry.separator = true;
        entries.push_back(std::move(entry));
        continue;
      }
      const std::string* id = item->FindString("id");
      if (!id || id->empty())
        return fail("items: each has an id");
      entry.id = *id;
      bool known_default = false;
      for (const auto& known : defaults) {
        if (known.id == *id) {
          entry = known;
          known_default = true;
        }
      }
      if (const std::string* title = item->FindString("title"))
        entry.title = *title;
      if (entry.title.empty())
        return fail("items: an item of the application's has a title");
      // A default item is never more enabled than what was right-clicked allows.
      if (auto enabled = item->FindBool("enabled"))
        entry.enabled = known_default ? entry.enabled && *enabled : *enabled;
      entries.push_back(std::move(entry));
    }
  }
  base::OnceCallback<void(std::string)> chosen;
  if (func) {
    chosen = base::BindOnce(
        [](base::WeakPtr<zebkit_view> view, zebkit_context_menu_item_func func, void* data, std::string item) {
          if (view)
            func(view.get(), item.c_str(), data);
        },
        view->GetWeakPtr(), func, data);
  }
  if (!zebkit::PresentContextMenu(view, *menu, std::move(entries), std::move(chosen)))
    return fail("unsupported: no window to show a menu in");
  return true;
}

}  // extern "C"
