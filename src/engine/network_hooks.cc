// Network requests and responses (<zebkit/hooks.h>): a URL loader factory in
// front of the network's, as extensions' webRequest and DevTools' Fetch domain
// put one. Requests a rule holds wait for the application, up to a deadline;
// those it observes are told and do not wait; the rest pass.

#include "zebkit/engine/network_hooks.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/byte_size.h"
#include "base/json/json_reader.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_util.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/zebkit_automation.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/mime_sniffer.h"
#include "net/base/net_errors.h"
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
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"
#include "zebkit/engine/automation_network.h"
#include "zebkit/engine/hook_requests.h"

namespace zebkit {

struct NetworkRule {
  std::unique_ptr<RE2> url;  // Null: every URL.
  bool hold = true;
  bool request_stage = true;
  std::set<std::string> resource_types;  // Empty: every type.
  enum class Frames { kAll, kMain, kSub } frames = Frames::kAll;
};
struct NetworkRules {
  std::vector<NetworkRule> rules;
};

namespace {

constexpr size_t kBodyLimit = 64 * 1024 * 1024;
constexpr int kRedirectLimit = 20;

enum class Action { kContinue, kBlock, kRedirect, kFulfill };

struct Traits {
  struct Decision {
    Action action = Action::kContinue;
    std::optional<net::HttpRequestHeaders> request_headers;
    std::optional<std::vector<std::pair<std::string, std::string>>> response_headers;
    int error = net::ERR_BLOCKED_BY_CLIENT;
    GURL url;
    int status = 200;
    std::string body;
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
  static const char* State(const Decision& decision) { return decision.state; }
};
using Requests = HookRequests<Traits>;

struct Target : base::RefCounted<Target> {
  mojo::Remote<network::mojom::URLLoaderFactory> factory;
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

class Loader;
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
  Loader(base::WeakPtr<zebkit_view> view, std::string frame, bool main_frame, bool worker,
         scoped_refptr<Target> target,
         mojo::PendingReceiver<network::mojom::URLLoader> loader, int32_t request_id, uint32_t options,
         const network::ResourceRequest& request,
         mojo::PendingRemote<network::mojom::URLLoaderClient> client,
         const net::MutableNetworkTrafficAnnotationTag& annotation)
      : view_(std::move(view)), frame_(std::move(frame)), main_frame_(main_frame), worker_(worker),
        target_(std::move(target)), request_id_(request_id), options_(options), request_(request),
        annotation_(annotation), receiver_(this, std::move(loader)), client_(std::move(client)) {
    static uint64_t next = 0;
    id_ = "n" + base::NumberToString(++next);
    Loaders().insert(this);
    receiver_.set_disconnect_handler(base::BindOnce(&Loader::Gone, base::Unretained(this)));
    client_.set_disconnect_handler(base::BindOnce(&Loader::Gone, base::Unretained(this)));
  }
  ~Loader() override {
    Loaders().erase(this);
    Requests::Get().Abandon(held_);
  }

  void Start() { AskAtRequest(); }

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
      client_->OnReceiveResponse(std::move(head), std::move(body), std::move(metadata));
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
    if (head_) {
      completion_ = status;
      return;
    }
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
      case Action::kContinue:
        break;
    }
    if (response) {
      if (decision.response_headers)
        ReplaceResponseHeaders(*decision.response_headers);
      client_->OnReceiveResponse(std::move(head_), std::move(body_), std::move(metadata_));
      if (transfer_)
        client_->OnTransferSizeUpdated(transfer_);
      transfer_ = 0;
      if (completion_)
        Finish(*completion_);
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
    target_->factory->CreateLoaderAndStart(network_.BindNewPipeAndPassReceiver(), request_id_, options_,
                                           request_, network_client_.BindNewPipeAndPassRemote(), annotation_);
    network_client_.set_disconnect_handler(base::BindOnce(&Loader::NetworkGone, base::Unretained(this)));
  }

  // The filter for this stage. A popup whose opener closed has no policy left.
  Hold Stage(bool request_stage) {
    zebkit_view* view = view_.get();
    if (!view)
      return Hold::kNone;
    zebkit_view* decider = view->Decider(&zebkit_view::network_rules_);
    if (!decider)
      return Hold::kBlocked;
    return Matches(decider, request_stage, true) && Requests::HasDecider(view) ? Hold::kHold : Hold::kNone;
  }

