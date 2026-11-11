// Network requests and responses (<zebkit/hooks.h>): a URL loader factory in
// front of the network's, as extensions' webRequest and DevTools' Fetch domain
// put one. Requests a rule holds wait for the application, up to a deadline;
// those it observes are told and do not wait; the rest pass. A frame's WebSocket
// handshake goes through the same rules, and a view can take on the workers of its
// profile, which have no view. Rules of the network's stages also take the network's
// header client, as extensions' webRequest and DevTools' Fetch do, to see what it adds.

#include "zebkit/engine/network_hooks.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/byte_size.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/global_request_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/zebkit_automation.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/unique_receiver_set.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/file_data_source.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/ip_endpoint.h"
#include "net/base/mime_sniffer.h"
#include "net/base/net_errors.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/redirect_util.h"
#include "services/network/public/cpp/header_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/http_raw_headers.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"
#include "zebkit/engine/automation_network.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/hook_requests.h"

namespace zebkit {

struct NetworkRule {
  std::unique_ptr<RE2> url;  // Null: every URL.
  bool hold = true;
  bool request_stage = true;
  bool body = false;  // Told as it passes; holds nothing.
  const char* network_stage = nullptr;  // One of the network's stages; null: the page's.
  std::set<std::string> resource_types;  // Empty: every type.
  enum class Frames { kAll, kMain, kSub } frames = Frames::kAll;
};
struct NetworkRules {
  std::vector<NetworkRule> rules;
};

namespace {

constexpr size_t kBodyLimit = 64 * 1024 * 1024;
constexpr int kRedirectLimit = 20;
constexpr size_t kPumpChunk = 64 * 1024;
constexpr int kPumpBurst = 16;
constexpr uint32_t kPipeSize = 512 * 1024;
constexpr size_t kStreamBuffer = 1024 * 1024;
constexpr char kWebSocket[] = "websocket";
// The stages inside the network, with the headers it adds; "request" and "response"
// are outside it, with the page's.
constexpr char kSendHeaders[] = "sendHeaders";
constexpr char kSent[] = "sent";
constexpr char kHeadersReceived[] = "headersReceived";

bool IsNetworkStage(const std::string* stage) {
  return stage && (*stage == kSendHeaders || *stage == kSent || *stage == kHeadersReceived);
}

enum class Action { kContinue, kBlock, kRedirect, kFulfill, kFile, kStream };
enum class Worker { kNone, kDedicated, kShared, kService };

struct Traits {
  struct Decision {
    Action action = Action::kContinue;
    std::optional<net::HttpRequestHeaders> request_headers;
    std::optional<std::vector<std::pair<std::string, std::string>>> response_headers;
    int error = net::ERR_BLOCKED_BY_CLIENT;
    GURL url;
    int status = 200;
    std::string body;
    base::FilePath file;
    uintptr_t stream = 0;
    const char* state = "continued";
  };
  using Request = zebkit_network_request;
  static constexpr auto kHandler = &zebkit_view::on_network_;
  static constexpr auto kData = &zebkit_view::on_network_data_;
  static constexpr auto kResult = &zebkit_view::on_network_event_;
  static constexpr auto kResultData = &zebkit_view::on_network_event_data_;
  static constexpr auto kGeneration = &zebkit_view::network_generation_;
  static constexpr auto kTimeout = &zebkit_view::network_timeout_ms_;
  // The view has gone: its page's loader is ended.
  static Decision Refusal() {
    Decision decision;
    decision.action = Action::kBlock;
    decision.error = net::ERR_ABORTED;
    decision.state = "cancelled";
    return decision;
  }
  // A deadline passed or a function replaced: the request goes on.
  static Decision Unanswered() { return {}; }
  // Except inside the network: what a rule there is for must not be got past by
  // saying nothing. The outcome told says so.
  static void Settling(const base::DictValue& metadata, Decision& decision) {
    if (IsNetworkStage(metadata.FindString("stage")) && !strcmp(decision.state, "continued")) {
      decision.action = Action::kBlock;
      decision.state = "refused";
    }
  }
  static const char* State(const Decision& decision) { return decision.state; }
};
using Requests = HookRequests<Traits>;

class Loader;

// The network's header client of one factory. It stays as long as the network's
// factory does: a redirect's preflight can come after the page's factory has gone.
class HeaderClient : public network::mojom::TrustedURLLoaderHeaderClient {
 public:
  HeaderClient(base::WeakPtr<zebkit_view> view, content::BrowserContext* profile, std::string frame, bool main_frame,
               Worker worker, mojo::PendingReceiver<network::mojom::TrustedURLLoaderHeaderClient> receiver)
      : view_(std::move(view)), profile_(profile), frame_(std::move(frame)), main_frame_(main_frame), worker_(worker),
        receiver_(this, std::move(receiver)) {}

  void Add(int32_t id, Loader* loader) { loaders_[id] = loader; }
  void Remove(int32_t id) { loaders_.erase(id); }

  // network::mojom::TrustedURLLoaderHeaderClient
  void OnLoaderCreated(int32_t request_id, mojo::PendingReceiver<network::mojom::TrustedHeaderClient> client) override;
  void OnLoaderForCorsPreflightCreated(const network::ResourceRequest& request,
                                       mojo::PendingReceiver<network::mojom::TrustedHeaderClient> client) override;

 private:
  base::WeakPtr<zebkit_view> view_;
  content::BrowserContext* profile_;
  std::string frame_;
  bool main_frame_;
  Worker worker_;
  std::map<int32_t, Loader*> loaders_;
  mojo::UniqueReceiverSet<network::mojom::TrustedHeaderClient> others_;
  mojo::Receiver<network::mojom::TrustedURLLoaderHeaderClient> receiver_;
};

struct Target : base::RefCounted<Target> {
  mojo::Remote<network::mojom::URLLoaderFactory> factory;
  std::unique_ptr<HeaderClient> header_client;  // Null: this factory cannot have one.
 private:
  friend class base::RefCounted<Target>;
  ~Target() = default;
};

base::ListValue RequestHeaderList(const net::HttpRequestHeaders& headers) {
  base::ListValue list;
  for (const auto& header : headers.GetHeaderVector()) {
    base::DictValue entry;
    entry.Set("name", header.key);
    entry.Set("value", header.value);
    list.Append(std::move(entry));
  }
  return list;
}

base::ListValue ResponseHeaderList(const net::HttpResponseHeaders* headers) {
  base::ListValue list;
  if (!headers)
    return list;
  size_t iterator = 0, bytes = 0;
  std::string name, value;
  while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
    if (list.size() >= 256 || bytes + name.size() + value.size() > 65536)
      break;
    bytes += name.size() + value.size();
    base::DictValue entry;
    entry.Set("name", name);
    entry.Set("value", value);
    list.Append(std::move(entry));
  }
  return list;
}

const char* WorkerName(Worker worker) {
  switch (worker) {
    case Worker::kDedicated: return "dedicated";
    case Worker::kShared: return "shared";
    case Worker::kService: return "service";
    case Worker::kNone: break;
  }
  return "";
}

std::string NextRequestId() {
  static uint64_t next = 0;
  return "n" + base::NumberToString(++next);
}

// The view that decides for a profile's shared and service workers. An entry
// whose view has gone is that profile's refusal, until a view takes over or says
// off: a policy once set is not lost by closing what held it. The key is compared
// and never followed.
std::map<content::BrowserContext*, base::WeakPtr<zebkit_view>>& WorkerPolicies() {
  static base::NoDestructor<std::map<content::BrowserContext*, base::WeakPtr<zebkit_view>>> policies;
  return *policies;
}

bool RuleApplies(const NetworkRule& rule, const std::string& type, const GURL& url, bool main_frame) {
  if (rule.frames == NetworkRule::Frames::kMain && !main_frame) return false;
  if (rule.frames == NetworkRule::Frames::kSub && main_frame) return false;
  if (!rule.resource_types.empty() && !rule.resource_types.contains(type)) return false;
  return !rule.url || RE2::FullMatch(url.spec(), *rule.url);
}

bool RulesMatch(zebkit_view* decider, const std::string& type, const GURL& url, bool main_frame,
                bool request_stage, bool hold, bool body) {
  if (!decider->network_rules_)
    return false;
  for (const NetworkRule& rule : decider->network_rules_->rules) {
    if (rule.network_stage || rule.hold != hold || rule.request_stage != request_stage) continue;
    if (body && !rule.body) continue;
    if (RuleApplies(rule, type, url, main_frame))
      return true;
  }
  return false;
}

bool NetworkStageMatches(zebkit_view* decider, const char* stage, const std::string& type, const GURL& url,
                         bool main_frame, bool hold) {
  if (!decider->network_rules_)
    return false;
  for (const NetworkRule& rule : decider->network_rules_->rules) {
    if (!rule.network_stage || strcmp(rule.network_stage, stage) || rule.hold != hold) continue;
    if (RuleApplies(rule, type, url, main_frame))
      return true;
  }
  return false;
}

// The view whose filter a request answers to. Shared and service workers have
// none of their own: a profile's policy, once set, stays, and without its view
// they are refused.
zebkit_view* PolicyViewFor(const base::WeakPtr<zebkit_view>& view, content::BrowserContext* profile, Worker worker,
                           bool* refused) {
  if (worker == Worker::kShared || worker == Worker::kService) {
    auto it = WorkerPolicies().find(profile);
    if (it != WorkerPolicies().end()) {
      *refused = !it->second;
      return it->second.get();
    }
  }
  return view.get();
}

std::string RawResponseHeaders(const std::string& status_line,
                               const std::vector<std::pair<std::string, std::string>>& headers) {
  std::string raw = status_line + "\r\n";
  for (const auto& [name, value] : headers)
    raw += name + ": " + value + "\r\n";
  return net::HttpUtil::AssembleRawHeaders(raw + "\r\n");
}

// The view whose on_network_event function hears of @view's requests: its
// own, or that of the opener a popup inherits from; null when none.
zebkit_view* EventTeller(zebkit_view* view) {
  zebkit_view* teller = view ? view->Decider(&zebkit_view::on_network_event_) : nullptr;
  return teller && teller->on_network_event_ ? teller : nullptr;
}

// Told, without waiting. Bounded, and what was dropped is counted.
void PostEvent(zebkit_view* view, base::DictValue event) {
  zebkit_view* teller = EventTeller(view);
  if (!teller)
    return;
  if (view->network_events_pending_ >= 4096) {
    ++view->network_events_dropped_;
    return;
  }
  ++view->network_events_pending_;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<zebkit_view> view, base::WeakPtr<zebkit_view> teller, uint64_t generation,
             base::DictValue event) {
            if (!view)
              return;
            --view->network_events_pending_;
            if (!teller || teller->network_generation_ != generation || !teller->on_network_event_)
              return;
            event.Set("dropped", base::NumberToString(view->network_events_dropped_));
            view->network_events_dropped_ = 0;
            const std::string json = base::WriteJson(event).value_or("{}");
            auto function = teller->on_network_event_;
            function(view.get(), json.c_str(), teller->on_network_event_data_);
          },
          view->GetWeakPtr(), teller->GetWeakPtr(), teller->network_generation_, std::move(event)));
}

