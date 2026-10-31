// The asked model of docs/ROADMAP_HOOKS.md for one hook: single-use requests
// on the engine's thread, with a deadline, whose function is the view's or that
// of the view that opened it (zebkit_view::Decider), and whose outcome is told
// afterwards. Traits names the hook's members of zebkit_view and its decisions.

#ifndef ZEBKIT_ENGINE_HOOK_REQUESTS_H_
#define ZEBKIT_ENGINE_HOOK_REQUESTS_H_

#include <stdint.h>
#include <string.h>

#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "zebkit/engine/view.h"

namespace zebkit {

template <typename Traits>
class HookRequests {
 public:
  using Decision = typename Traits::Decision;
  using Reply = base::OnceCallback<void(Decision)>;
  struct Entry {
    base::WeakPtr<zebkit_view> view;
    base::DictValue metadata;
    base::TimeTicks deadline;
    Reply reply;
  };

  static HookRequests& Get() {
    static base::NoDestructor<HookRequests> requests;
    return *requests;
  }

  void Start() { active_ = true; }
  bool Ready() const {
    return active_ && content::BrowserThread::CurrentlyOn(content::BrowserThread::UI);
  }
  // Ends every request with the refusal, for the engine going down.
  void Stop() {
    active_ = false;
    while (!pending_.empty())
      Settle(pending_.begin()->first, Traits::Refusal(), "shutdown");
  }

  // The function that decides for @view: nullptr when none does.
  static bool HasDecider(zebkit_view* view) {
    zebkit_view* decider = view ? view->Decider(Traits::kHandler) : nullptr;
    return decider && decider->*(Traits::kHandler);
  }
  // A popup whose opener has closed: refused by every hook.
  static bool OrphanedPopup(zebkit_view* view) {
    return view && !view->Decider(Traits::kHandler);
  }

  // Queues a request; false, and @reply unused, when too many wait on @view.
  bool Ask(zebkit_view* view, base::DictValue metadata, Reply& reply) {
    if (!Ready() || next_ == std::numeric_limits<uintptr_t>::max())
      return false;
    size_t count = 0;
    for (const auto& [id, entry] : pending_)
      count += entry.view.get() == view;
    if (count >= 1024)
      return false;
    const uintptr_t id = next_++;
    const uint32_t timeout = view->*(Traits::kTimeout);
    metadata.Set("id", base::NumberToString(id));
    metadata.Set("state", "pending");
    metadata.Set("timeoutMs", static_cast<int>(timeout));
    metadata.Set("deadlineUnixMs",
                 (base::Time::Now() + base::Milliseconds(timeout))
                     .InMillisecondsFSinceUnixEpoch());
    Entry entry;
    entry.view = view->GetWeakPtr();
    entry.metadata = std::move(metadata);
    entry.deadline = base::TimeTicks::Now() + base::Milliseconds(timeout);
    entry.reply = std::move(reply);
    pending_.emplace(id, std::move(entry));
    auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    runner->PostTask(FROM_HERE, base::BindOnce(&HookRequests::Deliver, id));
    runner->PostDelayedTask(FROM_HERE, base::BindOnce(&HookRequests::Check, id),
                            base::Milliseconds(timeout));
    last_id_ = id;
    return true;
  }
  uintptr_t last_id() const { return last_id_; }

  // The request when it is still pending. A deadline passed, or a view gone,
  // settles it first: blocked hosts cannot answer after their deadline.
  Entry* Find(uintptr_t id) {
    Check(id);
    return Peek(id);
  }

  // The reply can destroy what asked and close the view; nothing of this
  // object's is touched after it.
  void Settle(uintptr_t id, Decision decision, const char* reason) {
    auto it = pending_.find(id);
    if (it == pending_.end())
      return;
    Entry entry = std::move(it->second);
    pending_.erase(it);
    entry.metadata.Set("state", Traits::State(decision));
    entry.metadata.Set("reason", reason);
    Report(entry.view, std::move(entry.metadata));
    if (entry.reply)
      std::move(entry.reply).Run(std::move(decision));
  }

  // What asked has gone: the reply is dropped, unrun.
  void Abandon(uintptr_t id) {
    auto it = pending_.find(id);
    if (it == pending_.end())
      return;
    it->second.reply.Reset();
    Settle(id, Traits::Refusal(), "cancelled");
  }

