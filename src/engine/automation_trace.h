// zebkit's automation, what a page's run leaves (docs/AUTOMATION.md):
// Playwright's traces (the zip its trace viewer opens: trace.trace,
// trace.network, resources/), HAR files of its network, and video of what it
// showed (WebM, VP8).

#ifndef ZEBKIT_ENGINE_AUTOMATION_TRACE_H_
#define ZEBKIT_ENGINE_AUTOMATION_TRACE_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/values.h"

class SkBitmap;

namespace content {
class WebContents;
}  // namespace content

namespace zebkit {

using WrittenCallback =
    base::OnceCallback<void(const base::FilePath& path, const std::string& error)>;

// A HAR of a page's or a context's network, as Playwright's harTracer.ts
// makes it, from automation's network events (Page.request, Page.response,
// Page.requestContinued, Page.requestFinished, Page.requestFailed) and the
// lifecycle of the pages it records.
class HarRecorder {
 public:
  // Tracing.harStart's options: content ("embed", "omit", "attach"), mode
  // ("full", "minimal"), urlGlob or urlRegexSource and urlRegexFlags.
  explicit HarRecorder(const base::DictValue& options);
  ~HarRecorder();
  HarRecorder(HarRecorder&&);
  HarRecorder& operator=(HarRecorder&&);

  // A network event of the page @page_ref ("" for the recorder's own page),
  // whose frame is @frame_ref when given (else the event's "frame").
  void Record(const std::string& event,
              const base::DictValue& params,
              const std::string& page_ref = std::string(),
              const std::string& frame_ref = std::string());
  // A page to record (log.pages), and its lifecycle: "domcontentloaded" or
  // "load", with its title then.
  void AddPage(const std::string& page_ref);
  void PageLoadState(const std::string& page_ref,
                     const std::string& state,
                     const std::string& title);
  // An APIRequestContext.fetch exchange: its request ({url, method, headers,
  // postData}) and its answer ({url, status, statusText, headers, body}).
  void RecordFetch(const base::DictValue& request, const base::DictValue& response);
  // A response's body, once it has come (or nullopt when it will not).
  void SetBody(const std::string& request, std::optional<std::string> body);
  // Whether @request is recorded and its body is wanted.
  bool WantsBody(const std::string& request) const;

  // The entries, as HAR's (for traces' trace.network too); @body gives a
  // response's body by its request when SetBody() did not.
  using BodyFunc = base::RepeatingCallback<std::optional<std::string>(
      const std::string& request)>;
  // With @files, the attached contents by their names.
  base::ListValue Entries(const BodyFunc& body,
                          std::map<std::string, std::string>* files = nullptr) const;

  // HAR 1.2 to @path (a temporary file when empty); with @archive a zip of
  // it as har.har with the attached contents beside it, which is what a remote
  // client asks for (harExport's mode "archive") and unpacks itself.
  // @creator_version is Playwright's that the HAR is made as.
  void Export(base::FilePath path,
              const BodyFunc& body,
              const std::string& browser_version,
              const std::string& creator_version,
              bool archive,
              WrittenCallback done) const;

 private:
  struct Entry {
    base::Time started;
    base::TimeTicks start;
    base::TimeTicks ended;
    std::string page_ref;
    std::string frame_ref;
    base::DictValue request;
    base::DictValue response;
    base::DictValue finished_params;
    std::optional<base::DictValue> overrides;
    std::optional<base::ListValue> raw_request_headers;
    std::optional<base::ListValue> raw_response_headers;
    std::string redirect_url;
    std::string failure;
    bool has_body = false;
    std::optional<std::string> body;
    bool fetch = false;
  };
  struct Page {
    std::string ref;
    base::Time started;
    std::string title;
    double on_content_load = -1;
    double on_load = -1;
  };
  bool Includes(const std::string& url) const;
  base::DictValue EntryValue(const std::string& id,
                             const Entry& entry,
                             const BodyFunc& body,
                             std::map<std::string, std::string>* files) const;

  std::string content_ = "embed";
  // Before an attached content's name: traces' "resources/".
  std::string resources_prefix_;
  bool omit_scripts_ = false;
  bool minimal_ = false;
  std::string url_glob_;
  std::string url_regex_;
  std::map<std::string, Entry> entries_;
  std::vector<std::string> order_;
  std::vector<Page> pages_;
  int next_fetch_ = 0;
};

// A Playwright trace of a page's commands (before and after each, its call
// log, its error), its screencast (a JPEG after each command), its console
// and its network.
class TraceRecorder {
 public:
  TraceRecorder(std::string page_id, const base::DictValue& options);
  ~TraceRecorder();

  bool screenshots() const { return screenshots_; }

  std::string Before(const std::string& method, const base::DictValue& params);
  void After(const std::string& call_id, const std::string& error);
  void Log(const std::string& call_id, const std::string& message);
  void Screencast(std::vector<uint8_t> jpeg, int width, int height);
  void Event(const std::string& event, const base::DictValue& params);

  // The chunk so far, archived to @path (a temporary .zip when empty); a
  // new chunk starts after.
  void Export(base::FilePath path,
              const HarRecorder::BodyFunc& body,
              WrittenCallback done);
  // A new chunk, named @title.
  void StartChunk(const std::string& title);

 private:
  double Now() const;
  void Line(base::DictValue event);

  std::string page_id_;
  std::string title_;
  bool screenshots_ = false;
  base::TimeTicks origin_;
  int next_call_ = 0;
  std::vector<std::string> lines_;
  std::map<std::string, std::vector<uint8_t>> resources_;
  HarRecorder network_;
};

// Video of a page (Playwright's recordVideo): frames of the view's surface,
// 25 a second, encoded as VP8 by libvpx and written as WebM by FFmpeg.
class VideoRecorder {
 public:
  VideoRecorder(content::WebContents* contents,
                base::FilePath path,
                int width,
                int height);
  ~VideoRecorder();

  bool ok() const { return encoder_ != nullptr; }
  const base::FilePath& path() const { return path_; }

  // The file finished; @done once it is written.
  void Stop(WrittenCallback done);

 private:
  struct Encoder;
  void Capture();
  void Captured(base::TimeTicks when, const SkBitmap& bitmap);

  base::WeakPtr<content::WebContents> contents_;
  base::FilePath path_;
  base::TimeTicks start_;
  base::RepeatingTimer timer_;
  bool capturing_ = false;
  std::unique_ptr<Encoder> encoder_;
  base::WeakPtrFactory<VideoRecorder> weak_factory_{this};
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_AUTOMATION_TRACE_H_
