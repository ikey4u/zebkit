// zebkit_view_attach() on views (Linux): the view's page in a WebView of
// its own, over the parent's page in the parent's window, placed by bounds
// rather than by the window's layout. engine_mac.mm has macOS's.

#include <map>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/shell.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

struct Attached {
  base::WeakPtr<zebkit_view> parent;
  raw_ptr<views::WebView> host = nullptr;
};

std::map<const zebkit_view*, Attached>& AttachedViews() {
  static base::NoDestructor<std::map<const zebkit_view*, Attached>> attached;
  return *attached;
}

views::Widget* WidgetOf(zebkit_view* view) {
  content::Shell* shell = view ? view->shell() : nullptr;
  return shell ? views::Widget::GetWidgetForNativeWindow(shell->window())
               : nullptr;
}

// The WebView in @view's tree that shows @contents.
views::WebView* WebViewShowing(views::View* view,
                               content::WebContents* contents) {
  if (views::WebView* web_view = views::AsViewClass<views::WebView>(view)) {
    if (web_view->web_contents() == contents)
      return web_view;
  }
  for (views::View* child : view->children()) {
    if (views::WebView* found = WebViewShowing(child, contents))
      return found;
  }
  return nullptr;
}

}  // namespace

bool AttachView(zebkit_view* view, zebkit_view* parent, const gfx::Rect& bounds) {
  if (!view || !parent || view == parent || !view->web_contents())
    return false;
  views::Widget* parent_widget = WidgetOf(parent);
  views::View* contents =
      parent_widget ? parent_widget->GetContentsView() : nullptr;
  if (!contents)
    return false;
  DetachView(view);
  // Out of its own window's WebView first: a page's window has one parent.
  views::Widget* own = WidgetOf(view);
  if (own && own->GetRootView()) {
    if (views::WebView* old =
            WebViewShowing(own->GetRootView(), view->web_contents())) {
      old->SetWebContents(nullptr);
    }
  }
  auto host = std::make_unique<views::WebView>(
      view->web_contents()->GetBrowserContext());
  // Over the page, where the bounds say, whatever the window's layout does.
  host->SetProperty(views::kViewIgnoredByLayoutKey, true);
  views::WebView* added = contents->AddChildView(std::move(host));
  added->SetWebContents(view->web_contents());
  added->SetBoundsRect(bounds);
  if (own)
    own->Hide();
  AttachedViews()[view] = {parent->GetWeakPtr(), added};
  return true;
}

void SetAttachedBounds(zebkit_view* view, const gfx::Rect& bounds) {
  auto it = AttachedViews().find(view);
  if (it != AttachedViews().end() && it->second.parent)
    it->second.host->SetBoundsRect(bounds);
}

void SetAttachedVisible(zebkit_view* view, bool visible) {
  auto it = AttachedViews().find(view);
  if (it != AttachedViews().end() && it->second.parent)
    it->second.host->SetVisible(visible);
}

void DetachView(zebkit_view* view) {
  auto it = AttachedViews().find(view);
  if (it == AttachedViews().end())
    return;
  // The host goes with its parent's window when that closes first.
  if (it->second.parent && it->second.host->parent()) {
    views::WebView* host = it->second.host;
    host->SetWebContents(nullptr);
    host->parent()->RemoveChildViewT(host);
  }
  AttachedViews().erase(it);
}

}  // namespace zebkit
