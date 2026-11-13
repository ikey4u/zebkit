#ifdef UNSAFE_BUFFERS_BUILD
#pragma allow_unsafe_buffers
#endif
#include "zebkit/engine/permissions.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/permission_types.h"
#include "zebkit/engine/view.h"
#include <stdlib.h>
#include <string.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/embedder_permission_hooks.h"
#include "content/public/browser/permission_controller.h"
#include "content/public/browser/permission_descriptor_util.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "zebkit/engine/c_api_util.h"
#include "zebkit/engine/permission_manager.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace zebkit {
namespace {
using Type = blink::PermissionType;
using Status = blink::mojom::PermissionStatus;
constexpr size_t kMaxRules = 4096, kMaxRequests = 4096;
struct Group {
  PermissionManager::RequestReply reply;
  std::vector<content::PermissionResult> results;
  size_t remaining = 0;
};
struct Pending {
  base::WeakPtr<zebkit_view> view;
  // A named permission's code (NamedCode), with its answer; 0 for content's.
  int named = 0;
  base::OnceCallback<void(Status, bool decided)> done;
  content::WeakDocumentPtr document;
  content::GlobalRenderFrameHostId frame;
  std::shared_ptr<Group> group;
  size_t index = 0;
  Type type;
  url::Origin requesting, embedding;
  base::DictValue metadata;
  base::TimeTicks deadline;
};
struct Requests {
  bool active = false;
  uintptr_t next = 1;
  std::map<uintptr_t, std::unique_ptr<Pending>> pending;
};
Requests& Store() { static base::NoDestructor<Requests> store; return *store; }
bool Ready() { return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI) && Store().active; }
Status ToStatus(zebkit_permission_state state) {
  return state == ZEBKIT_PERMISSION_GRANTED ? Status::GRANTED : state == ZEBKIT_PERMISSION_DENIED ? Status::DENIED : Status::ASK;
}
zebkit_permission_state ToState(Status status) {
  return status == Status::GRANTED ? ZEBKIT_PERMISSION_GRANTED : status == Status::DENIED ? ZEBKIT_PERMISSION_DENIED : ZEBKIT_PERMISSION_PROMPT;
}
const char* StateName(Status status) { return status == Status::GRANTED ? "granted" : status == Status::DENIED ? "denied" : "prompt"; }
bool ValidState(zebkit_permission_state state) { return state >= ZEBKIT_PERMISSION_PROMPT && state <= ZEBKIT_PERMISSION_DENIED; }
Status DefaultReply(Type) { return Status::DENIED; }
// Named permissions keep their rules in the view's table under codes past
// every blink::PermissionType, without content's revisions: content never
// reads them.
constexpr const char* kNamed[] = {"file-system-read", "file-system-write", "protocol-handler"};
constexpr int kNamedBase = 1 << 20;
std::optional<int> NamedCode(std::string_view name) {
  for (size_t i = 0; i < std::size(kNamed); ++i)
    if (name == kNamed[i]) return kNamedBase + static_cast<int>(i);
  return std::nullopt;
}
// A remembered answer holds for a site under the same top-level site only: a
// camera granted to a widget under one page is not granted under every page.
std::string RememberKey(const url::Origin& requesting, const url::Origin& embedding) {
  return requesting == embedding ? requesting.Serialize() : requesting.Serialize() + " " + embedding.Serialize();
}
bool Scope(const char* text, bool wildcard, std::string* key, std::optional<url::Origin>* origin) {
  if (!text || !*text || strlen(text) > 8192) return false;
  if (wildcard && !strcmp(text, "*")) { *key = "*"; origin->reset(); return true; }
  const GURL url(text);
  if (!url.is_valid()) return false;
  auto parsed = url::Origin::Create(url);
  if (parsed.opaque()) return false;
  *key = parsed.Serialize(); *origin = std::move(parsed); return true;
}
std::optional<Status> ViewRule(zebkit_view* view, const url::Origin& requesting,
                                const url::Origin& embedding, Type type) {
  if (!zebkit::LiveView(view) || !view->web_contents()) return std::nullopt;
  const auto revision = content::GetEmbedderContextPermissionRevision(
      view->web_contents()->GetBrowserContext(), requesting, embedding, type);
  const std::string keys[] = {RememberKey(requesting, embedding), requesting.Serialize(), "*"};
  auto find = [&](Type permission) -> const zebkit_view::PermissionRule* {
    // A popup's own rules first, then those of the view that opened it.
    for (zebkit_view* target = view; target;
         target = target->inherits_policy_ ? target->policy_opener_.get() : nullptr) {
      for (const auto& origin : keys) {
        const auto it = target->permission_rules_.find({origin, static_cast<int>(permission)});
        if (it != target->permission_rules_.end() && it->second.revision > revision)
          return &it->second;
      }
    }
    return nullptr;
  };
  const auto* rule = find(type);
  if (!rule && type == Type::GEOLOCATION_APPROXIMATE) rule = find(Type::GEOLOCATION);
  return rule ? std::make_optional(ToStatus(rule->state)) : std::nullopt;
}
std::vector<Type> WrittenTypes(Type type, Status status) {
  std::vector<Type> types{type};
  if (type == Type::MIDI && status != Status::GRANTED) types.push_back(Type::MIDI_SYSEX);
  if (type == Type::MIDI_SYSEX && status == Status::GRANTED) types.push_back(Type::MIDI);
  return types;
}
bool CanRemember(zebkit_view* view, const std::string& origin, Type type, Status status) {
  if (type == Type::GEOLOCATION_APPROXIMATE) return false;
  size_t added = 0;
  for (auto permission : WrittenTypes(type, status))
    added += !view->permission_rules_.contains({origin, static_cast<int>(permission)});
  return view->permission_rules_.size() + added <= kMaxRules;
}
void WriteRule(zebkit_view* view, const std::string& origin, Type type, Status status) {
  const auto revision = content::NextEmbedderPermissionRevision();
  for (auto permission : WrittenTypes(type, status))
    view->permission_rules_[{origin, static_cast<int>(permission)}] = {ToState(status), revision};
}
void Expire(content::BrowserContext* context, zebkit_view* only = nullptr);
void Scan(content::BrowserContext* context, base::WeakPtr<zebkit_view> only, bool restricted);