// A response's body on its way to the page. The next chunk is read only when the
// last one is written, so that a page that does not read keeps nothing here and
// the network waits as it would without this.
class BodyPump {
 public:
  using Chunk = base::RepeatingCallback<void(base::span<const uint8_t>)>;

  BodyPump(mojo::ScopedDataPipeConsumerHandle from, mojo::ScopedDataPipeProducerHandle to, Chunk chunk,
           base::OnceClosure done)
      : from_(std::move(from)), to_(std::move(to)), buffer_(kPumpChunk),
        from_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        to_watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL),
        chunk_(std::move(chunk)), done_(std::move(done)) {
    from_watcher_.Watch(from_.get(), MOJO_HANDLE_SIGNAL_READABLE,
                        base::BindRepeating(&BodyPump::Ready, base::Unretained(this)));
    to_watcher_.Watch(to_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
                      base::BindRepeating(&BodyPump::Ready, base::Unretained(this)));
    from_watcher_.ArmOrNotify();
  }

 private:
  void Ready(MojoResult result) {
    if (result != MOJO_RESULT_CANCELLED)
      Pump();
  }

  void Pump() {
    base::WeakPtr<BodyPump> alive = weak_factory_.GetWeakPtr();
    for (int burst = 0;; ++burst) {
      while (offset_ < size_) {
        size_t written = 0;
        const MojoResult result = to_->WriteData(base::span(buffer_).subspan(offset_, size_ - offset_),
                                                 MOJO_WRITE_DATA_FLAG_NONE, written);
        if (result == MOJO_RESULT_SHOULD_WAIT) {
          to_watcher_.ArmOrNotify();
          return;
        }
        if (result != MOJO_RESULT_OK) {
          // The page stopped reading: so does the network, and it says how that ended.
          End();
          return;
        }
        offset_ += written;
      }
      offset_ = size_ = 0;
      // Not all at once on the engine's thread.
      if (burst >= kPumpBurst) {
        from_watcher_.ArmOrNotify();
        return;
      }
      size_t read = 0;
      const MojoResult result = from_->ReadData(MOJO_READ_DATA_FLAG_NONE, base::span(buffer_), read);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        from_watcher_.ArmOrNotify();
        return;
      }
      if (result != MOJO_RESULT_OK) {
        End();
        return;
      }
      size_ = read;
      chunk_.Run(base::span(buffer_).first(read));
      if (!alive)
        return;
    }
  }

  void End() {
    from_watcher_.Cancel();
    to_watcher_.Cancel();
    from_.reset();
    to_.reset();
    std::move(done_).Run();
  }

  mojo::ScopedDataPipeConsumerHandle from_;
  mojo::ScopedDataPipeProducerHandle to_;
  std::vector<uint8_t> buffer_;
  size_t offset_ = 0;
  size_t size_ = 0;
  mojo::SimpleWatcher from_watcher_;
  mojo::SimpleWatcher to_watcher_;
  Chunk chunk_;
  base::OnceClosure done_;
  base::WeakPtrFactory<BodyPump> weak_factory_{this};
};

class AnswerStream;
// zebkit_network_stream: what the application holds until it ends the stream,
// which may be after the page has gone. Numbers are not reused.
struct StreamEntry {
  base::WeakPtr<AnswerStream> stream;
  zebkit_network_stream_writable_func writable = nullptr;
  void* data = nullptr;
  bool missed = false;
};
std::map<uintptr_t, StreamEntry>& Streams() {
  static base::NoDestructor<std::map<uintptr_t, StreamEntry>> streams;
  return *streams;
}
void StreamWritable(uintptr_t token) {
  auto it = Streams().find(token);
  if (it == Streams().end())
    return;
  if (!it->second.writable) {
    it->second.missed = true;
    return;
  }
  auto writable = it->second.writable;
  writable(reinterpret_cast<zebkit_network_stream*>(token), it->second.data);
}

// An answer the application writes as it goes. The page's end of the pipe goes
// when it stops reading; what waits here is bounded.
class AnswerStream {
 public:
  using Ended = base::OnceCallback<void(bool failed, size_t bytes)>;

  AnswerStream(uintptr_t token, mojo::ScopedDataPipeProducerHandle to, Ended ended)
      : token_(token), to_(std::move(to)),
        watcher_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL), ended_(std::move(ended)) {
    watcher_.Watch(to_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
                   base::BindRepeating(&AnswerStream::Ready, base::Unretained(this)));
  }
  // Whoever waits to write learns that it never will.
  ~AnswerStream() {
    if (wanted_) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                                  base::BindOnce(&StreamWritable, token_));
    }
  }

  base::WeakPtr<AnswerStream> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  bool open() const { return to_.is_valid() && !ending_; }

  bool Write(base::span<const uint8_t> bytes) {
    if (!open())
      return false;
    if (bytes.size() > kStreamBuffer || Waiting() + bytes.size() > kStreamBuffer) {
      wanted_ = bytes.size() <= kStreamBuffer ? std::max<size_t>(1, bytes.size()) : 0;
      return false;
    }
    if (offset_ && offset_ >= pending_.size() / 2) {
      pending_.erase(0, offset_);
      offset_ = 0;
    }
    pending_.append(base::as_string_view(bytes));
    written_ += bytes.size();
    Flush();
    return true;
  }

  void End(bool failed) {
    if (!to_.is_valid() || ending_)
      return;
    ending_ = true;
    if (failed) {
      pending_.clear();
      offset_ = 0;
      Close(true);
      return;
    }
    Flush();
  }

 private:
  size_t Waiting() const { return pending_.size() - offset_; }

  void Ready(MojoResult result) {
    if (result != MOJO_RESULT_CANCELLED)
      Flush();
  }

  void Flush() {
    while (to_.is_valid() && Waiting()) {
      size_t written = 0;
      const MojoResult result =
          to_->WriteData(base::as_byte_span(pending_).subspan(offset_), MOJO_WRITE_DATA_FLAG_NONE, written);
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        watcher_.ArmOrNotify();
        break;
      }
      if (result != MOJO_RESULT_OK) {
        // The page stopped reading. Its loader says the rest, or has gone.
        pending_.clear();
        offset_ = 0;
        Close(true);
        break;
      }
      offset_ += written;
    }
    if (to_.is_valid() && ending_ && !Waiting()) {
      Close(false);
      return;
    }
    if (wanted_ && (!to_.is_valid() || Waiting() + wanted_ <= kStreamBuffer)) {
      wanted_ = 0;
      // After what is on the stack here: the function may write again.
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE,
                                                                  base::BindOnce(&StreamWritable, token_));
    }
  }

  void Close(bool failed) {
    watcher_.Cancel();
    to_.reset();
    if (ended_)
      std::move(ended_).Run(failed, written_);
  }

  uintptr_t token_;
  mojo::ScopedDataPipeProducerHandle to_;
  mojo::SimpleWatcher watcher_;
  Ended ended_;
  std::string pending_;
  size_t offset_ = 0;
  size_t written_ = 0;
  size_t wanted_ = 0;
  bool ending_ = false;
  base::WeakPtrFactory<AnswerStream> weak_factory_{this};
};

// What an answer from a file needs of it, off the engine's thread.
struct OpenedFile {
  base::File file;
  int64_t size = 0;
  std::string start;
};
OpenedFile OpenAnswerFile(const base::FilePath& path) {
  OpenedFile opened;
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  base::File::Info info;
  if (!file.IsValid() || !file.GetInfo(&info) || info.is_directory)
    return opened;
  opened.size = info.size;
  opened.start.resize(static_cast<size_t>(std::min<int64_t>(info.size, net::kMaxBytesToSniff)));
  const std::optional<size_t> read = file.Read(0, base::as_writable_byte_span(opened.start));
  opened.start.resize(read.value_or(0));
  opened.file = std::move(file);
  return opened;
}

// The network's stages of one network loader or preflight, every hop of it. What a
// hold rule is for fails closed here: no function, no policy, too many held, no answer in
// time, each ends the request. It goes with its pipe, and what it holds with it.
class HeaderWatch : public network::mojom::TrustedHeaderClient {
 public:
  using Describe = base::RepeatingCallback<base::DictValue(const char*)>;

  HeaderWatch(base::WeakPtr<zebkit_view> view, content::BrowserContext* profile, Worker worker, std::string type,
              bool main_frame, Describe describe)
      : had_view_(!!view), view_(std::move(view)), profile_(profile), worker_(worker), type_(std::move(type)),
        main_frame_(main_frame), describe_(std::move(describe)) {}
  ~HeaderWatch() override { Requests::Get().Abandon(held_); }

