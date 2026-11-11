// <zebkit/media.h>: a page's media elements, and captures of what they
// play. The renderer's agent of each frame (media_capture_renderer.cc)
// lists its elements and taps a player's demuxer; the packets go straight
// to a CaptureWriter on a sequence that may block
// (media_capture_writer.cc), and its reports come back to the engine's
// thread here.

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/task/bind_post_task.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/sequence_bound.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client_channel.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size_f.h"
#include "zebkit/engine/include/zebkit/media.h"
#include "zebkit/engine/media_capture.h"
#include "zebkit/engine/media_capture.mojom.h"
#include "zebkit/engine/media_capture_writer.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

using Agent = mojo::AssociatedRemote<mojom::MediaCaptureAgent>;

std::unique_ptr<Agent> AgentOf(content::RenderFrameHost* frame) {
  auto agent = std::make_unique<Agent>();
  frame->GetRemoteAssociatedInterfaces()->GetInterface(agent.get());
  return agent;
}

std::string FramePrefix(content::RenderFrameHost* frame) {
  const content::GlobalRenderFrameHostId id = frame->GetGlobalId();
  return base::NumberToString(id.child_id.GetUnsafeValue()) + "." +
         base::NumberToString(id.frame_routing_id) + ".";
}

base::DictValue FrameValue(content::RenderFrameHost* frame) {
  base::DictValue value;
  value.Set("url", frame->GetLastCommittedURL().spec());
  value.Set("main", frame->GetParent() == nullptr);
  value.Set("name", frame->GetFrameName());
  return value;
}

// An element's entry with the media's id and its frame.
base::Value Decorate(content::RenderFrameHost* frame, base::Value entry) {
  if (!entry.is_dict()) {
    return base::Value();
  }
  base::DictValue& dict = entry.GetDict();
  const std::optional<int> element = dict.FindInt("element");
  if (!element) {
    return base::Value();
  }
  dict.Set("id", FramePrefix(frame) + base::NumberToString(*element));
  dict.Set("frame", FrameValue(frame));
  return entry;
}

// The live frames of the view's page.
std::vector<content::RenderFrameHost*> FramesOf(
    content::WebContents* contents) {
  std::vector<content::RenderFrameHost*> frames;
  if (!contents) {
    return frames;
  }
  contents->GetPrimaryMainFrame()->ForEachRenderFrameHost(
      [&](content::RenderFrameHost* frame) {
        if (frame->IsRenderFrameLive()) {
          frames.push_back(frame);
        }
      });
  return frames;
}

// The answers of every frame's agent, put together.
class ListRequest : public base::RefCounted<ListRequest> {
 public:
  explicit ListRequest(MediaJsonCallback done) : done_(std::move(done)) {}

  void Start(const std::vector<content::RenderFrameHost*>& frames) {
    for (content::RenderFrameHost* frame : frames) {
      const size_t index = answers_.size();
      answers_.push_back({frame->GetGlobalId(), std::string("[]")});
      agents_.push_back(AgentOf(frame));
      (*agents_.back())
          ->List(mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(&ListRequest::OnList, base::WrapRefCounted(this),
                             index),
              std::string("[]")));
    }
    pending_ = answers_.size();
    if (!pending_) {
      Deliver();
      return;
    }
    // A renderer that does not answer (hung, or busy for long) is left out.
    timeout_.Start(FROM_HERE, base::Seconds(5),
                   base::BindOnce(&ListRequest::Deliver,
                                  base::WrapRefCounted(this)));
  }

 private:
  friend class base::RefCounted<ListRequest>;
  ~ListRequest() = default;

  struct Answer {
    content::GlobalRenderFrameHostId frame;
    std::string json;
  };

  void OnList(size_t index, const std::string& json) {
    answers_[index].json = json;
    if (--pending_ == 0) {
      Deliver();
    }
  }

  void Deliver() {
    if (delivered_) {
      return;
    }
    delivered_ = true;
    timeout_.Stop();
    base::ListValue list;
    for (const Answer& answer : answers_) {
      content::RenderFrameHost* frame =
          content::RenderFrameHost::FromID(answer.frame);
      std::optional<base::Value> entries = base::JSONReader::Read(answer.json, 0);
      if (!frame || !entries || !entries->is_list()) {
        continue;
      }
      for (base::Value& entry : entries->GetList()) {
        base::Value decorated = Decorate(frame, std::move(entry));
        if (!decorated.is_none()) {
          list.Append(std::move(decorated));
        }
      }
    }
    // The agents keep the answers' callbacks, which keep this; they go
    // once this answer has run (it may be one of theirs).
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::make_unique<std::vector<std::unique_ptr<Agent>>>(
                       std::move(agents_)));
    std::move(done_).Run(base::WriteJson(list).value_or("[]"));
  }

  MediaJsonCallback done_;
  std::vector<Answer> answers_;
  std::vector<std::unique_ptr<Agent>> agents_;
  size_t pending_ = 0;
  bool delivered_ = false;
  base::OneShotTimer timeout_;
};

}  // namespace
}  // namespace zebkit