std::optional<Status> NamedRule(zebkit_view* view, const url::Origin& requesting,
                                const url::Origin& embedding, int code) {
  if (!view) return std::nullopt;
  const std::string keys[] = {RememberKey(requesting, embedding), requesting.Serialize(), "*"};
  for (zebkit_view* target = view; target;
       target = target->inherits_policy_ ? target->policy_opener_.get() : nullptr) {
    for (const auto& origin : keys) {
      const auto it = target->permission_rules_.find({origin, code});
      if (it != target->permission_rules_.end()) return ToStatus(it->second.state);
    }
  }
  return std::nullopt;
}
void WriteNamed(zebkit_view* view, const std::string& origin, int code, Status status) {
  view->permission_rules_[{origin, code}] = {ToState(status), content::NextEmbedderPermissionRevision()};
}

void Remember(zebkit_view* view, const std::string& origin, Type type, Status status) {
  Expire(nullptr, view);
  auto weak = view->GetWeakPtr();
  content::UpdateEmbedderPermissionState(view->web_contents()->GetBrowserContext(),
      base::BindOnce([](base::WeakPtr<zebkit_view> target, std::string key, Type permission, Status state) {
        if (target) WriteRule(target.get(), key, permission, state);
      }, weak, origin, type, status));
  if (weak) Scan(nullptr, weak, true);
}
Pending* Find(uintptr_t id) {
  auto it = Store().pending.find(id);
  return it == Store().pending.end() ? nullptr : it->second.get();
}
bool Live(const Pending& request) {
  auto* frame = request.document.AsRenderFrameHostIfValid();
  return request.view && request.view->web_contents() && frame && frame->IsActive();
}
// A fixed policy supersedes a prompt already in flight. Context prompt
// overrides are terminal CDP decisions; native prompt rules ask the host.
std::optional<Status> FixedDecision(const Pending& request) {
  if (!Live(request)) return Status::DENIED;
  if (request.named) {
    auto rule = NamedRule(request.view.get(), request.requesting, request.embedding, request.named);
    return rule && *rule != Status::ASK ? rule : std::nullopt;
  }
  if (auto rule = ViewRule(request.view.get(), request.requesting, request.embedding, request.type))
    return *rule == Status::ASK ? std::nullopt : rule;
  return content::GetEmbedderContextPermissionOverride(request.view->web_contents()->GetBrowserContext(),
      request.requesting, request.embedding, request.type);
}
void Report(base::WeakPtr<zebkit_view> view, base::DictValue event) {
  if (!Store().active || !view || !view->on_permission_result_) return;
  const auto json = base::WriteJson(event).value_or("{}");
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce([](base::WeakPtr<zebkit_view> target, std::string message) {
        if (Store().active && target && target->on_permission_result_)
          target->on_permission_result_(target.get(), message.c_str(), target->on_permission_result_data_);
      }, view, json));
}
void Finish(uintptr_t id, Status status, bool remember, const char* reason) {
  auto it = Store().pending.find(id);
  if (it == Store().pending.end()) return;
  auto request = std::move(it->second);
  Store().pending.erase(it);
  if (request->named) {
    if (remember && Live(*request))
      WriteNamed(request->view.get(), RememberKey(request->requesting, request->embedding), request->named, status);
    auto event = request->metadata.Clone();
    event.Set("state", StateName(status)); event.Set("reason", reason); event.Set("remembered", remember);
    Report(request->view, std::move(event));
    const bool decided = !strcmp(reason, "answered") || !strcmp(reason, "policy-changed");
    std::move(request->done).Run(status, decided);
    return;
  }
  if (remember && Live(*request)) Remember(request->view.get(), RememberKey(request->requesting, request->embedding), request->type, status);
  auto event = request->metadata.Clone();
  event.Set("state", StateName(status)); event.Set("reason", reason); event.Set("remembered", remember);
  Report(request->view, std::move(event));
  auto group = request->group;
  group->results[request->index] = content::CreateEmbedderPermissionResult(request->type, status);
  if (--group->remaining == 0) std::move(group->reply).Run(group->results);
}
void TimedOut(uintptr_t id);
void Deliver(uintptr_t id) {
  auto* request = Find(id);
  if (!request) return;
  if (!Live(*request)) { Finish(id, Status::DENIED, false, "cancelled"); return; }
  if (base::TimeTicks::Now() >= request->deadline) { TimedOut(id); return; }
  if (auto status = FixedDecision(*request)) { Finish(id, *status, false, "policy-changed"); return; }
  auto view = request->view;
  zebkit_view* decider = view->Decider(&zebkit_view::on_permission_);
  if (!decider || !decider->on_permission_) { Finish(id, DefaultReply(request->type), false, "handler-changed"); return; }
  // The host may reply or close the view here; never borrow request-owned
  // strings or inspect the request after invoking it.
  const auto json = base::WriteJson(request->metadata).value_or("{}");
  auto function = decider->on_permission_; void* data = decider->on_permission_data_;
  function(view.get(), reinterpret_cast<zebkit_permission_request*>(id), json.c_str(), data);
}
void TimedOut(uintptr_t id) {
  auto* request = Find(id);
  if (!request) return;
  const auto state = Live(*request) ? FixedDecision(*request).value_or(DefaultReply(request->type)) : Status::DENIED;
  Finish(id, state, false, "timeout");
}
void Expire(content::BrowserContext* context, zebkit_view* only) {
  std::vector<uintptr_t> ids;
  const auto now = base::TimeTicks::Now();
  for (const auto& [id, request] : Store().pending) {
    auto view = request->view;
    if (only && view.get() != only) continue;
    if (context && (!view || !view->web_contents() || view->web_contents()->GetBrowserContext() != context)) continue;
    if (now >= request->deadline) ids.push_back(id);
  }
  for (auto id : ids) TimedOut(id);
}
void Scan(content::BrowserContext* context, base::WeakPtr<zebkit_view> only, bool restricted) {
  if (!Store().active || (restricted && !only)) return;
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) {
    auto view = request->view;
    if (!view || !view->web_contents() || (restricted && view.get() != only.get()) ||
        (context && view->web_contents()->GetBrowserContext() != context)) continue;
    ids.push_back(id);
  }
  for (auto id : ids) {
    auto* request = Find(id);
    if (!request) continue;
    if (!Live(*request)) Finish(id, Status::DENIED, false, "cancelled");
    else if (base::TimeTicks::Now() >= request->deadline) TimedOut(id);
    else if (auto status = FixedDecision(*request)) Finish(id, *status, false, "policy-changed");
  }
}
void ScheduleScan(content::BrowserContext* context, base::WeakPtr<zebkit_view> only = {}) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
      base::BindOnce(&Scan, context, only, static_cast<bool>(only)));
}
std::optional<Status> StatusHook(content::RenderFrameHost* frame, Type type,
    const url::Origin& requesting, const url::Origin& embedding) {
  if (!Store().active) return std::nullopt;
  return ViewRule(ViewOf(content::WebContents::FromRenderFrameHost(frame)), requesting, embedding, type);
}
std::optional<Status> DocumentStatus(content::RenderFrameHost* frame, Type, const GURL&, const GURL&) {
  auto* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  if (!Store().active || !view) return std::nullopt;
  auto* decider = view->Decider(&zebkit_view::on_permission_);
  if (!decider) return Status::DENIED;
  return decider->on_permission_ ? std::make_optional(Status::ASK) : std::nullopt;
}
bool Request(content::RenderFrameHost* frame, const content::PermissionRequestDescription& description,
             PermissionManager::RequestReply* reply) {
  auto* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  if (!Store().active || !view) return false;
  auto* decider = view->Decider(&zebkit_view::on_permission_);
  if (decider && !decider->on_permission_) return false;
  const size_t count = description.permissions.size();
  if (!count) { std::move(*reply).Run({}); return true; }
  // With an installed policy, resource exhaustion must not bypass a decision.
  size_t mine = 0;
  for (const auto& [id, request] : Store().pending) mine += request->view.get() == view;
  if (!decider || mine + count > kMaxRequests ||
      count >= std::numeric_limits<uintptr_t>::max() - Store().next) {
    std::move(*reply).Run(std::vector<content::PermissionResult>(count, content::PermissionResult(Status::DENIED)));
    return true;
  }
  auto group = std::make_shared<Group>();
  group->reply = std::move(*reply);
  group->results.assign(count, content::PermissionResult(Status::DENIED)); group->remaining = count;
  for (size_t index = 0; index < count; ++index) {
    const auto id = Store().next++;
    auto request = std::make_unique<Pending>();
    request->view = view->GetWeakPtr(); request->document = frame->GetWeakDocumentPtr(); request->frame = frame->GetGlobalId();
    request->group = group; request->index = index;
    request->deadline = base::TimeTicks::Now() + base::Milliseconds(view->permission_timeout_ms_);
    request->type = blink::PermissionDescriptorToPermissionType(description.permissions[index]);
    request->requesting = url::Origin::Create(description.requesting_origin);
    request->embedding = frame->GetMainFrame()->GetLastCommittedOrigin();
    request->metadata.Set("id", base::NumberToString(id));
    request->metadata.Set("permission", PermissionName(request->type));
    request->metadata.Set("requestingOrigin", request->requesting.Serialize());
    request->metadata.Set("topOrigin", request->embedding.Serialize());
    request->metadata.Set("userGesture", description.user_gesture);
    request->metadata.Set("timeoutMs", static_cast<int>(view->permission_timeout_ms_));
    request->metadata.Set("deadlineUnixMs", static_cast<double>((base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds()) + view->permission_timeout_ms_);
    request->metadata.Set("state", "pending");
    Store().pending.emplace(id, std::move(request));
    auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
    runner->PostTask(FROM_HERE, base::BindOnce(&Deliver, id));
    runner->PostDelayedTask(FROM_HERE, base::BindOnce(&TimedOut, id), base::Milliseconds(view->permission_timeout_ms_));
  }
  return true;
}
}  // namespace