  // network::mojom::TrustedHeaderClient
  void OnBeforeSendHeaders(const GURL& url, const net::HttpRequestHeaders& headers,
                           OnBeforeSendHeadersCallback callback) override {
    url_ = url;
    base::DictValue description = describe_.Run(kSendHeaders);
    description.Set("url", url.spec());
    description.Set("headers", RequestHeaderList(headers));
    Tell(kSendHeaders, description);
    send_ = std::move(callback);
    Requests::Reply reply = base::BindOnce(&HeaderWatch::SendDecided, weak_factory_.GetWeakPtr());
    if (!Hold(kSendHeaders, std::move(description), reply) && reply)
      std::move(reply).Run(Pass());
  }
  void OnHeadersReceived(const std::string& headers, const net::IPEndPoint& endpoint,
                         const std::optional<net::SSLInfo>& ssl_info, OnHeadersReceivedCallback callback) override {
    auto parsed = base::MakeRefCounted<net::HttpResponseHeaders>(headers);
    status_line_ = parsed->GetStatusLine();
    base::DictValue description = describe_.Run(kHeadersReceived);
    description.Set("url", url_.spec());
    description.Set("status", parsed->response_code());
    description.Set("statusText", parsed->GetStatusText());
    description.Set("responseHeaders", ResponseHeaderList(parsed.get()));
    // The address the response came from, after DNS: a host's rules can see a
    // public name that resolved to a private address.
    if (endpoint.address().IsValid())
      description.Set("remoteAddress", endpoint.ToStringWithoutPort());
    Tell(kHeadersReceived, description);
    received_ = std::move(callback);
    Requests::Reply reply = base::BindOnce(&HeaderWatch::ReceivedDecided, weak_factory_.GetWeakPtr());
    if (!Hold(kHeadersReceived, std::move(description), reply) && reply)
      std::move(reply).Run(Pass());
  }
  void OnRequestHeadersSent(std::vector<network::mojom::HttpRawHeaderPairPtr> headers) override {
    base::DictValue description = describe_.Run(kSent);
    base::ListValue list;
    for (const auto& header : headers) {
      base::DictValue entry;
      entry.Set("name", header->key);
      entry.Set("value", header->value);
      list.Append(std::move(entry));
    }
    description.Set("url", url_.spec());
    description.Set("headers", std::move(list));
    Tell(kSent, description);
  }

 private:
  static Traits::Decision Pass() {
    Traits::Decision decision;
    decision.state = "answered";
    return decision;
  }
  static Traits::Decision Refuse() {
    Traits::Decision decision;
    decision.action = Action::kBlock;
    decision.state = "refused";
    return decision;
  }

  void Tell(const char* stage, const base::DictValue& description) {
    bool refused = false;
    zebkit_view* view = PolicyViewFor(view_, profile_, worker_, &refused);
    zebkit_view* decider = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    if (!decider || !EventTeller(view) || !NetworkStageMatches(decider, stage, type_, url_, main_frame_, false))
      return;
    base::DictValue event = description.Clone();
    event.Set("kind", stage);
    PostEvent(view, std::move(event));
  }

  // True when @reply has been given to the application, or to a refusal; false when
  // no rule holds this.
  bool Hold(const char* stage, base::DictValue description, Requests::Reply& reply) {
    bool refused = false;
    zebkit_view* view = PolicyViewFor(view_, profile_, worker_, &refused);
    // A view that closed, or a popup whose opener did, has no policy left.
    zebkit_view* decider = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    if (refused || (!view && had_view_) || (view && !decider)) {
      std::move(reply).Run(Refuse());
      return true;
    }
    if (!decider || !NetworkStageMatches(decider, stage, type_, url_, main_frame_, true))
      return false;
    auto& requests = Requests::Get();
    if (!Requests::HasDecider(view) || !requests.Ask(view, std::move(description), reply)) {
      std::move(reply).Run(Refuse());
      return true;
    }
    held_ = requests.last_id();
    return true;
  }

  void SendDecided(Traits::Decision decision) {
    held_ = 0;
    auto callback = std::move(send_);
    if (decision.action != Action::kContinue)
      std::move(callback).Run(decision.error, std::nullopt, std::nullopt);
    else
      std::move(callback).Run(net::OK, decision.request_headers, std::nullopt);
  }

  void ReceivedDecided(Traits::Decision decision) {
    held_ = 0;
    auto callback = std::move(received_);
    if (decision.action != Action::kContinue) {
      std::move(callback).Run(decision.error, std::nullopt, std::nullopt);
      return;
    }
    std::optional<std::string> headers;
    if (decision.response_headers)
      headers = RawResponseHeaders(status_line_, *decision.response_headers);
    std::move(callback).Run(net::OK, headers, std::nullopt);
  }

  const bool had_view_;
  base::WeakPtr<zebkit_view> view_;
  content::BrowserContext* profile_;
  Worker worker_;
  std::string type_;
  bool main_frame_;
  Describe describe_;
  GURL url_;
  std::string status_line_;
  OnBeforeSendHeadersCallback send_;
  OnHeadersReceivedCallback received_;
  uintptr_t held_ = 0;
  base::WeakPtrFactory<HeaderWatch> weak_factory_{this};
};

// A loader or preflight no rule of the network's stages is for, in a factory that has a
// header client: another proxy's flag asks for one. As DevTools' NoOpHeaderClient.
class HeaderPass : public network::mojom::TrustedHeaderClient {
 public:
  void OnBeforeSendHeaders(const GURL&, const net::HttpRequestHeaders&, OnBeforeSendHeadersCallback callback) override {
    std::move(callback).Run(net::OK, std::nullopt, std::nullopt);
  }
  void OnHeadersReceived(const std::string&, const net::IPEndPoint&, const std::optional<net::SSLInfo>&,
                         OnHeadersReceivedCallback callback) override {
    std::move(callback).Run(net::OK, std::nullopt, std::nullopt);
  }
  void OnRequestHeadersSent(std::vector<network::mojom::HttpRawHeaderPairPtr>) override {}
};

std::set<Loader*>& Loaders() {
  static base::NoDestructor<std::set<Loader*>> loaders;
  return *loaders;
}
class Factory;
std::set<Factory*>& Factories() {
  static base::NoDestructor<std::set<Factory*>> factories;
  return *factories;
}