  bool Matches(zebkit_view* decider, bool request_stage, bool hold) const {
    if (!decider->network_rules_)
      return false;
    const std::string type = NetworkResourceType(request_);
    for (const NetworkRule& rule : decider->network_rules_->rules) {
      if (rule.hold != hold || rule.request_stage != request_stage)
        continue;
      if (rule.frames == NetworkRule::Frames::kMain && !main_frame_) continue;
      if (rule.frames == NetworkRule::Frames::kSub && main_frame_) continue;
      if (!rule.resource_types.empty() && !rule.resource_types.contains(type)) continue;
      if (rule.url && !RE2::FullMatch(request_.url.spec(), *rule.url)) continue;
      return true;
    }
    return false;
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
    value.Set("worker", worker_);
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
    if (!requests.Ask(view_.get(), Describe(stage, head), reply))
      return false;
    held_ = requests.last_id();
    return true;
  }

  // Told, without waiting. Bounded, and what was dropped is counted.
  void Observe(const char* kind, const network::mojom::URLResponseHead* head, bool, int error) {
    zebkit_view* view = view_.get();
    zebkit_view* decider = view ? view->Decider(&zebkit_view::network_rules_) : nullptr;
    if (!decider || !view->on_network_event_)
      return;
    const bool response_kind = strcmp(kind, "request") != 0;
    if (!Matches(decider, !response_kind, false))
      return;
    if (view->network_events_pending_ >= 4096) {
      ++view->network_events_dropped_;
      return;
    }
    base::DictValue event = Describe(kind, head);
    event.Set("kind", kind);
    if (error != net::OK)
      event.Set("error", net::ErrorToString(error));
    ++view->network_events_pending_;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, uint64_t generation, base::DictValue event) {
              if (!view)
                return;
              --view->network_events_pending_;
              if (view->network_generation_ != generation || !view->on_network_event_)
                return;
              event.Set("dropped", base::NumberToString(view->network_events_dropped_));
              view->network_events_dropped_ = 0;
              const std::string json = base::WriteJson(event).value_or("{}");
              auto function = view->on_network_event_;
              function(view.get(), json.c_str(), view->on_network_event_data_);
            },
            view_, view->network_generation_, std::move(event)));
  }

  // As DevTools' ProcessRedirectByClient: a 307 of the application's, which the
  // page's side follows like any other, with its own checks.
  void Redirect(const GURL& url) {
    if (++redirects_ > kRedirectLimit) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_TOO_MANY_REDIRECTS));
      return;
    }
    if (request_.redirect_mode != network::mojom::RedirectMode::kFollow) {
      Finish(network::URLLoaderCompletionStatus(net::ERR_FAILED));
      return;
    }
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

  static std::string RawHeaders(const std::string& status_line,
                                const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string raw = status_line + "\r\n";
    for (const auto& [name, value] : headers)
      raw += name + ": " + value + "\r\n";
    return net::HttpUtil::AssembleRawHeaders(raw + "\r\n");
  }

  void ReplaceResponseHeaders(const std::vector<std::pair<std::string, std::string>>& headers) {
    const std::string line = head_->headers ? head_->headers->GetStatusLine() : "HTTP/1.1 200 OK";
    head_->headers = base::MakeRefCounted<net::HttpResponseHeaders>(RawHeaders(line, headers));
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
  void Fulfill(int status, const std::optional<std::vector<std::pair<std::string, std::string>>>& headers,
               std::string body) {
    auto head = network::mojom::URLResponseHead::New();
    const std::string line = "HTTP/1.1 " + base::NumberToString(status) + " " +
                             std::string(net::GetHttpReasonPhrase(static_cast<net::HttpStatusCode>(status)));
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        RawHeaders(line, headers.value_or(std::vector<std::pair<std::string, std::string>>())));
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
    head->request_time = head->response_time = base::Time::Now();
    if (options_ & network::mojom::kURLLoadOptionSniffMimeType) {
      if (!body.empty() && network::ShouldSniffContent(request_.url, *head)) {
        const std::string hint = head->mime_type;
        net::SniffMimeType(std::string_view(body).substr(0, std::min(body.size(), size_t{net::kMaxBytesToSniff})),
                           request_.url, hint, net::ForceSniffFileUrlsForHtml::kDisabled, &head->mime_type);
        head->did_mime_sniff = true;
      } else if (head->mime_type.empty()) {
        head->mime_type = "text/plain";
      }
    }
    head->content_length = static_cast<int64_t>(body.size());
    head->encoded_data_length = 0;
    head->encoded_body_length = network::mojom::EncodedBodyLength::New(0u);
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
    network::URLLoaderCompletionStatus done(net::OK);
    done.encoded_data_length = base::ByteSize(body.size());
    done.encoded_body_length = base::ByteSize(body.size());
    done.decoded_body_length = base::ByteSize(body.size());
    done.completion_time = base::TimeTicks::Now();
    Finish(done);
  }

  // Nothing more from the network.
  void Drop() {
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
    Observe(status.error_code == net::OK ? "finished" : "failed", nullptr, false, status.error_code);
    client_->OnComplete(status);
  }

  void NetworkGone() {
    network_client_.reset();
    if (!finished_ && !completion_)
      OnComplete(network::URLLoaderCompletionStatus(net::ERR_ABORTED));
  }
  void Gone() { delete this; }

  base::WeakPtr<zebkit_view> view_;
  std::string id_;
  std::string frame_;
  bool main_frame_;
  bool worker_;
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
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Remote<network::mojom::URLLoader> network_;
  mojo::Receiver<network::mojom::URLLoaderClient> network_client_{this};
  base::WeakPtrFactory<Loader> weak_factory_{this};
};