bool IsNamedPermission(std::string_view name) { return NamedCode(name).has_value(); }

void RequestNamedPermission(content::RenderFrameHost* frame, std::string_view name,
                            base::DictValue details, bool user_gesture,
                            base::OnceCallback<void(NamedAnswer)> done) {
  auto code = NamedCode(name);
  auto* view = frame ? ViewOf(content::WebContents::FromRenderFrameHost(frame)) : nullptr;
  if (!code || !view || !Store().active || frame->GetLastCommittedOrigin().opaque()) {
    std::move(done).Run(NamedAnswer::kUndecided);
    return;
  }
  if (auto rule = NamedRule(view, frame->GetLastCommittedOrigin(),
                            frame->GetMainFrame()->GetLastCommittedOrigin(), *code);
      rule && *rule != Status::ASK) {
    std::move(done).Run(*rule == Status::GRANTED ? NamedAnswer::kGranted : NamedAnswer::kDenied);
    return;
  }
  auto* decider = view->Decider(&zebkit_view::on_permission_);
  if (!decider || !decider->on_permission_) {
    std::move(done).Run(NamedAnswer::kUndecided);
    return;
  }
  size_t mine = 0;
  for (const auto& [id, request] : Store().pending) mine += request->view.get() == view;
  if (mine >= kMaxRequests || Store().next == std::numeric_limits<uintptr_t>::max()) {
    std::move(done).Run(NamedAnswer::kUndecided);
    return;
  }
  const auto id = Store().next++;
  auto request = std::make_unique<Pending>();
  request->view = view->GetWeakPtr(); request->document = frame->GetWeakDocumentPtr(); request->frame = frame->GetGlobalId();
  request->named = *code; request->type = Type::NUM;
  request->done = base::BindOnce([](base::OnceCallback<void(NamedAnswer)> answer, Status status, bool decided) {
    std::move(answer).Run(!decided ? NamedAnswer::kUndecided
                          : status == Status::GRANTED ? NamedAnswer::kGranted : NamedAnswer::kDenied);
  }, std::move(done));
  request->deadline = base::TimeTicks::Now() + base::Milliseconds(view->permission_timeout_ms_);
  request->requesting = frame->GetLastCommittedOrigin();
  request->embedding = frame->GetMainFrame()->GetLastCommittedOrigin();
  request->metadata = std::move(details);
  request->metadata.Set("id", base::NumberToString(id));
  request->metadata.Set("permission", std::string(name));
  request->metadata.Set("requestingOrigin", request->requesting.Serialize());
  request->metadata.Set("topOrigin", request->embedding.Serialize());
  request->metadata.Set("userGesture", user_gesture);
  request->metadata.Set("timeoutMs", static_cast<int>(view->permission_timeout_ms_));
  request->metadata.Set("deadlineUnixMs", static_cast<double>((base::Time::Now() - base::Time::UnixEpoch()).InMilliseconds()) + view->permission_timeout_ms_);
  request->metadata.Set("state", "pending");
  const uint32_t timeout = view->permission_timeout_ms_;
  Store().pending.emplace(id, std::move(request));
  auto runner = base::SingleThreadTaskRunner::GetCurrentDefault();
  runner->PostTask(FROM_HERE, base::BindOnce(&Deliver, id));
  runner->PostDelayedTask(FROM_HERE, base::BindOnce(&TimedOut, id), base::Milliseconds(timeout));
}