// One request of the page, between its loader and the network's. Lives until
// the page's end goes.
class Loader : public network::mojom::URLLoader,
               public network::mojom::URLLoaderClient {
 public:
  Loader(base::WeakPtr<zebkit_view> view, content::BrowserContext* profile, std::string frame, bool main_frame,
         Worker worker, scoped_refptr<Target> target,
         mojo::PendingReceiver<network::mojom::URLLoader> loader, int32_t request_id, uint32_t options,
         const network::ResourceRequest& request,
         mojo::PendingRemote<network::mojom::URLLoaderClient> client,
         const net::MutableNetworkTrafficAnnotationTag& annotation)
      : view_(std::move(view)), profile_(profile), id_(NextRequestId()), frame_(std::move(frame)),
        main_frame_(main_frame), worker_(worker),
        target_(std::move(target)), request_id_(request_id), options_(options), request_(request),
        annotation_(annotation), receiver_(this, std::move(loader)), client_(std::move(client)) {
    Loaders().insert(this);
    receiver_.set_disconnect_handler(base::BindOnce(&Loader::Gone, base::Unretained(this)));
    client_.set_disconnect_handler(base::BindOnce(&Loader::Gone, base::Unretained(this)));
  }
  ~Loader() override {
    Loaders().erase(this);
    Requests::Get().Abandon(held_);
    ForgetRequestId();
  }

  void ForgetRequestId() {
    if (network_request_id_ && target_->header_client)
      target_->header_client->Remove(*network_request_id_);
    network_request_id_.reset();
  }

  void Start() { AskAtRequest(); }

  // The network's loader for this request has started.
  void WatchHeaders(mojo::PendingReceiver<network::mojom::TrustedHeaderClient> client) {
    // A second loader for the same request, as CORS restarts one, takes over.
    header_watch_.Clear();
    header_watch_.Add(std::make_unique<HeaderWatch>(
                          view_, profile_, worker_, NetworkResourceType(request_), main_frame_,
                          base::BindRepeating(&Loader::DescribeNetworkStage, base::Unretained(this))),
                      std::move(client));
  }

  // network::mojom::URLLoader
  void FollowRedirect(network::HttpRequestHeadersUpdateParams params,
                      const std::optional<GURL>& new_url) override {
    if (!redirect_)
      return;
    const net::RedirectInfo info = *redirect_;
    redirect_.reset();
    bool clear_body = false;
    net::RedirectUtil::UpdateHttpRequest(request_.url, request_.method, info, params.removed_headers,
                                         params.modified_headers, &request_.headers, &clear_body);
    request_.cors_exempt_headers.MergeFrom(params.modified_cors_exempt_headers);
    for (const std::string& name : params.removed_headers)
      request_.cors_exempt_headers.RemoveHeader(name);
    if (clear_body)
      request_.request_body = nullptr;
    request_.UpdateOnRedirect(info);
    // The network has not started on what the application itself redirected.
    if (synthetic_redirect_) {
      synthetic_redirect_ = false;
      AskAtRequest();
      return;
    }
    follow_ = std::move(params);
    follow_url_ = new_url;
    AskAtRequest();
  }
  void SetPriority(net::RequestPriority priority, int32_t value) override {
    if (network_)
      network_->SetPriority(priority, value);
  }

  // network::mojom::URLLoaderClient
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {
    client_->OnReceiveEarlyHints(std::move(hints));
  }
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr head, mojo::ScopedDataPipeConsumerHandle body,
                         std::optional<mojo_base::BigBuffer> metadata) override {
    Observe("response", head.get(), false, net::OK);
    const Hold hold = Stage(false);
    if (hold == Hold::kNone) {
      Respond(std::move(head), std::move(body), std::move(metadata));
      return;
    }
    // A rule holds it and it cannot be held, or no policy is left: not past either.
    if (hold == Hold::kBlocked || !Ask("response", head.get())) {
      Drop();
      Finish(network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
      return;
    }
    head_ = std::move(head);
    body_ = std::move(body);
    metadata_ = std::move(metadata);
  }
  void OnReceiveRedirect(const net::RedirectInfo& redirect, network::mojom::URLResponseHeadPtr head) override {
    Observe("redirect", head.get(), true, net::OK);
    redirect_ = redirect;
    client_->OnReceiveRedirect(redirect, std::move(head));
  }
  void OnUploadProgress(int64_t position, int64_t total, OnUploadProgressCallback callback) override {
    client_->OnUploadProgress(position, total, std::move(callback));
  }
  void OnTransferSizeUpdated(int32_t diff) override {
    // Not before the response it belongs to.
    if (head_)
      transfer_ += diff;
    else
      client_->OnTransferSizeUpdated(diff);
  }
  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    // After what is held, and after the last of the body is told.
    if (head_ || pump_) {
      completion_ = status;
      return;
    }
    TellBodyEnd();
    Finish(status);
  }

  void Decided(const char* stage, Traits::Decision decision) {
    held_ = 0;
    const bool response = !strcmp(stage, "response");
    switch (decision.action) {
      case Action::kBlock:
        Drop();
        Finish(network::URLLoaderCompletionStatus(decision.error));
        return;
      case Action::kRedirect:
        Drop();
        Redirect(decision.url);
        return;
      case Action::kFulfill:
        Drop();
        Fulfill(decision.status, decision.response_headers, std::move(decision.body));
        return;
      case Action::kFile:
        Drop();
        FulfillFile(decision.status, std::move(decision.response_headers), std::move(decision.file));
        return;
      case Action::kStream:
        Drop();
        FulfillStream(decision.status, decision.response_headers, decision.stream);
        return;
      case Action::kContinue:
        break;
    }
    if (response) {
      if (decision.response_headers)
        ReplaceResponseHeaders(*decision.response_headers);
      Respond(std::move(head_), std::move(body_), std::move(metadata_));
      if (transfer_)
        client_->OnTransferSizeUpdated(transfer_);
      transfer_ = 0;
      if (completion_ && !pump_) {
        const network::URLLoaderCompletionStatus status = *completion_;
        completion_.reset();
        OnComplete(status);
      }
      return;
    }
    if (decision.request_headers) {
      // What the next hop changes against what it would have sent.
      if (follow_) {
        for (const auto& header : request_.headers.GetHeaderVector())
          if (!decision.request_headers->HasHeader(header.key))
            follow_->removed_headers.push_back(header.key);
        follow_->modified_headers = *decision.request_headers;
      }
      request_.headers = *decision.request_headers;
    }
    Send();
  }

 private:
  enum class Hold { kNone, kHold, kBlocked };

  void AskAtRequest() {
    Observe("request", nullptr, false, net::OK);
    const Hold hold = Stage(true);
    if (hold == Hold::kBlocked) {
      Drop();
      Finish(network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
      return;
    }
    if (hold == Hold::kHold) {
      // Too many held already must not be how a page gets past a rule.
      if (Ask("request", nullptr))
        return;
      Drop();
      Finish(network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
      return;
    }
    Send();
  }

  // To the network: a new request, or the redirect it waits to follow.
  void Send() {
    if (follow_) {
      auto params = std::move(*follow_);
      follow_.reset();
      network_->FollowRedirect(std::move(params), follow_url_);
      return;
    }
    if (!target_->factory.is_connected()) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_FAILED));
      return;
    }
    uint32_t options = options_;
    int32_t request_id = request_id_;
    bool any = false, hold = false;
    NetworkStageRules(&any, &hold);
    if (any && target_->header_client) {
      // Ours to tell apart: a page chooses its request's number.
      options |= network::mojom::kURLLoadOptionUseHeaderClient;
      request_id = content::GlobalRequestID::MakeBrowserInitiated().request_id;
      // A second send (a redirect the application made) leaves no entry of
      // the first behind to reach this loader after it has gone.
      ForgetRequestId();
      network_request_id_ = request_id;
      target_->header_client->Add(request_id, this);
    } else if (hold) {
      // A rule that cannot be kept is not how a request gets past it.
      Finish(network::URLLoaderCompletionStatus(net::ERR_BLOCKED_BY_CLIENT));
      return;
    }
    target_->factory->CreateLoaderAndStart(network_.BindNewPipeAndPassReceiver(), request_id, options,
                                           request_, network_client_.BindNewPipeAndPassRemote(), annotation_);
    network_client_.set_disconnect_handler(base::BindOnce(&Loader::NetworkGone, base::Unretained(this)));
  }

  // A shared worker's script comes through its first client's frame.
  zebkit_view* PolicyView(bool* refused) const { return PolicyViewFor(view_, profile_, worker_, refused); }

  // Whether rules of the network's stages exist, whatever URL: a redirect can reach one
  // that matches, and a loader has its header client from its start or never.
  void NetworkStageRules(bool* any, bool* hold) const {
    bool refused = false;
    zebkit_view* view = PolicyView(&refused);
    zebkit_view* decider = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    if (!decider || !decider->network_rules_)
      return;
    for (const NetworkRule& rule : decider->network_rules_->rules) {
      if (rule.network_stage) {
        *any = true;
        *hold = *hold || rule.hold;
      }
    }
  }

  base::DictValue DescribeNetworkStage(const char* stage) const { return Describe(stage, nullptr); }

  // The filter for this stage. A popup whose opener closed has no policy left.
  Hold Stage(bool request_stage) {
    bool refused = false;
    zebkit_view* view = PolicyView(&refused);
    if (refused)
      return Hold::kBlocked;
    if (!view)
      return Hold::kNone;
    zebkit_view* decider = view->Decider(&zebkit_view::network_rules_);
    if (!decider)
      return Hold::kBlocked;
    return Matches(decider, request_stage, true) && Requests::HasDecider(view) ? Hold::kHold : Hold::kNone;
  }

  bool Matches(zebkit_view* decider, bool request_stage, bool hold, bool body = false) const {
    return RulesMatch(decider, NetworkResourceType(request_), request_.url, main_frame_, request_stage, hold, body);
  }

  base::DictValue Describe(const char* stage, const network::mojom::URLResponseHead* head) const {
    base::DictValue value;
    value.Set("stage", stage);
    value.Set("requestId", id_);
    value.Set("url", request_.url.spec());
    value.Set("method", request_.method);
    value.Set("headers", RequestHeaderList(request_.headers));
    value.Set("resourceType", NetworkResourceType(request_));
    value.Set("frameId", frame_);
    value.Set("mainFrame", main_frame_);
    value.Set("worker", worker_ != Worker::kNone);
    if (worker_ != Worker::kNone)
      value.Set("workerType", WorkerName(worker_));
    value.Set("navigation", request_.mode == network::mojom::RequestMode::kNavigate);
    value.Set("initiator", request_.request_initiator ? request_.request_initiator->Serialize() : "");
    size_t bytes = 0;
    bool whole = true;
    if (request_.request_body) {
      std::string body;
      for (const auto& element : *request_.request_body->elements()) {
        if (element.type() != network::DataElement::Tag::kBytes) { whole = false; continue; }
        const auto span = element.As<network::DataElementBytes>().bytes();
        bytes += span.size();
        if (body.size() + span.size() <= 65536) body.append(base::as_string_view(span));
        else whole = false;
      }
      value.Set("body", base::Base64Encode(body));
    }
    value.Set("hasBody", !!request_.request_body);
    value.Set("bodySize", static_cast<double>(bytes));
    value.Set("bodyComplete", whole);
    if (head) {
      value.Set("status", head->headers ? head->headers->response_code() : 0);
      value.Set("statusText", head->headers ? head->headers->GetStatusText() : "");
      value.Set("responseHeaders", ResponseHeaderList(head->headers.get()));
      value.Set("mimeType", head->mime_type);
    }
    return value;
  }

  bool Ask(const char* stage, const network::mojom::URLResponseHead* head) {
    auto& requests = Requests::Get();
    Requests::Reply reply = base::BindOnce(&Loader::Decided, weak_factory_.GetWeakPtr(), stage);
    bool refused = false;
    zebkit_view* view = PolicyView(&refused);
    if (refused || !view || !requests.Ask(view, Describe(stage, head), reply))
      return false;
    held_ = requests.last_id();
    return true;
  }

  // Told, without waiting. @always: an end the application caused by answering
  // from a file or a stream, which no rule has to ask for.
  void Observe(const char* kind, const network::mojom::URLResponseHead* head, bool always, int error) {
    bool refused = false;
    zebkit_view* view = PolicyView(&refused);
    zebkit_view* decider = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    if (!decider || !EventTeller(view))
      return;
    const bool response_kind = strcmp(kind, "request") != 0;
    if (!always && !Matches(decider, !response_kind, false))
      return;
    base::DictValue event = Describe(kind, head);
    event.Set("kind", kind);
    if (error != net::OK)
      event.Set("error", net::ErrorToString(error));
    PostEvent(view, std::move(event));
  }

  // The response to the page, through a pump when a rule tells its body.
  void Respond(network::mojom::URLResponseHeadPtr head, mojo::ScopedDataPipeConsumerHandle body,
               std::optional<mojo_base::BigBuffer> metadata) {
    bool refused = false;
    zebkit_view* view = PolicyView(&refused);
    zebkit_view* rules = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    zebkit_view* teller = view ? view->Decider(&zebkit_view::on_network_body_) : nullptr;
    if (!rules || !teller || !teller->on_network_body_ || !Matches(rules, false, false, true)) {
      client_->OnReceiveResponse(std::move(head), std::move(body), std::move(metadata));
      return;
    }
    body_view_ = view->GetWeakPtr();
    body_teller_ = teller->GetWeakPtr();
    body_generation_ = teller->network_body_generation_;
    body_telling_ = true;
    if (body.is_valid()) {
      mojo::ScopedDataPipeProducerHandle producer;
      mojo::ScopedDataPipeConsumerHandle consumer;
      if (mojo::CreateDataPipe(kPipeSize, producer, consumer) != MOJO_RESULT_OK) {
        Drop();
        TellBodyEnd();
        Finish(network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
        return;
      }
      pump_ = std::make_unique<BodyPump>(std::move(body), std::move(producer),
                                         base::BindRepeating(&Loader::TellBody, base::Unretained(this)),
                                         base::BindOnce(&Loader::BodyDrained, base::Unretained(this)));
      body = std::move(consumer);
    }
    client_->OnReceiveResponse(std::move(head), std::move(body), std::move(metadata));
  }

  // The function that was told this body's start, while it is still the one.
  zebkit_network_body_func BodyFunction() const {
    if (!body_telling_ || !body_view_ || !body_teller_ ||
        body_teller_->network_body_generation_ != body_generation_)
      return nullptr;
    return body_teller_->on_network_body_;
  }

  void TellBody(base::span<const uint8_t> bytes) {
    zebkit_network_body_func function = BodyFunction();
    if (!function) {
      body_telling_ = false;
      return;
    }
    const size_t told = std::min(bytes.size(), kBodyLimit - body_told_);
    const bool over = told < bytes.size();
    body_told_ += told;
    base::WeakPtr<Loader> alive = weak_factory_.GetWeakPtr();
    if (told)
      function(body_view_.get(), id_.c_str(), bytes.data(), told, false, false, body_teller_->on_network_body_data_);
    if (!alive || !over)
      return;
    // The page goes on, untold.
    function = BodyFunction();
    body_telling_ = false;
    if (function)
      function(body_view_.get(), id_.c_str(), nullptr, 0, true, true, body_teller_->on_network_body_data_);
  }

  // Told once. Before the body's outcome: finished or failed comes after it.
  void TellBodyEnd() {
    zebkit_network_body_func function = BodyFunction();
    body_telling_ = false;
    if (function)
      function(body_view_.get(), id_.c_str(), nullptr, 0, true, false, body_teller_->on_network_body_data_);
  }

  // The network's pipe is empty and closed, or the page's closed. The page's reads
  // end with its pipe; how the request ended waits for the network to say.
  void BodyDrained() {
    // From inside the pump: it goes after what is on the stack.
    base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE, std::move(pump_));
    if (completion_) {
      const network::URLLoaderCompletionStatus status = *completion_;
      completion_.reset();
      OnComplete(status);
    }
  }

  // As DevTools' ProcessRedirectByClient: a 307 of the application's, which the
  // page's side follows like any other, with its own checks.
  void Redirect(const GURL& url) {
    if (++redirects_ > kRedirectLimit) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_TOO_MANY_REDIRECTS));
      return;
    }
    // The page's side applies redirect: "manual" and "error", as for DevTools'.
    follow_.reset();
    auto head = network::mojom::URLResponseHead::New();
    const std::string raw = "HTTP/1.1 307 Internal Redirect\r\nLocation: " + url.spec() +
                            "\r\nNon-Authoritative-Reason: zebkit\r\n\r\n";
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(net::HttpUtil::AssembleRawHeaders(raw));
    head->encoded_data_length = 0;
    head->request_time = head->response_time = base::Time::Now();
    const auto policy = request_.update_first_party_url_on_redirect
                            ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
                            : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL;
    net::RedirectInfo info = net::RedirectInfo::ComputeRedirectInfo(
        request_.method, request_.url, request_.site_for_cookies, policy, request_.referrer_policy,
        request_.referrer.spec(), request_.request_initiator, 307, url, std::nullopt,
        /*insecure_scheme_was_upgraded=*/false, /*copy_fragment=*/true);
    redirect_ = info;
    synthetic_redirect_ = true;
    client_->OnReceiveRedirect(info, std::move(head));
  }

  void ReplaceResponseHeaders(const std::vector<std::pair<std::string, std::string>>& headers) {
    const std::string line = head_->headers ? head_->headers->GetStatusLine() : "HTTP/1.1 200 OK";
    head_->headers = base::MakeRefCounted<net::HttpResponseHeaders>(RawResponseHeaders(line, headers));
    std::string mime, charset;
    head_->headers->GetMimeTypeAndCharset(&mime, &charset);
    if (!mime.empty()) {
      head_->mime_type = mime;
      head_->charset = charset;
    }
    // Parsed again by who needs it, as for DevTools' overridden responses.
    head_->parsed_headers = nullptr;
  }

  // As DevTools' ProcessResponseOverride and SendResponse.
  network::mojom::URLResponseHeadPtr AnswerHead(
      int status, const std::optional<std::vector<std::pair<std::string, std::string>>>& headers,
      std::string_view start) {
    auto head = network::mojom::URLResponseHead::New();
    const std::string line = "HTTP/1.1 " + base::NumberToString(status) + " " +
                             std::string(net::GetHttpReasonPhrase(static_cast<net::HttpStatusCode>(status)));
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        RawResponseHeaders(line, headers.value_or(std::vector<std::pair<std::string, std::string>>())));
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
    head->request_time = head->response_time = base::Time::Now();
    if (options_ & network::mojom::kURLLoadOptionSniffMimeType) {
      if (!start.empty() && network::ShouldSniffContent(request_.url, *head)) {
        const std::string hint = head->mime_type;
        net::SniffMimeType(start.substr(0, std::min(start.size(), size_t{net::kMaxBytesToSniff})),
                           request_.url, hint, net::ForceSniffFileUrlsForHtml::kDisabled, &head->mime_type);
        head->did_mime_sniff = true;
      } else if (head->mime_type.empty()) {
        head->mime_type = "text/plain";
      }
    }
    head->encoded_data_length = 0;
    head->encoded_body_length = network::mojom::EncodedBodyLength::New(0u);
    return head;
  }

  void FinishAnswer(size_t bytes) {
    network::URLLoaderCompletionStatus done(net::OK);
    done.encoded_data_length = base::ByteSize(bytes);
    done.encoded_body_length = base::ByteSize(bytes);
    done.decoded_body_length = base::ByteSize(bytes);
    done.completion_time = base::TimeTicks::Now();
    Finish(done);
  }

  void FulfillFile(int status, std::optional<std::vector<std::pair<std::string, std::string>>> headers,
                   base::FilePath path) {
    answered_ = true;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&OpenAnswerFile, std::move(path)),
        base::BindOnce(&Loader::FileOpened, weak_factory_.GetWeakPtr(), status, std::move(headers)));
  }

  void FileOpened(int status, std::optional<std::vector<std::pair<std::string, std::string>>> headers,
                  OpenedFile opened) {
    if (!opened.file.IsValid()) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_FILE_NOT_FOUND));
      return;
    }
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(kPipeSize, producer, consumer) != MOJO_RESULT_OK) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
      return;
    }
    auto head = AnswerHead(status, headers, opened.start);
    head->content_length = opened.size;
    file_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
    client_->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
    // The bytes go from a sequence of its own, as the pipe takes them.
    file_->Write(std::make_unique<mojo::FileDataSource>(std::move(opened.file)),
                 base::BindOnce(&Loader::FileSent, weak_factory_.GetWeakPtr(), static_cast<size_t>(opened.size)));
  }

  void FileSent(size_t bytes, MojoResult result) {
    file_.reset();
    if (result == MOJO_RESULT_OK)
      FinishAnswer(bytes);
    else
      Finish(network::URLLoaderCompletionStatus(net::ERR_FAILED));
  }

  void FulfillStream(int status, const std::optional<std::vector<std::pair<std::string, std::string>>>& headers,
                     uintptr_t token) {
    answered_ = true;
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(kPipeSize, producer, consumer) != MOJO_RESULT_OK) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
      return;
    }
    stream_ = std::make_unique<AnswerStream>(token, std::move(producer),
                                             base::BindOnce(&Loader::StreamEnded, base::Unretained(this)));
    Streams()[token].stream = stream_->GetWeakPtr();
    // Its first bytes are not there yet: a type is the application's to give.
    client_->OnReceiveResponse(AnswerHead(status, headers, {}), std::move(consumer), std::nullopt);
  }

  void StreamEnded(bool failed, size_t bytes) {
    if (failed)
      Finish(network::URLLoaderCompletionStatus(net::ERR_FAILED));
    else
      FinishAnswer(bytes);
  }

  void Fulfill(int status, const std::optional<std::vector<std::pair<std::string, std::string>>>& headers,
               std::string body) {
    auto head = AnswerHead(status, headers, body);
    head->content_length = static_cast<int64_t>(body.size());
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(std::max<size_t>(1, body.size()), producer, consumer) != MOJO_RESULT_OK) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
      return;
    }
    if (!body.empty()) {
      size_t written = 0;
      const MojoResult result = producer->WriteData(base::as_byte_span(body), MOJO_WRITE_DATA_FLAG_ALL_OR_NONE, written);
      if (result != MOJO_RESULT_OK || written != body.size()) {
        Finish(network::URLLoaderCompletionStatus(net::ERR_INSUFFICIENT_RESOURCES));
        return;
      }
    }
    producer.reset();
    client_->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
    FinishAnswer(body.size());
  }

  // Nothing more from the network.
  void Drop() {
    ForgetRequestId();
    network_client_.reset();
    network_.reset();
    follow_.reset();
    head_.reset();
    body_.reset();
    metadata_.reset();
    completion_.reset();
    transfer_ = 0;
  }

  void Finish(const network::URLLoaderCompletionStatus& status) {
    if (finished_)
      return;
    finished_ = true;
    Observe(status.error_code == net::OK ? "finished" : "failed", nullptr, answered_, status.error_code);
    client_->OnComplete(status);
  }

  void NetworkGone() {
    network_client_.reset();
    if (!finished_ && !completion_)
      OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
  }
  // The page's end went: a body being told ends here, as failed.
  void Gone() {
    if (body_telling_ && !finished_) {
      base::WeakPtr<Loader> alive = weak_factory_.GetWeakPtr();
      TellBodyEnd();
      if (!alive)
        return;
      finished_ = true;
      Observe("failed", nullptr, false, net::ERR_ABORTED);
    }
    delete this;
  }

  base::WeakPtr<zebkit_view> view_;
  content::BrowserContext* profile_;
  std::string id_;
  std::string frame_;
  bool main_frame_;
  Worker worker_;
  scoped_refptr<Target> target_;
  int32_t request_id_;
  uint32_t options_;
  network::ResourceRequest request_;
  net::MutableNetworkTrafficAnnotationTag annotation_;
  uintptr_t held_ = 0;
  int redirects_ = 0;
  bool finished_ = false;
  bool synthetic_redirect_ = false;
  std::optional<net::RedirectInfo> redirect_;
  std::optional<network::HttpRequestHeadersUpdateParams> follow_;
  std::optional<GURL> follow_url_;
  // A response held.
  network::mojom::URLResponseHeadPtr head_;
  mojo::ScopedDataPipeConsumerHandle body_;
  std::optional<mojo_base::BigBuffer> metadata_;
  std::optional<network::URLLoaderCompletionStatus> completion_;
  int32_t transfer_ = 0;
  // A body told as it passes.
  std::unique_ptr<BodyPump> pump_;
  base::WeakPtr<zebkit_view> body_view_;
  base::WeakPtr<zebkit_view> body_teller_;
  uint64_t body_generation_ = 0;
  size_t body_told_ = 0;
  bool body_telling_ = false;
  // An answer from a file or a stream.
  bool answered_ = false;
  std::unique_ptr<mojo::DataPipeProducer> file_;
  std::unique_ptr<AnswerStream> stream_;
  // The network's stages.
  std::optional<int32_t> network_request_id_;
  mojo::UniqueReceiverSet<network::mojom::TrustedHeaderClient> header_watch_;
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Remote<network::mojom::URLLoader> network_;
  mojo::Receiver<network::mojom::URLLoaderClient> network_client_{this};
  base::WeakPtrFactory<Loader> weak_factory_{this};
};