// A capture, on the engine's thread. It is the application's until
// zebkit_media_capture_free(), and lives on until its files are written.
struct zebkit_media_capture {
  enum class State { kStarting, kCapturing, kFinishing, kDone };

  zebkit_media_capture_func on_progress = nullptr;
  void* progress_data = nullptr;
  zebkit_media_capture_func on_done = nullptr;
  void* done_data = nullptr;

  State state = State::kStarting;
  base::DictValue last;
  base::Value media;
  bool released = false;
  // In the done function, which may free the capture.
  bool delivering = false;
  // The renderer's answer to the start: until then a done report waits,
  // so that a refusal is what the application is told.
  bool replied = false;
  std::string start_error;
  std::optional<std::string> pending_done;

  std::unique_ptr<zebkit::Agent> agent;
  // A renderer that never answers the start (hung) ends the capture.
  base::OneShotTimer start_timeout;
  base::SequenceBound<zebkit::CaptureWriter> writer;

  // The start's answer and its timeout reach the capture only while it is
  // there: the agent, which holds the answer, goes after it may have been
  // freed (DeleteSoon()), and runs the answer as it goes. Last, so that
  // nothing reaches the capture while it is taken apart.
  base::WeakPtrFactory<zebkit_media_capture> weak_factory{this};

  void Stop(const std::string& reason, const std::string& error) {
    if (state == State::kDone || state == State::kFinishing) {
      return;
    }
    state = State::kFinishing;
    writer.AsyncCall(&zebkit::CaptureWriter::Finish)
        .WithArgs(reason, error);
  }

  std::string Status() const {
    base::DictValue status = last.Clone();
    const char* names[] = {"starting", "capturing", "finishing", "done"};
    status.Set("state", names[static_cast<int>(state)]);
    status.Set("media", media.Clone());
    return base::WriteJson(status).value_or("{}");
  }

  void OnStarted(const std::string& error, const std::string& json) {
    if (replied) {
      return;
    }
    replied = true;
    start_timeout.Stop();
    // Not while its answer runs.
    base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(agent));
    if (std::optional<base::Value> entry = base::JSONReader::Read(json, 0)) {
      media = std::move(*entry);
    }
    if (!error.empty()) {
      start_error = error;
      Stop("error", error);
    } else if (state == State::kStarting) {
      state = State::kCapturing;
    }
    if (pending_done) {
      std::string done = std::move(*pending_done);
      pending_done.reset();
      Deliver(done);
    }
  }

  void OnReport(std::string json, bool done) {
    if (!done) {
      if (state == State::kDone) {
        return;
      }
      if (std::optional<base::Value> value = base::JSONReader::Read(json, 0);
          value && value->is_dict()) {
        last = std::move(value->GetDict());
      }
      if (on_progress && !released) {
        on_progress(this, json.c_str(), progress_data);
      }
      return;
    }
    if (!replied) {
      pending_done = std::move(json);
      return;
    }
    Deliver(json);
  }

  void Deliver(const std::string& json) {
    state = State::kDone;
    std::optional<base::Value> value = base::JSONReader::Read(json, 0);
    if (value && value->is_dict()) {
      last = std::move(value->GetDict());
    }
    if (!start_error.empty()) {
      last.Set("error", start_error);
      last.Set("reason", "error");
      last.Set("files", base::ListValue());
    }
    last.Set("media", media.Clone());
    const std::string text = base::WriteJson(last).value_or("{}");
    writer.Reset();
    if (on_done && !released) {
      delivering = true;
      on_done(this, text.c_str(), done_data);
      delivering = false;
    }
    if (released) {
      delete this;
    }
  }
};

namespace {

char* Duplicate(const std::string& text) {
  return strdup(text.c_str());
}

void SetError(char** error, const std::string& message) {
  if (error) {
    *error = Duplicate(message);
  }
}

}  // namespace

