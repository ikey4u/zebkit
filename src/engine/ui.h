// zebkit.ui's native side: a tree of Chromium's views (ui/views), which
// the application's script builds and changes in batches of mount
// operations (ui.cc).

#ifndef ZEBKIT_ENGINE_UI_H_
#define ZEBKIT_ENGINE_UI_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

namespace zebkit {

class UiTree {
 public:
  // An event of a node: "click", "input" (with the new text), "submit",
  // "close" (a window).
  using EventSink = base::RepeatingCallback<
      void(int id, const std::string& event, base::Value value)>;

  UiTree(EventSink events, base::RepeatingClosure windows_changed);
  ~UiTree();

  // Applies a batch of operations, each an array:
  //   ["create", id, type]        window, view, text, button, input,
  //                               image, scroll, web
  //   ["set", id, {props}]
  //   ["append", parent, child, before]   before 0 for the end
  //   ["remove", id]              out of its parent, kept
  //   ["destroy", id]             with what is inside it
  //   ["show", window]   ["focus", id]
  // Returns "" or what was wrong; what came before it is applied.
  std::string Commit(const base::ListValue& operations);

  // A node and what is inside it, laid out: type, text, value and bounds
  // in its window's content area, for tests and for automation.
  base::Value Dump(int id);

  // What a user does: "click" a button, "type" text into an input,
  // "submit" an input. Returns "" or what was wrong.
  std::string Act(int id, const std::string& action, const base::Value& value);

  // A node as the compositor shows it, as a PNG file's bytes; empty when
  // there is nothing to show. Asynchronous: the picture is read back from
  // the window's next frame.
  void Capture(int id,
               base::OnceCallback<void(std::vector<uint8_t> png)> done);

  // Windows created and not closed.
  size_t open_windows() const;

  struct Node;

 private:
  Node* Find(int id);
  std::string Create(int id, const std::string& type);
  std::string Set(int id, const base::DictValue& props);
  std::string Append(int parent, int child, int before);
  std::string Remove(int id);
  std::string Destroy(int id);
  void Forget(int id);
  void WindowClosed(int id);

  EventSink events_;
  base::RepeatingClosure windows_changed_;
  std::map<int, std::unique_ptr<Node>> nodes_;
  base::WeakPtrFactory<UiTree> weak_factory_{this};
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_UI_H_