void HeaderClient::OnLoaderCreated(int32_t request_id,
                                   mojo::PendingReceiver<network::mojom::TrustedHeaderClient> client) {
  auto it = loaders_.find(request_id);
  if (it != loaders_.end())
    it->second->WatchHeaders(std::move(client));
  else
    others_.Add(std::make_unique<HeaderPass>(), std::move(client));
}

// As in DevTools' Network, a request of its own, which CDP's Fetch does not hold and
// a rule of the network's stages does. It has no loader here to tell it from another.
void HeaderClient::OnLoaderForCorsPreflightCreated(const network::ResourceRequest& request,
                                                   mojo::PendingReceiver<network::mojom::TrustedHeaderClient> client) {
  auto describe = [](std::string id, std::string frame, bool main_frame, Worker worker, std::string method,
                     std::string initiator, const char* stage) {
    base::DictValue value;
    value.Set("stage", stage);
    value.Set("requestId", id);
    value.Set("method", method);
    value.Set("resourceType", "preflight");
    value.Set("preflight", true);
    value.Set("frameId", frame);
    value.Set("mainFrame", main_frame);
    value.Set("worker", worker != Worker::kNone);
    if (worker != Worker::kNone)
      value.Set("workerType", WorkerName(worker));
    value.Set("navigation", false);
    value.Set("initiator", initiator);
    value.Set("hasBody", false);
    value.Set("bodySize", 0.0);
    value.Set("bodyComplete", true);
    return value;
  };
  others_.Add(std::make_unique<HeaderWatch>(
                  view_, profile_, worker_, "preflight", main_frame_,
                  base::BindRepeating(describe, NextRequestId(), frame_, main_frame_, worker_, request.method,
                                      request.request_initiator ? request.request_initiator->Serialize() : "")),
              std::move(client));
}

