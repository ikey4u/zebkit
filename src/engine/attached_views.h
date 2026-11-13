// The views attached to another view's window (zebkit_view_attach()), by
// view, with what the platform keeps of each: attach.cc's for views,
// engine_mac.mm's for macOS. Each platform's Entry has `parent`, a
// base::WeakPtr<zebkit_view>.

#ifndef ZEBKIT_ENGINE_ATTACHED_VIEWS_H_
#define ZEBKIT_ENGINE_ATTACHED_VIEWS_H_

#include <map>

#include "base/no_destructor.h"
#include "zebkit/engine/view.h"

namespace zebkit {

template <typename Entry>
std::map<const zebkit_view*, Entry>& AttachedViewsOf() {
  static base::NoDestructor<std::map<const zebkit_view*, Entry>> attached;
  return *attached;
}

// Closes the views attached to @parent for which @in_parent(entry) is true,
// as a task: the parent is inside its own teardown.
template <typename Entry, typename Filter>
void CloseAttachedViewsOf(zebkit_view* parent, Filter in_parent) {
  for (const auto& [view, entry] : AttachedViewsOf<Entry>()) {
    if (entry.parent.get() == parent && in_parent(entry))
      CloseSoon(const_cast<zebkit_view*>(view)->GetWeakPtr());
  }
}

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_ATTACHED_VIEWS_H_