namespace zebkit {

namespace {

bool g_keep_history = false;

// Asks frame @next of @order for the media element at @point (the view's
// coordinates), and the frames after it while none has one.
void AskElementAt(base::WeakPtr<content::WebContents> contents,
                  gfx::PointF point,
                  std::vector<content::GlobalRenderFrameHostId> order,
                  size_t next,
                  MediaJsonCallback done) {
  for (; contents && next < order.size(); ++next) {
    content::RenderFrameHost* frame =
        content::RenderFrameHost::FromID(order[next]);
    if (!frame || !frame->IsRenderFrameLive() ||
        content::WebContents::FromRenderFrameHost(frame) != contents.get()) {
      continue;
    }
    content::RenderWidgetHostView* view = frame->GetView();
    gfx::PointF local = point;
    if (frame->GetParent()) {
      if (!view) {
        continue;
      }
      local = view->TransformRootPointToViewCoordSpace(point);
      if (!gfx::RectF(gfx::SizeF(view->GetViewBounds().size()))
               .Contains(local)) {
        continue;
      }
    }
    auto agent = AgentOf(frame);
    Agent* raw = agent.get();
    (*raw)->ElementAt(
        local,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(
                [](base::WeakPtr<content::WebContents> contents,
                   gfx::PointF point,
                   std::vector<content::GlobalRenderFrameHostId> order,
                   size_t next, MediaJsonCallback done,
                   std::unique_ptr<Agent> agent, const std::string& token,
                   const std::string& json) {
                  // The agent goes once its answer has run.
                  base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
                      FROM_HERE, std::move(agent));
                  if (contents && !token.empty()) {
                    for (content::RenderFrameHost* frame :
                         FramesOf(contents.get())) {
                      if (frame->GetFrameToken().ToString() != token) {
                        continue;
                      }
                      std::optional<base::Value> entry =
                          base::JSONReader::Read(json, 0);
                      if (entry) {
                        base::Value decorated =
                            Decorate(frame, std::move(*entry));
                        if (!decorated.is_none()) {
                          std::move(done).Run(
                              base::WriteJson(decorated).value_or("null"));
                          return;
                        }
                      }
                      break;
                    }
                  }
                  AskElementAt(std::move(contents), point, std::move(order),
                               next + 1, std::move(done));
                },
                contents, point, order, next, std::move(done),
                std::move(agent)),
            std::string(), std::string("null")));
    return;
  }
  std::move(done).Run("null");
}

}  // namespace

void AppendMediaSwitch(base::CommandLine* command_line) {
  if (g_keep_history ||
      base::CommandLine::ForCurrentProcess()->HasSwitch(kMediaHistorySwitch)) {
    command_line->AppendSwitch(kMediaHistorySwitch);
  }
}

void ListMedia(content::WebContents* contents, MediaJsonCallback done) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto request = base::MakeRefCounted<ListRequest>(std::move(done));
  request->Start(FramesOf(contents));
}

void MediaAt(content::WebContents* contents,
             float x,
             float y,
             MediaJsonCallback done) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  content::RenderFrameHost* main =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!main || !main->IsRenderFrameLive()) {
    std::move(done).Run("null");
    return;
  }
  // The main frame's widget, then each frame that hit-tests in a widget of
  // its own (an iframe of another site, in another process), the innermost
  // first: the main frame's hit test stops at such an iframe.
  std::vector<std::pair<int, content::GlobalRenderFrameHostId>> roots;
  for (content::RenderFrameHost* frame : FramesOf(contents)) {
    content::RenderFrameHost* parent = frame->GetParent();
    if (!parent ||
        frame->GetRenderWidgetHost() == parent->GetRenderWidgetHost()) {
      continue;
    }
    int depth = 0;
    for (content::RenderFrameHost* up = parent; up; up = up->GetParent()) {
      ++depth;
    }
    roots.emplace_back(-depth, frame->GetGlobalId());
  }
  std::stable_sort(roots.begin(), roots.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<content::GlobalRenderFrameHostId> order = {main->GetGlobalId()};
  for (const auto& [depth, id] : roots) {
    order.push_back(id);
  }
  AskElementAt(contents->GetWeakPtr(), gfx::PointF(x, y), std::move(order), 0,
               std::move(done));
}