// Deletes itself when the page's factory and its clones go. Loaders keep the
// network's factory for as long as they want it.
class Factory : public network::mojom::URLLoaderFactory {
 public:
  Factory(base::WeakPtr<zebkit_view> view, content::BrowserContext* profile, std::string frame, bool main_frame,
          Worker worker, mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
          mojo::PendingRemote<network::mojom::URLLoaderFactory> target,
          mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>* header_client)
      : view_(std::move(view)), profile_(profile), frame_(std::move(frame)), main_frame_(main_frame),
        worker_(worker), target_(base::MakeRefCounted<Target>()) {
    Factories().insert(this);
    target_->factory.Bind(std::move(target));
    if (header_client && !*header_client) {
      target_->header_client = std::make_unique<HeaderClient>(view_, profile_, frame_, main_frame_, worker_,
                                                              header_client->InitWithNewPipeAndPassReceiver());
    }
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(&Factory::Disconnected, base::Unretained(this)));
  }
  ~Factory() override { Factories().erase(this); }

  void CreateLoaderAndStart(mojo::PendingReceiver<network::mojom::URLLoader> loader, int32_t request_id,
                            uint32_t options, const network::ResourceRequest& request,
                            mojo::PendingRemote<network::mojom::URLLoaderClient> client,
                            const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    // A main frame's navigation factory is the frame's; its request says.
    const bool main = request.mode == network::mojom::RequestMode::kNavigate
                          ? request.is_outermost_main_frame : main_frame_;
    // The script of a shared worker comes through its first client's frame.
    const Worker worker = request.destination == network::mojom::RequestDestination::kSharedWorker
                              ? Worker::kShared : worker_;
    // A worker's factory is here from its start, for a policy set while it runs.
    // Until then it is one hop and no object per request.
    if (!view_ && !WorkerPolicies().contains(profile_)) {
      target_->factory->CreateLoaderAndStart(std::move(loader), request_id, options, request, std::move(client),
                                             annotation);
      return;
    }
    (new Loader(view_, profile_, frame_, main, worker, target_, std::move(loader), request_id, options, request,
                std::move(client), annotation))->Start();
  }
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver) override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  void Disconnected() {
    if (receivers_.empty())
      delete this;
  }
  base::WeakPtr<zebkit_view> view_;
  content::BrowserContext* profile_;
  std::string frame_;
  bool main_frame_;
  Worker worker_;
  scoped_refptr<Target> target_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
};

class WebSocketHold;
std::set<WebSocketHold*>& WebSocketHolds() {
  static base::NoDestructor<std::set<WebSocketHold*>> holds;
  return *holds;
}

// One WebSocket's handshake before it starts: request stage only, since after it
// the network talks to the page directly. Frames are not seen.
class WebSocketHold {
 public:
  WebSocketHold(content::ContentBrowserClient::WebSocketFactory factory, const GURL& url,
                net::HttpRequestHeaders headers,
                mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> client,
                mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client)
      : factory_(std::move(factory)), url_(url), headers_(std::move(headers)), client_(std::move(client)),
        header_client_(std::move(header_client)) {
    WebSocketHolds().insert(this);
    // The page closed it, or went, while it waits.
    client_.set_disconnect_handler(base::BindOnce([](WebSocketHold* self) { delete self; }, base::Unretained(this)));
  }
  ~WebSocketHold() {
    WebSocketHolds().erase(this);
    Requests::Get().Abandon(held_);
  }

  // The profile and kind of worker whose socket it is, for the network's stages.
  void SetOwner(content::BrowserContext* profile, Worker worker) {
    profile_ = profile;
    worker_ = worker;
  }

  // May end it, and this with it.
  void Start(zebkit_view* view, bool refused, base::DictValue description, bool main_frame) {
    if (view)
      view_ = view->GetWeakPtr();
    main_frame_ = main_frame;
    description_ = description.Clone();
    if (refused) {
      Refuse(net::ERR_BLOCKED_BY_CLIENT);
      return;
    }
    if (!view) {
      Connect();
      return;
    }
    zebkit_view* decider = view->Decider(&zebkit_view::network_rules_);
    if (!decider) {
      Refuse(net::ERR_BLOCKED_BY_CLIENT);
      return;
    }
    if (EventTeller(view) && RulesMatch(decider, kWebSocket, url_, main_frame, true, false, false)) {
      base::DictValue event = description.Clone();
      event.Set("kind", "request");
      PostEvent(view, std::move(event));
    }
    if (!RulesMatch(decider, kWebSocket, url_, main_frame, true, true, false) || !Requests::HasDecider(view)) {
      Connect();
      return;
    }
    auto& requests = Requests::Get();
    Requests::Reply reply = base::BindOnce(&WebSocketHold::Decided, weak_factory_.GetWeakPtr());
    // Too many held already must not be how a page gets past a rule.
    if (!requests.Ask(view, std::move(description), reply)) {
      Refuse(net::ERR_BLOCKED_BY_CLIENT);
      return;
    }
    held_ = requests.last_id();
  }

 private:
  void Decided(Traits::Decision decision) {
    held_ = 0;
    if (decision.action == Action::kBlock) {
      Refuse(decision.error);
      return;
    }
    if (decision.request_headers)
      headers_ = *decision.request_headers;
    Connect();
  }

  void Connect() {
    std::vector<network::mojom::HttpHeaderPtr> additional;
    for (const auto& header : headers_.GetHeaderVector())
      additional.push_back(network::mojom::HttpHeader::New(header.key, header.value));
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client = std::move(header_client_);
    // A rule of the network's stages for WebSockets: the network's own handshake
    // asks this client, with its Cookie, as it asks a loader's (HeaderWatch), the
    // way extensions' webRequest sees a socket's headers.
    if (WantsNetworkStages()) {
      mojo::PendingRemote<network::mojom::TrustedHeaderClient> watch;
      mojo::MakeSelfOwnedReceiver(
          std::make_unique<HeaderWatch>(
              view_, profile_, worker_, kWebSocket, main_frame_,
              base::BindRepeating(
                  [](const base::DictValue* description, const char* stage) {
                    base::DictValue value = description->Clone();
                    value.Set("stage", stage);
                    return value;
                  },
                  base::Owned(std::make_unique<base::DictValue>(description_.Clone())))),
          watch.InitWithNewPipeAndPassReceiver());
      header_client = std::move(watch);
    }
    std::move(factory_).Run(url_, std::move(additional), client_.Unbind(), mojo::NullRemote(),
                            std::move(header_client));
    delete this;
  }

  bool WantsNetworkStages() {
    bool refused = false;
    zebkit_view* view = PolicyViewFor(view_, profile_, worker_, &refused);
    zebkit_view* decider = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    if (refused || !decider)
      return false;
    for (const char* stage : {kSendHeaders, kSent, kHeadersReceived}) {
      for (bool hold : {false, true}) {
        if (NetworkStageMatches(decider, stage, kWebSocket, url_, main_frame_, hold))
          return true;
      }
    }
    return false;
  }

  void Refuse(int error) {
    client_->OnFailure(net::ErrorToString(error), error, 0);
    delete this;
  }