// Deletes itself when the page's factory and its clones go. Loaders keep the
// network's factory for as long as they want it.
class Factory : public network::mojom::URLLoaderFactory {
 public:
  Factory(base::WeakPtr<zebkit_view> view, std::string frame, bool main_frame, bool worker,
          mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
          mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : view_(std::move(view)), frame_(std::move(frame)), main_frame_(main_frame), worker_(worker),
        target_(base::MakeRefCounted<Target>()) {
    Factories().insert(this);
    target_->factory.Bind(std::move(target));
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
    (new Loader(view_, frame_, main, worker_, target_, std::move(loader), request_id, options, request,
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
  std::string frame_;
  bool main_frame_;
  bool worker_;
  scoped_refptr<Target> target_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
};

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

}  // namespace

void InstallNetworkHooks() { Requests::Get().Start(); }
void StopNetworkHooks() {
  while (!Loaders().empty())
    delete *Loaders().begin();
  while (!Factories().empty())
    delete *Factories().begin();
  Requests::Get().Stop();
}
void CancelNetworkHookRequests(zebkit_view* view) { Requests::Get().CancelAll(view, "cancelled"); }

void InterceptNetworkHookFactory(content::RenderFrameHost* frame, bool worker,
                                 network::URLLoaderFactoryBuilder& builder) {
  if (!frame || !Requests::Get().Ready())
    return;
  zebkit_view* view = ViewOf(content::WebContents::FromRenderFrameHost(frame));
  if (!view)
    return;
  // A popup keeps the proxy its opener's policy gave it after that opener goes.
  zebkit_view* decider = view->Decider(&zebkit_view::network_rules_);
  if (decider && decider->network_ever_)
    view->network_ever_ = true;
  if (!view->network_ever_)
    return;
  auto [receiver, target] = builder.Append();
  new Factory(view->GetWeakPtr(), "f" + base::NumberToString(frame->GetFrameTreeNodeId().value()),
              !frame->GetParentOrOuterDocument(), worker, std::move(receiver), std::move(target));
}

}  // namespace zebkit

extern "C" {
bool zebkit_view_set_network_filter(zebkit_view* view, const char* json, char** error) {
  using namespace zebkit;
  auto fail = [&](const std::string& message) { if (error) *error = strdup(message.c_str()); return false; };
  if (!Requests::Get().Ready() || !view || !view->web_contents()) return fail("the view is closed");
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
      if (*stage != "request" && *stage != "response") return fail("stage is request or response");
      rule.request_stage = *stage == "request";
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
void zebkit_view_on_network(zebkit_view* view, zebkit_network_func function, void* data) {
  zebkit::Requests::Get().Install(view, function, data);
}
void zebkit_view_on_network_event(zebkit_view* view, zebkit_network_event_func function, void* data) {
  zebkit::Requests::Get().Observe(view, function, data);
}
bool zebkit_view_set_network_timeout(zebkit_view* view, uint32_t milliseconds) {
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
  const bool response_stage = stage && *stage == "response";
  if (!action) return fail("a reply has an action");
  if (body_size && *action != "fulfill") return fail("only fulfill takes a body");
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
        // What the network refuses a page outright, closing all its requests.
        if (!network::IsRequestHeaderSafe(name, value)) return fail("the network does not allow this header");
        set.SetHeader(name, value);
      }
      std::string forbidden;
      net::HttpRequestHeaders checked = set;
      if (network::ContainsForbiddenSecurityHeader(checked, &forbidden)) return fail("the network does not allow this header");
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
}  // extern "C"