void InstallPermissionHooks() {
  Store().active = true;
  content::SetEmbedderPermissionHooks(base::BindRepeating(&StatusHook), base::BindRepeating(
      [](content::BrowserContext* context, const std::optional<url::Origin>&,
         const std::optional<url::Origin>&, std::optional<Type>, bool committed) {
        if (committed) Scan(context, {}, false); else Expire(context);
      }));
  PermissionManager::SetDocumentPermissionHooks(base::BindRepeating(&DocumentStatus), base::BindRepeating(&Request));
}
void StopPermissionHooks() {
  Store().active = false;
  while (!Store().pending.empty()) Finish(Store().pending.begin()->first, Status::DENIED, false, "cancelled");
  PermissionManager::SetDocumentPermissionHooks({}, {});
  content::SetEmbedderPermissionHooks({}, {});
}
void CancelPermissionRequests(zebkit_view* view, const char* reason) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) if (request->view.get() == view) ids.push_back(id);
  for (auto id : ids) {
    auto* request = Find(id);
    if (!request) continue;
    const auto state = !strcmp(reason, "cancelled") || !Live(*request) ? Status::DENIED
        : FixedDecision(*request).value_or(DefaultReply(request->type));
    Finish(id, state, false, reason);
  }
}
void CheckPermissionDocuments(zebkit_view* view) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending) if (request->view.get() == view && !Live(*request)) ids.push_back(id);
  for (auto id : ids) Finish(id, Status::DENIED, false, "cancelled");
}
void PermissionFrameDeleted(zebkit_view* view, content::RenderFrameHost* frame) {
  std::vector<uintptr_t> ids;
  for (const auto& [id, request] : Store().pending)
    if (request->view.get() == view && request->frame == frame->GetGlobalId()) ids.push_back(id);
  for (auto id : ids) Finish(id, Status::DENIED, false, "cancelled");
}
}  // namespace zebkit