zebkit_media_capture* StartMediaCapture(content::WebContents* contents,
                                        const std::string& media_id,
                                        const std::string& path,
                                        const std::string& options_json,
                                        std::string* error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!contents) {
    *error = "the view is closed";
    return nullptr;
  }
  // "<process>.<frame>.<element>"
  std::vector<std::string> parts = base::SplitString(
      media_id, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  int process = 0, routing = 0, element = 0;
  if (parts.size() != 3 || !base::StringToInt(parts[0], &process) ||
      !base::StringToInt(parts[1], &routing) ||
      !base::StringToInt(parts[2], &element)) {
    *error = "not a media id of the media list";
    return nullptr;
  }
  content::RenderFrameHost* frame =
      content::RenderFrameHost::FromID(process, routing);
  if (!frame || content::WebContents::FromRenderFrameHost(frame) != contents ||
      !frame->IsRenderFrameLive()) {
    *error = "the media's frame is gone (list the media again)";
    return nullptr;
  }
  const base::FilePath file = base::FilePath::FromUTF8Unsafe(path);
  if (file.empty() || !file.IsAbsolute() || file.EndsWithSeparator()) {
    *error = "the path must be an absolute path of a file";
    return nullptr;
  }
  CaptureOptions options;
  bool include_buffered = false;
  if (!ParseCaptureOptions(options_json, &options, &include_buffered, error)) {
    return nullptr;
  }

  auto* capture = new zebkit_media_capture();
  mojo::PendingRemote<mojom::MediaCaptureSink> sink;
  auto receiver = sink.InitWithNewPipeAndPassReceiver();
  // Reports come to the engine's thread; the capture is there until its
  // done report.
  auto report = base::BindPostTaskToCurrentDefault(base::BindRepeating(
      [](zebkit_media_capture* capture, std::string json, bool done) {
        capture->OnReport(std::move(json), done);
      },
      base::Unretained(capture)));
  capture->writer = base::SequenceBound<CaptureWriter>(
      base::ThreadPool::CreateSequencedTaskRunner(
          // A capture still running when the engine quits does not hold
          // it up, and is not written (media.h).
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
      file, std::move(options), std::move(receiver), std::move(report));
  capture->agent = AgentOf(frame);
  (*capture->agent)
      ->Start(element, include_buffered, std::move(sink),
              mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                  base::BindOnce(&zebkit_media_capture::OnStarted,
                                 capture->weak_factory.GetWeakPtr()),
                  std::string("the frame went away"), std::string("null")));
  capture->start_timeout.Start(
      FROM_HERE, base::Seconds(10),
      base::BindOnce(&zebkit_media_capture::OnStarted,
                     capture->weak_factory.GetWeakPtr(),
                     std::string("the page did not answer the capture's start"),
                     std::string("null")));
  return capture;
}

// @func's answer, called once: with the JSON, or with null as the view
// closes, before on_close (zebkit_view::AddCancel()).
MediaJsonCallback OnceForView(zebkit_view* view, zebkit_result_func func, void* data) {
  const int id = view->AddCancel(base::BindOnce(
      [](zebkit_view* view, zebkit_result_func func, void* data) {
        if (func)
          func(view, nullptr, "the view was closed", data);
      },
      base::Unretained(view), func, data));
  return base::BindOnce(
      [](base::WeakPtr<zebkit_view> view, int id, zebkit_result_func func,
         void* data, std::string json) {
        if (view && view->TakeCancel(id) && func)
          func(view.get(), json.c_str(), nullptr, data);
      },
      view->GetWeakPtr(), id, func, data);
}

}  // namespace zebkit