  void CancelAll(zebkit_view* view, const char* reason) {
    for (uintptr_t id : IdsOf(view))
      Settle(id, Traits::Refusal(), reason);
  }

  std::vector<uintptr_t> IdsOf(zebkit_view* view) const {
    std::vector<uintptr_t> ids;
    for (const auto& [id, entry] : pending_)
      if (entry.view.get() == view)
        ids.push_back(id);
    return ids;
  }

  // zebkit_view_on_<hook>: replacing a function ends what it was asked.
  void Install(zebkit_view* view, auto function, void* data) {
    if (!Ready() || !view || !view->web_contents())
      return;
    if (view->*(Traits::kHandler) == function && view->*(Traits::kData) == data)
      return;
    auto weak = view->GetWeakPtr();
    for (uintptr_t id : IdsOf(view)) {
      Settle(id, Traits::Unanswered(), "handler-changed");
      if (!weak)
        return;
    }
    view->*(Traits::kHandler) = function;
    view->*(Traits::kData) = data;
  }

  void Observe(zebkit_view* view, auto function, void* data) {
    if (!Ready() || !view || !view->web_contents())
      return;
    ++(view->*(Traits::kGeneration));
    view->*(Traits::kResult) = function;
    view->*(Traits::kResultData) = data;
  }

  bool SetTimeout(zebkit_view* view, uint32_t milliseconds) {
    if (!Ready() || !view || !view->web_contents() || !milliseconds || milliseconds > 300000)
      return false;
    view->*(Traits::kTimeout) = milliseconds;
    return true;
  }

  // Owned JSON while pending; nullptr after it.
  char* Info(uintptr_t id) {
    if (!Ready())
      return nullptr;
    Entry* entry = Find(id);
    return entry ? strdup(base::WriteJson(entry->metadata).value_or("{}").c_str()) : nullptr;
  }

 private:
  friend class base::NoDestructor<HookRequests>;
  HookRequests() = default;

  Entry* Peek(uintptr_t id) {
    auto it = pending_.find(id);
    return it == pending_.end() ? nullptr : &it->second;
  }

  static void Check(uintptr_t id) {
    HookRequests& self = Get();
    Entry* entry = self.Peek(id);
    if (!entry)
      return;
    if (!entry->view || !entry->view->web_contents())
      self.Settle(id, Traits::Refusal(), "cancelled");
    else if (base::TimeTicks::Now() >= entry->deadline)
      self.Settle(id, Traits::Unanswered(), "timeout");
  }

  static void Deliver(uintptr_t id) {
    HookRequests& self = Get();
    Entry* entry = self.Find(id);
    if (!entry)
      return;
    auto view = entry->view;
    zebkit_view* decider = view->Decider(Traits::kHandler);
    if (!decider) {
      self.Settle(id, Traits::Refusal(), "opener-closed");
      return;
    }
    if (!(decider->*(Traits::kHandler))) {
      self.Settle(id, Traits::Unanswered(), "handler-changed");
      return;
    }
    const std::string json = base::WriteJson(entry->metadata).value_or("{}");
    auto function = decider->*(Traits::kHandler);
    void* data = decider->*(Traits::kData);
    // May answer, replace the function, or close the view.
    function(view.get(), reinterpret_cast<typename Traits::Request*>(id), json.c_str(), data);
  }

  static void Report(base::WeakPtr<zebkit_view> view, base::DictValue event) {
    if (!view || !(view.get()->*(Traits::kResult)))
      return;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, uint64_t generation, std::string json) {
              if (!view || view.get()->*(Traits::kGeneration) != generation ||
                  !(view.get()->*(Traits::kResult)))
                return;
              auto function = view.get()->*(Traits::kResult);
              function(view.get(), json.c_str(), view.get()->*(Traits::kResultData));
            },
            view, view.get()->*(Traits::kGeneration),
            base::WriteJson(event).value_or("{}")));
  }

  bool active_ = false;
  uintptr_t next_ = 1;
  uintptr_t last_id_ = 0;
  std::map<uintptr_t, Entry> pending_;
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_HOOK_REQUESTS_H_