extern "C" {
bool zebkit_view_set_permission(zebkit_view* view, const char* origin, const char* permission,
                                zebkit_permission_state state, char** error) {
  using namespace zebkit;
  auto fail = [&](const std::string& message) { return zebkit::SetError(error, message); };
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents()) return fail("permission policy needs a live view on the engine thread");
  if (auto code = NamedCode(permission ? permission : "")) {
    if (!ValidState(state)) return fail("unknown permission or state");
    std::string key; std::optional<url::Origin> parsed;
    if (!Scope(origin, true, &key, &parsed)) return fail("permission origin must be a non-opaque URL or *");
    if (!view->permission_rules_.contains({key, *code}) && view->permission_rules_.size() >= kMaxRules)
      return fail("too many view permission rules");
    Expire(nullptr, view);
    WriteNamed(view, key, *code, ToStatus(state));
    ScheduleScan(nullptr, view->GetWeakPtr());
    return true;
  }
  auto type = PermissionTypeNamed(permission ? permission : "");
  if (!type || !ValidState(state)) return fail("unknown permission or state");
  if (*type == Type::GEOLOCATION_APPROXIMATE) return fail("use geolocation to set both location precisions; approximate-only decisions may be answered once");
  std::string key; std::optional<url::Origin> parsed;
  if (!Scope(origin, true, &key, &parsed)) return fail("permission origin must be a non-opaque URL or *");
  if (!CanRemember(view, key, *type, ToStatus(state))) return fail("too many view permission rules");
  Remember(view, key, *type, ToStatus(state));
  ScheduleScan(nullptr, view->GetWeakPtr());
  return true;
}
zebkit_permission_state zebkit_view_get_permission(zebkit_view* view, const char* origin, const char* permission) {
  using namespace zebkit;
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents()) return ZEBKIT_PERMISSION_INVALID;
  if (auto code = NamedCode(permission ? permission : "")) {
    std::string key; std::optional<url::Origin> parsed;
    auto* main = view->web_contents()->GetPrimaryMainFrame();
    if (!origin) {
      if (!main || main->GetLastCommittedOrigin().opaque()) return ZEBKIT_PERMISSION_INVALID;
      parsed = main->GetLastCommittedOrigin();
    } else if (!Scope(origin, false, &key, &parsed)) return ZEBKIT_PERMISSION_INVALID;
    auto embedding = main && !main->GetLastCommittedOrigin().opaque() ? main->GetLastCommittedOrigin() : *parsed;
    if (auto rule = NamedRule(view, *parsed, embedding, *code)) return ToState(*rule);
    auto* decider = view->Decider(&zebkit_view::on_permission_);
    return decider && decider->on_permission_ ? ZEBKIT_PERMISSION_PROMPT : ZEBKIT_PERMISSION_DENIED;
  }
  auto type = PermissionTypeNamed(permission ? permission : "");
  if (!type) return ZEBKIT_PERMISSION_INVALID;
  std::string key; std::optional<url::Origin> parsed;
  auto* frame = view->web_contents()->GetPrimaryMainFrame();
  if (!origin) {
    if (!frame || frame->GetLastCommittedOrigin().opaque()) return ZEBKIT_PERMISSION_INVALID;
    parsed = frame->GetLastCommittedOrigin();
  } else if (!Scope(origin, false, &key, &parsed)) return ZEBKIT_PERMISSION_INVALID;
  auto embedding = frame && !frame->GetLastCommittedOrigin().opaque() ? frame->GetLastCommittedOrigin() : *parsed;
  std::optional<Status> status = ViewRule(view, *parsed, embedding, *type);
  if (!status) status = content::GetEmbedderContextPermissionOverride(
      view->web_contents()->GetBrowserContext(), *parsed, embedding, *type);
  if (status) return ToState(content::CreateEmbedderPermissionResult(*type, *status, true).status);
  auto* decider = view->Decider(&zebkit_view::on_permission_);
  if (!decider) return ZEBKIT_PERMISSION_DENIED;
  if (decider->on_permission_) return ZEBKIT_PERMISSION_PROMPT;
  auto descriptor = content::PermissionDescriptorUtil::CreatePermissionDescriptorForPermissionType(*type);
  if (!descriptor) return ZEBKIT_PERMISSION_INVALID;
  return ToState(view->web_contents()->GetBrowserContext()->GetPermissionController()
      ->GetPermissionResultForOriginWithoutContext(descriptor, *parsed, embedding).status);
}
bool zebkit_view_reset_permissions(zebkit_view* view, const char* origin, char** error) {
  using namespace zebkit;
  auto fail = [&](const char* message) { return zebkit::SetError(error, message); };
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents()) return fail("permission policy needs a live view on the engine thread");
  std::optional<std::string> key;
  if (origin) {
    std::string parsed_key; std::optional<url::Origin> parsed;
    if (!Scope(origin, true, &parsed_key, &parsed)) return fail("permission origin must be a non-opaque URL or *");
    key = std::move(parsed_key);
  }
  Expire(nullptr, view);
  auto weak = view->GetWeakPtr();
  content::UpdateEmbedderPermissionState(view->web_contents()->GetBrowserContext(),
      base::BindOnce([](base::WeakPtr<zebkit_view> target, std::optional<std::string> scope) {
        if (!target) return;
        if (!scope) target->permission_rules_.clear();
        else for (auto it = target->permission_rules_.begin(); it != target->permission_rules_.end();) {
          if (it->first.first == *scope || it->first.first.starts_with(*scope + " ")) it = target->permission_rules_.erase(it); else ++it;
        }
      }, weak, key));
  if (weak) Scan(nullptr, weak, true);
  return true;
}
bool zebkit_view_set_permission_timeout(zebkit_view* view, uint32_t milliseconds) {
  using namespace zebkit;
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents() || !milliseconds || milliseconds > 300000) return false;
  view->permission_timeout_ms_ = milliseconds;
  return true;
}
void zebkit_view_on_permission(zebkit_view* view, zebkit_permission_func function, void* data) {
  using namespace zebkit;
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents()) return;
  if (view->on_permission_ == function && view->on_permission_data_ == data) return;
  Expire(nullptr, view);
  auto weak = view->GetWeakPtr();
  content::UpdateEmbedderPermissionState(view->web_contents()->GetBrowserContext(),
      base::BindOnce([](base::WeakPtr<zebkit_view> target, zebkit_permission_func callback, void* payload) {
        if (!target) return;
        CancelPermissionRequests(target.get(), "handler-changed");
        if (!target) return;
        target->on_permission_ = callback; target->on_permission_data_ = payload;
      }, weak, function, data));
}
void zebkit_view_on_permission_result(zebkit_view* view, zebkit_permission_result_func function, void* data) {
  using namespace zebkit;
  if (!Ready() || !zebkit::LiveView(view) || !view->web_contents()) return;
  view->on_permission_result_ = function; view->on_permission_result_data_ = data;
}
char* zebkit_permission_request_info(zebkit_permission_request* handle) {
  using namespace zebkit;
  if (!Ready()) return nullptr;
  const auto id = reinterpret_cast<uintptr_t>(handle);
  auto* request = Find(id);
  if (!request) return nullptr;
  if (!Live(*request)) { Finish(id, Status::DENIED, false, "cancelled"); return nullptr; }
  if (base::TimeTicks::Now() >= request->deadline) { TimedOut(id); return nullptr; }
  if (auto status = FixedDecision(*request)) { Finish(id, *status, false, "policy-changed"); return nullptr; }
  return strdup(base::WriteJson(request->metadata).value_or("{}").c_str());
}
bool zebkit_permission_respond(zebkit_permission_request* handle, zebkit_permission_state state, bool remember) {
  using namespace zebkit;
  if (!Ready() || !ValidState(state)) return false;
  const auto id = reinterpret_cast<uintptr_t>(handle);
  auto* request = Find(id);
  if (!request) return false;
  if (!Live(*request)) { Finish(id, Status::DENIED, false, "cancelled"); return false; }
  if (base::TimeTicks::Now() >= request->deadline) { TimedOut(id); return false; }
  if (auto status = FixedDecision(*request)) { Finish(id, *status, false, "policy-changed"); return false; }
  auto view = request->view;
  if (remember && (request->requesting.opaque() ||
      (request->named ? view->permission_rules_.size() >= kMaxRules
                      : !CanRemember(view.get(), RememberKey(request->requesting, request->embedding), request->type, ToStatus(state))))) return false;
  Finish(id, ToStatus(state), remember, "answered");
  if (remember && view && view->web_contents()) ScheduleScan(nullptr, view);
  return true;
}
}