  content::ContentBrowserClient::WebSocketFactory factory_;
  GURL url_;
  net::HttpRequestHeaders headers_;
  mojo::Remote<network::mojom::WebSocketHandshakeClient> client_;
  mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client_;
  base::WeakPtr<zebkit_view> view_;
  content::BrowserContext* profile_ = nullptr;
  Worker worker_ = Worker::kNone;
  bool main_frame_ = false;
  base::DictValue description_;
  uintptr_t held_ = 0;
  base::WeakPtrFactory<WebSocketHold> weak_factory_{this};
};

// What a view has for a frame's or dedicated worker's request: whether it ever had
// a filter, itself or through its opener.
bool HasNetworkPolicy(zebkit_view* view) {
  // A popup keeps the proxy its opener's policy gave it after that opener goes.
  zebkit_view* decider = view->Decider(&zebkit_view::network_rules_);
  if (decider && decider->network_ever_)
    view->network_ever_ = true;
  return view->network_ever_;
}

bool ParseHeaders(const base::ListValue& list, std::vector<std::pair<std::string, std::string>>* out) {
  if (list.size() > 256)
    return false;
  for (const base::Value& header : list) {
    const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
    const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
    if (!name || !value || !net::HttpUtil::IsValidHeaderName(*name) || !net::HttpUtil::IsValidHeaderValue(*value))
      return false;
    out->emplace_back(*name, *value);
  }
  return true;
}

// {status, headers} of an answer whose body comes from elsewhere: as fulfill's.
bool ParseAnswer(Requests::Entry* entry, const char* json, Traits::Decision* decision, std::string* error) {
  const std::string* type = entry->metadata.FindString("resourceType");
  if (type && *type == kWebSocket) { *error = "a WebSocket is continued or blocked"; return false; }
  std::optional<base::Value> parsed = base::JSONReader::Read(json ? json : "{}", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) { *error = "a reply is a JSON object"; return false; }
  const base::DictValue& reply = parsed->GetDict();
  const std::string* action = reply.FindString("action");
  if (action && *action != "fulfill") { *error = "this reply is a fulfill"; return false; }
  decision->status = reply.FindInt("status").value_or(200);
  if (decision->status < 200 || decision->status > 599 || (decision->status >= 300 && decision->status < 400)) {
    *error = "fulfill takes a status from 200 to 599, not a redirect";
    return false;
  }
  std::vector<std::pair<std::string, std::string>> pairs;
  const base::ListValue* headers = reply.FindList("headers");
  if (headers && !ParseHeaders(*headers, &pairs)) { *error = "invalid headers"; return false; }
  if (IsNetworkStage(entry->metadata.FindString("stage"))) {
    *error = "inside the network a request is continued or blocked";
    return false;
  }
  decision->response_headers = std::move(pairs);
  decision->state = "answered";
  return true;
}

}  // namespace

void InstallNetworkHooks() { Requests::Get().Start(); }
void StopNetworkHooks() {
  while (!Loaders().empty())
    delete *Loaders().begin();
  while (!Factories().empty())
    delete *Factories().begin();
  while (!WebSocketHolds().empty())
    delete *WebSocketHolds().begin();
  Requests::Get().Stop();
  Streams().clear();
  WorkerPolicies().clear();
}
void CancelNetworkHookRequests(zebkit_view* view) { Requests::Get().CancelAll(view, "cancelled"); }

void ForgetWorkerPolicy(content::BrowserContext* profile) {
  WorkerPolicies().erase(profile);
}

void InterceptNetworkHookFactory(content::BrowserContext* profile, content::RenderFrameHost* frame,
                                 NetworkHookFactory kind, network::URLLoaderFactoryBuilder& builder,
                                 mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>* header_client) {
  if (!Requests::Get().Ready())
    return;
  const Worker worker = kind == NetworkHookFactory::kWorker
                            ? (frame ? Worker::kDedicated : Worker::kShared)
                            : kind == NetworkHookFactory::kServiceWorker ? Worker::kService : Worker::kNone;
  if (!frame) {
    if (worker == Worker::kNone)
      return;
    auto [receiver, target] = builder.Append();
    new Factory(nullptr, profile, std::string(), false, worker, std::move(receiver), std::move(target), header_client);
    return;
  }
  zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  // A worker's script through a frame may be a shared worker's, which a profile's
  // policy decides whatever the frame's view has.
  const bool profile_policy = worker != Worker::kNone && WorkerPolicies().contains(profile);
  if (!(view && HasNetworkPolicy(view)) && !profile_policy)
    return;
  auto [receiver, target] = builder.Append();
  new Factory(view && HasNetworkPolicy(view) ? view->GetWeakPtr() : base::WeakPtr<zebkit_view>(), profile,
              "f" + base::NumberToString(frame->GetFrameTreeNodeId().value()),
              !frame->GetParentOrOuterDocument(), worker, std::move(receiver), std::move(target), header_client);
}

bool WillInterceptNetworkHookWebSocket(content::RenderFrameHost* frame) {
  if (!Requests::Get().Ready())
    return false;
  // Shared and service workers' have no frame, and nothing says whose they are.
  if (!frame)
    return !WorkerPolicies().empty();
  zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  return view && HasNetworkPolicy(view);
}

void CreateNetworkHookWebSocket(content::RenderFrameHost* frame,
                                content::ContentBrowserClient::WebSocketFactory factory, const GURL& url,
                                const std::optional<std::string>& user_agent,
                                mojo::PendingRemote<network::mojom::WebSocketHandshakeClient> client,
                                mojo::PendingRemote<network::mojom::TrustedHeaderClient> header_client) {
  zebkit_view* view = nullptr;
  bool refused = false;
  bool main_frame = false;
  std::string frame_id;
  base::DictValue description;
  if (frame) {
    view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
    main_frame = !frame->GetParentOrOuterDocument();
    frame_id = "f" + base::NumberToString(frame->GetFrameTreeNodeId().value());
    description.Set("initiator", frame->GetLastCommittedOrigin().Serialize());
    // A dedicated worker's is given its frame, and is not told from the frame's own.
    description.Set("worker", false);
  } else {
    // Two profiles with a policy: nothing says which this one's is.
    auto& policies = WorkerPolicies();
    refused = policies.size() != 1 || !policies.begin()->second;
    if (!refused)
      view = policies.begin()->second.get();
    description.Set("initiator", "");
    description.Set("worker", true);
    description.Set("workerType", "shared-or-service");
  }
  net::HttpRequestHeaders headers;
  if (user_agent)
    headers.SetHeader(net::HttpRequestHeaders::kUserAgent, *user_agent);
  description.Set("stage", "request");
  description.Set("requestId", NextRequestId());
  description.Set("url", url.spec());
  description.Set("method", "GET");
  description.Set("headers", RequestHeaderList(headers));
  description.Set("resourceType", kWebSocket);
  description.Set("frameId", frame_id);
  description.Set("mainFrame", main_frame);
  description.Set("navigation", false);
  description.Set("hasBody", false);
  description.Set("bodySize", 0.0);
  description.Set("bodyComplete", true);
  auto* hold = new WebSocketHold(std::move(factory), url, std::move(headers), std::move(client),
                                 std::move(header_client));
  hold->SetOwner(frame ? frame->GetBrowserContext() : view && view->web_contents()
                                                          ? view->web_contents()->GetBrowserContext()
                                                          : nullptr,
                 frame ? Worker::kNone : Worker::kShared);
  hold->Start(view, refused, std::move(description), main_frame);
}

}  // namespace zebkit