extern "C" {

void zebkit_media_keep_history(bool keep) {
  zebkit::g_keep_history = keep;
}

void zebkit_view_media_list(zebkit_view* view,
                            zebkit_result_func func,
                            void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::ListMedia(
      view->web_contents(),
      zebkit::OnceForView(view, func, data));
}

void zebkit_view_media_at(zebkit_view* view,
                          float x,
                          float y,
                          zebkit_result_func func,
                          void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::MediaAt(
      view->web_contents(), x, y,
      zebkit::OnceForView(view, func, data));
}

zebkit_media_capture* zebkit_media_capture_start(zebkit_view* view,
                                                 const char* media_id,
                                                 const char* path,
                                                 const char* options_json,
                                                 char** error) {
  if (error) {
    *error = nullptr;
  }
  if (!zebkit::LiveView(view)) {
    SetError(error, "the view is closed, or this is not the engine's thread");
    return nullptr;
  }
  std::string problem;
  zebkit_media_capture* capture = zebkit::StartMediaCapture(
      view->web_contents(), media_id ? media_id : "",
      path ? path : "", options_json ? options_json : "", &problem);
  if (!capture) {
    SetError(error, problem);
  }
  return capture;
}

void zebkit_media_capture_on_progress(zebkit_media_capture* capture,
                                      zebkit_media_capture_func func,
                                      void* data) {
  if (!capture || !zebkit::CurrentlyOnEngineThread())
    return;
  capture->on_progress = func;
  capture->progress_data = data;
}

void zebkit_media_capture_on_done(zebkit_media_capture* capture,
                                  zebkit_media_capture_func func,
                                  void* data) {
  if (!capture || !zebkit::CurrentlyOnEngineThread())
    return;
  capture->on_done = func;
  capture->done_data = data;
}

void zebkit_media_capture_stop(zebkit_media_capture* capture) {
  if (!capture || !zebkit::CurrentlyOnEngineThread())
    return;
  capture->Stop("stopped", std::string());
}

char* zebkit_media_capture_status(zebkit_media_capture* capture) {
  if (!capture || !zebkit::CurrentlyOnEngineThread())
    return nullptr;
  return Duplicate(capture->Status());
}

void zebkit_media_capture_free(zebkit_media_capture* capture) {
  if (!capture || !zebkit::CurrentlyOnEngineThread()) {
    return;
  }
  capture->released = true;
  if (capture->state == zebkit_media_capture::State::kDone) {
    if (!capture->delivering) {
      delete capture;
    }
    return;
  }
  capture->Stop("stopped", std::string());
}

}  // extern "C"

// ---- Zebkit.media* over CDP --------------------------------------------