extern "C" {
bool zebkit_view_set_network_filter(zebkit_view* view, const char* json, char** error) {
  using namespace zebkit;
  auto fail = [&](const std::string& message) { if (error) *error = strdup(message.c_str()); return false; };
  if (!Requests::Get().Ready() || !zebkit::LiveView(view) || !view->web_contents()) return fail("the view is closed");
  std::optional<base::Value> parsed = base::JSONReader::Read(json ? json : "", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list() || parsed->GetList().size() > 256)
    return fail("a filter is a JSON array of at most 256 rules");
  auto rules = std::make_shared<NetworkRules>();
  for (const base::Value& item : parsed->GetList()) {
    if (!item.is_dict()) return fail("a rule is an object");
    const base::DictValue& dict = item.GetDict();
    NetworkRule rule;
    if (const std::string* url = dict.FindString("url")) {
      RE2::Options options;
      options.set_log_errors(false);
      rule.url = std::make_unique<RE2>(NetworkGlobToRegex(*url), options);
      if (!rule.url->ok()) return fail("invalid URL pattern: " + *url);
    }
    if (const std::string* mode = dict.FindString("mode")) {
      if (*mode != "hold" && *mode != "observe") return fail("mode is hold or observe");
      rule.hold = *mode == "hold";
    }
    if (const std::string* stage = dict.FindString("stage")) {
      if (*stage == kSendHeaders) rule.network_stage = kSendHeaders;
      else if (*stage == kSent) rule.network_stage = kSent;
      else if (*stage == kHeadersReceived) rule.network_stage = kHeadersReceived;
      else if (*stage != "request" && *stage != "response")
        return fail("stage is request, sendHeaders, sent, headersReceived or response");
      rule.request_stage = *stage == "request";
      if (*stage == kSent && rule.hold) return fail("what is sent has gone: a rule at sent observes");
    }
    if (const std::string* frames = dict.FindString("frames")) {
      if (*frames == "main") rule.frames = NetworkRule::Frames::kMain;
      else if (*frames == "sub") rule.frames = NetworkRule::Frames::kSub;
      else if (*frames != "all") return fail("frames is all, main or sub");
    }
    if (const base::ListValue* types = dict.FindList("resourceTypes")) {
      for (const base::Value& type : *types) {
        if (!type.is_string()) return fail("a resource type is a string");
        rule.resource_types.insert(type.GetString());
      }
    }
    if (dict.FindBool("body").value_or(false)) {
      if (rule.request_stage || rule.network_stage) return fail("body is told at the response stage");
      const std::string* mode = dict.FindString("mode");
      if (mode && *mode == "hold") return fail("a rule with body holds nothing");
      rule.body = true;
      rule.hold = false;
    }
    rules->rules.push_back(std::move(rule));
  }
  view->network_rules_ = std::move(rules);
  // Frames' factories made before any policy have no proxy in front of them.
  std::vector<zebkit_view*> affected;
  for (zebkit_view* other : zebkit_view::Views())
    if (other == view || other->Decider(&zebkit_view::network_rules_) == view)
      affected.push_back(other);
  for (zebkit_view* other : affected) {
    if (other->network_ever_ || !other->web_contents()) continue;
    other->network_ever_ = true;
    other->web_contents()->ForEachRenderFrameHost(
        [](content::RenderFrameHost* frame) { content::UpdateSubresourceLoaderFactories(frame); });
  }
  return true;
}
bool zebkit_view_set_network_filter_for_workers(zebkit_view* view, bool on) {
  using namespace zebkit;
  if (!Requests::Get().Ready() || !zebkit::LiveView(view) || !view->web_contents()) return false;
  content::BrowserContext* profile = view->web_contents()->GetBrowserContext();
  auto& policies = WorkerPolicies();
  auto it = policies.find(profile);
  if (!on) {
    // Not another view's, while it is there to decide.
    if (it != policies.end() && it->second && it->second.get() != view) return false;
    if (it != policies.end()) policies.erase(it);
    return true;
  }
  // A filter is what decides: a promise of one would let everything through.
  if (!view->network_rules_) return false;
  policies[profile] = view->GetWeakPtr();
  return true;
}
void zebkit_view_on_network_body(zebkit_view* view, zebkit_network_body_func function, void* data) {
  using namespace zebkit;
  if (!Requests::Get().Ready() || !zebkit::LiveView(view) || !view->web_contents()) return;
  // Bodies under way are not told to a function that did not see their start.
  ++view->network_body_generation_;
  view->on_network_body_ = function;
  view->on_network_body_data_ = data;
}
void zebkit_view_on_network(zebkit_view* view, zebkit_network_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Install(view, function, data);
}
void zebkit_view_on_network_event(zebkit_view* view, zebkit_network_event_func function, void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::Requests::Get().Observe(view, function, data);
}
bool zebkit_view_set_network_timeout(zebkit_view* view, uint32_t milliseconds) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  return zebkit::Requests::Get().SetTimeout(view, milliseconds);
}
char* zebkit_network_request_info(zebkit_network_request* request) {
  return zebkit::Requests::Get().Info(reinterpret_cast<uintptr_t>(request));
}
bool zebkit_network_respond(zebkit_network_request* request, const char* json, const void* body, size_t body_size,
                            char** error) {
  using namespace zebkit;
  auto fail = [&](const char* message) { if (error) *error = strdup(message); return false; };
  auto& requests = Requests::Get();
  if (!requests.Ready()) return fail("network replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto* entry = requests.Find(id);
  if (!entry) return fail("the request is no longer pending");
  std::optional<base::Value> parsed = base::JSONReader::Read(json ? json : "", base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return fail("a reply is a JSON object");
  const base::DictValue& reply = parsed->GetDict();
  const std::string* action = reply.FindString("action");
  const std::string* stage = entry->metadata.FindString("stage");
  const bool network_stage = IsNetworkStage(stage);
  const bool response_stage = stage && (*stage == "response" || *stage == kHeadersReceived);
  if (!action) return fail("a reply has an action");
  if (body_size && *action != "fulfill") return fail("only fulfill takes a body");
  const std::string* type = entry->metadata.FindString("resourceType");
  const bool websocket = type && *type == kWebSocket;
  if (websocket && *action != "continue" && *action != "block")
    return fail("a WebSocket is continued or blocked");
  if (network_stage && *action != "continue" && *action != "block")
    return fail("inside the network a request is continued or blocked");
  Traits::Decision decision;
  decision.state = "answered";
  const base::ListValue* headers = reply.FindList("headers");
  std::vector<std::pair<std::string, std::string>> pairs;
  if (headers && !ParseHeaders(*headers, &pairs)) return fail("invalid headers");
  if (*action == "continue") {
    decision.action = Action::kContinue;
    if (headers && response_stage) {
      decision.response_headers = std::move(pairs);
    } else if (headers) {
      net::HttpRequestHeaders set;
      for (const auto& [name, value] : pairs) {
        // Inside the network, past the checks on a page's headers: a Cookie is the
        // application's to change there, as an extension's with extraHeaders.
        if (network_stage) {
          set.SetHeader(name, value);
          continue;
        }
        // What the network refuses a page outright, closing all its requests.
        if (!network::IsRequestHeaderSafe(name, value)) return fail("the network does not allow this header");
        // What the network would leave out of a handshake, saying nothing.
        if (websocket && !net::HttpUtil::IsSafeHeader(name, value) &&
            !base::EqualsCaseInsensitiveASCII(name, net::HttpRequestHeaders::kUserAgent) &&
            !base::EqualsCaseInsensitiveASCII(name, net::HttpRequestHeaders::kCookie))
          return fail("the network does not allow this header");
        set.SetHeader(name, value);
      }
      std::string forbidden;
      net::HttpRequestHeaders checked = set;
      if (!network_stage && network::ContainsForbiddenSecurityHeader(checked, &forbidden))
        return fail("the network does not allow this header");
      decision.request_headers = std::move(set);
    }
  } else if (*action == "block") {
    decision.action = Action::kBlock;
    if (const std::string* code = reply.FindString("error")) decision.error = NetworkErrorCode(*code);
    if (headers) return fail("block takes no headers");
  } else if (*action == "redirect") {
    decision.action = Action::kRedirect;
    const std::string* url = reply.FindString("url");
    if (!url) return fail("redirect takes a url");
    decision.url = GURL(*url);
    if (!decision.url.is_valid() || !decision.url.SchemeIsHTTPOrHTTPS()) return fail("redirect takes an http or https url");
    if (headers) return fail("redirect takes no headers");
  } else if (*action == "fulfill") {
    decision.action = Action::kFulfill;
    decision.status = reply.FindInt("status").value_or(200);
    if (decision.status < 200 || decision.status > 599 || (decision.status >= 300 && decision.status < 400))
      return fail("fulfill takes a status from 200 to 599, not a redirect");
    if (body_size > kBodyLimit || (body_size && !body)) return fail("a body is at most 64 MiB");
    decision.response_headers = std::move(pairs);
    if (body_size) decision.body.assign(static_cast<const char*>(body), body_size);
  } else {
    return fail("action is continue, block, redirect or fulfill");
  }
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
bool zebkit_network_respond_file(zebkit_network_request* request, const char* reply_json, const char* path,
                                 char** error) {
  using namespace zebkit;
  auto fail = [&](const std::string& message) { if (error) *error = strdup(message.c_str()); return false; };
  auto& requests = Requests::Get();
  if (!requests.Ready()) return fail("network replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto* entry = requests.Find(id);
  if (!entry) return fail("the request is no longer pending");
  Traits::Decision decision;
  std::string reason;
  if (!ParseAnswer(entry, reply_json, &decision, &reason)) return fail(reason);
  decision.file = base::FilePath::FromUTF8Unsafe(path ? path : "");
  if (!decision.file.IsAbsolute()) return fail("a file is an absolute path");
  decision.action = Action::kFile;
  requests.Settle(id, std::move(decision), "answered");
  return true;
}
zebkit_network_stream* zebkit_network_respond_stream(zebkit_network_request* request, const char* reply_json,
                                                     char** error) {
  using namespace zebkit;
  auto fail = [&](const std::string& message) -> zebkit_network_stream* {
    if (error) *error = strdup(message.c_str());
    return nullptr;
  };
  auto& requests = Requests::Get();
  if (!requests.Ready()) return fail("network replies need the engine thread");
  const auto id = reinterpret_cast<uintptr_t>(request);
  auto* entry = requests.Find(id);
  if (!entry) return fail("the request is no longer pending");
  Traits::Decision decision;
  std::string reason;
  if (!ParseAnswer(entry, reply_json, &decision, &reason)) return fail(reason);
  static uintptr_t next = 0;
  const uintptr_t token = ++next;
  Streams()[token];
  decision.action = Action::kStream;
  decision.stream = token;
  // Runs the loader's side now; it may have nothing left to answer, which writing says.
  requests.Settle(id, std::move(decision), "answered");
  return reinterpret_cast<zebkit_network_stream*>(token);
}
bool zebkit_network_stream_write(zebkit_network_stream* stream, const void* bytes, size_t size) {
  using namespace zebkit;
  if (!Requests::Get().Ready() || (size && !bytes)) return false;
  auto it = Streams().find(reinterpret_cast<uintptr_t>(stream));
  if (it == Streams().end() || !it->second.stream) return false;
  return it->second.stream->Write(
      UNSAFE_BUFFERS(base::span(static_cast<const uint8_t*>(bytes), size)));
}
void zebkit_network_stream_on_writable(zebkit_network_stream* stream, zebkit_network_stream_writable_func function,
                                       void* data) {
  using namespace zebkit;
  if (!Requests::Get().Ready()) return;
  const auto token = reinterpret_cast<uintptr_t>(stream);
  auto it = Streams().find(token);
  if (it == Streams().end()) return;
  it->second.writable = function;
  it->second.data = data;
  if (function && it->second.missed) {
    it->second.missed = false;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, base::BindOnce(&StreamWritable, token));
  }
}
bool zebkit_network_stream_is_open(zebkit_network_stream* stream) {
  using namespace zebkit;
  if (!Requests::Get().Ready()) return false;
  auto it = Streams().find(reinterpret_cast<uintptr_t>(stream));
  return it != Streams().end() && it->second.stream && it->second.stream->open();
}
void zebkit_network_stream_end(zebkit_network_stream* stream, bool failed) {
  using namespace zebkit;
  if (!Requests::Get().Ready()) return;
  auto it = Streams().find(reinterpret_cast<uintptr_t>(stream));
  if (it == Streams().end()) return;
  base::WeakPtr<AnswerStream> answer = it->second.stream;
  Streams().erase(it);
  if (answer) answer->End(failed);
}
}  // extern "C"