namespace zebkit {

namespace {

struct CdpCapture {
  // Null once it is done; its result stays for Zebkit.mediaCaptureStatus.
  raw_ptr<zebkit_media_capture> capture;
  std::string result;
  // The session that started it, the only one that reaches it; null once
  // it detached (OnMediaClientDetached()).
  raw_ptr<content::DevToolsAgentHostClientChannel, DanglingUntriaged> channel;
};

std::map<int, CdpCapture>& CdpCaptures() {
  static base::NoDestructor<std::map<int, CdpCapture>> captures;
  return *captures;
}

void SendResult(content::DevToolsAgentHostClientChannel* channel,
                int id,
                base::DictValue result) {
  base::DictValue message;
  message.Set("id", id);
  message.Set("result", std::move(result));
  SendToDevToolsClient(channel, std::move(message));
}

void SendError(content::DevToolsAgentHostClientChannel* channel,
               int id,
               int code,
               const std::string& text) {
  base::DictValue error;
  error.Set("code", code);
  error.Set("message", text);
  base::DictValue message;
  message.Set("id", id);
  message.Set("error", std::move(error));
  SendToDevToolsClient(channel, std::move(message));
}

base::Value Parsed(const std::string& json) {
  std::optional<base::Value> value = base::JSONReader::Read(json, 0);
  return value ? std::move(*value) : base::Value();
}

void SendCaptureEvent(int capture_id, const char* method, const char* key,
                      const char* json) {
  auto it = CdpCaptures().find(capture_id);
  if (it == CdpCaptures().end()) {
    return;
  }
  base::DictValue params;
  params.Set("captureId", capture_id);
  params.Set(key, Parsed(json ? json : "{}"));
  base::DictValue message;
  message.Set("method", method);
  message.Set("params", std::move(params));
  SendToDevToolsClient(it->second.channel, std::move(message));
}

void OnCdpProgress(zebkit_media_capture* capture,
                   const char* json,
                   void* data) {
  SendCaptureEvent(static_cast<int>(reinterpret_cast<intptr_t>(data)),
                   "Zebkit.mediaCaptureProgress", "state", json);
}

void OnCdpDone(zebkit_media_capture* capture, const char* json, void* data) {
  const int id = static_cast<int>(reinterpret_cast<intptr_t>(data));
  SendCaptureEvent(id, "Zebkit.mediaCaptureDone", "result", json);
  auto it = CdpCaptures().find(id);
  if (it != CdpCaptures().end()) {
    if (!it->second.channel) {
      // Its session is gone: nobody can ask for the result.
      CdpCaptures().erase(it);
    } else {
      it->second.capture = nullptr;
      it->second.result = json ? json : "{}";
    }
  }
  // Clients that poll read the last ones.
  while (CdpCaptures().size() > 64) {
    auto oldest = std::ranges::find_if(
        CdpCaptures(), [](const auto& entry) { return !entry.second.capture; });
    if (oldest == CdpCaptures().end()) {
      break;
    }
    CdpCaptures().erase(oldest);
  }
  zebkit_media_capture_free(capture);
}

}  // namespace

bool IsMediaCommand(std::string_view method) {
  return base::StartsWith(method, "Zebkit.media");
}

void HandleMediaCommand(content::DevToolsAgentHostClientChannel* channel,
                        int id,
                        std::string_view method,
                        const std::string& params_json) {
  base::Value params_value = Parsed(params_json);
  const base::DictValue empty;
  const base::DictValue& params =
      params_value.is_dict() ? params_value.GetDict() : empty;
  // A capture is the session's that started it.
  if (method == "Zebkit.mediaCaptureStop" ||
      method == "Zebkit.mediaCaptureStatus") {
    auto it = CdpCaptures().find(params.FindInt("captureId").value_or(0));
    if (it == CdpCaptures().end() || it->second.channel != channel) {
      SendError(channel, id, -32000, "no such capture");
      return;
    }
    base::DictValue result;
    if (!it->second.capture) {
      result.Set("status", Parsed(it->second.result));
    } else if (method == "Zebkit.mediaCaptureStop") {
      zebkit_media_capture_stop(it->second.capture);
    } else {
      char* status = zebkit_media_capture_status(it->second.capture);
      result.Set("status", Parsed(status));
      free(status);
    }
    SendResult(channel, id, std::move(result));
    return;
  }
  content::DevToolsAgentHost* host = channel->GetAgentHost();
  content::WebContents* contents = host ? host->GetWebContents() : nullptr;
  if (!contents || host->GetType() != content::DevToolsAgentHost::kTypePage) {
    SendError(channel, id, -32000, "Command can only be executed on a page");
    return;
  }
  // The channel may go before an answer: SendToDevToolsClient checks.
  auto answer_media = [](content::DevToolsAgentHostClientChannel* channel,
                         int id, std::string json) {
    base::DictValue result;
    result.Set("media", Parsed(json));
    SendResult(channel, id, std::move(result));
  };
  if (method == "Zebkit.mediaList") {
    ListMedia(contents, base::BindOnce(answer_media, channel, id));
    return;
  }
  if (method == "Zebkit.mediaAt") {
    MediaAt(contents,
            static_cast<float>(params.FindDouble("x").value_or(0)),
            static_cast<float>(params.FindDouble("y").value_or(0)),
            base::BindOnce(answer_media, channel, id));
    return;
  }
  if (method == "Zebkit.mediaCaptureStart") {
    const std::string* media_id = params.FindString("mediaId");
    const std::string* path = params.FindString("path");
    std::string options = "{}";
    if (const base::DictValue* given = params.FindDict("options")) {
      options = base::WriteJson(*given).value_or("{}");
    }
    if (!media_id || !path) {
      SendError(channel, id, -32602, "mediaId and path are needed");
      return;
    }
    std::string error;
    zebkit_media_capture* capture =
        StartMediaCapture(contents, *media_id, *path, options, &error);
    if (!capture) {
      SendError(channel, id, -32000, error);
      return;
    }
    static int next_id = 0;
    const int capture_id = ++next_id;
    CdpCaptures()[capture_id] = {capture, std::string(), channel};
    void* data = reinterpret_cast<void*>(static_cast<intptr_t>(capture_id));
    zebkit_media_capture_on_progress(capture, OnCdpProgress, data);
    zebkit_media_capture_on_done(capture, OnCdpDone, data);
    base::DictValue result;
    result.Set("captureId", capture_id);
    SendResult(channel, id, std::move(result));
    return;
  }
  SendError(channel, id, -32601,
            std::string("'") + std::string(method) + "' wasn't found");
}

void OnMediaClientDetached(content::DevToolsAgentHostClientChannel* channel) {
  for (auto it = CdpCaptures().begin(); it != CdpCaptures().end();) {
    if (it->second.channel != channel) {
      ++it;
      continue;
    }
    if (!it->second.capture) {
      it = CdpCaptures().erase(it);
      continue;
    }
    // Stopped: its files are written, and the entry goes when it is done.
    it->second.channel = nullptr;
    zebkit_media_capture_stop(it->second.capture);
    ++it;
  }
}

}  // namespace zebkit
