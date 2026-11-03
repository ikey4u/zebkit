// zebkit's automation, what a page's run leaves (automation_trace.h).

#ifdef UNSAFE_BUFFERS_BUILD
// libvpx's and FFmpeg's buffers are C's.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/automation_trace.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/hash/sha1.h"
#include "base/json/json_writer.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/no_destructor.h"
#include "net/base/url_util.h"
#include "third_party/re2/src/re2/re2.h"
#include "url/gurl.h"
#include "zebkit/engine/automation_network.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "media/ffmpeg/ffmpeg_common.h"
#include "third_party/libvpx/source/libvpx/vpx/vp8cx.h"
#include "third_party/libvpx/source/libvpx/vpx/vpx_encoder.h"
#include "third_party/libyuv/include/libyuv/convert_from_argb.h"
#include "third_party/libyuv/include/libyuv/scale_argb.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/zlib/google/zip.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

std::string Json(const base::Value& value) {
  return base::WriteJson(value).value_or("null");
}

// Where an export goes: @path, or a new file in the temporary directory.
base::FilePath OutputPath(base::FilePath path, const char* extension) {
  if (!path.empty())
    return path;
  base::FilePath directory;
  base::GetTempDir(&directory);
  return directory.AppendASCII("zebkit-" + base::NumberToString(base::RandUint64()) +
                               extension);
}

void Reply(WrittenCallback done, base::FilePath path, bool ok) {
  std::move(done).Run(path, ok ? std::string() : "cannot write " + path.AsUTF8Unsafe());
}

std::string Iso8601(base::Time time) {
  base::Time::Exploded parts;
  time.UTCExplode(&parts);
  char text[32];
  snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", parts.year,
           parts.month, parts.day_of_month, parts.hour, parts.minute,
           parts.second, parts.millisecond);
  return text;
}

base::ListValue Headers(const base::DictValue& from) {
  const base::ListValue* headers = from.FindList("headers");
  return headers ? headers->Clone() : base::ListValue();
}

}  // namespace

// ---- HAR ----------------------------------------------------------------

namespace {

std::string HeaderValue(const base::ListValue* headers, std::string_view name) {
  if (!headers)
    return std::string();
  for (const base::Value& header : *headers) {
    const std::string* key = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
    const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
    if (key && value && base::EqualsCaseInsensitiveASCII(*key, name))
      return *value;
  }
  return std::string();
}

// isomorphic/mimeType.ts's isTextualMimeType().
bool IsTextualMimeType(const std::string& mime) {
  static const base::NoDestructor<RE2> textual(
      R"(^(text\/.*?|application\/(json|(x-)?javascript|xml.*?|ecmascript|graphql|x-www-form-urlencoded)|image\/svg(\+xml)?|application\/.*?(\+json|\+xml))(;\s*charset=.*)?$)");
  return RE2::FullMatch(mime, *textual);
}

// The extension the mime package gives the contents' files.
std::string Extension(const std::string& mime) {
  const std::string type = base::ToLowerASCII(mime.substr(0, mime.find(';')));
  static const std::pair<const char*, const char*> kKnown[] = {
      {"text/html", "html"},         {"text/css", "css"},
      {"text/javascript", "js"},     {"application/javascript", "js"},
      {"application/json", "json"},  {"text/plain", "txt"},
      {"image/png", "png"},          {"image/jpeg", "jpeg"},
      {"image/gif", "gif"},          {"image/svg+xml", "svg"},
      {"image/webp", "webp"},        {"image/x-icon", "ico"},
      {"font/woff", "woff"},         {"font/woff2", "woff2"},
      {"application/xml", "xml"},    {"text/xml", "xml"},
      {"application/pdf", "pdf"},    {"application/octet-stream", "bin"},
      {"application/x-www-form-urlencoded", "dat"}, {"multipart/form-data", "dat"},
  };
  for (const auto& [name, extension] : kKnown) {
    if (type == name)
      return extension;
  }
  return "dat";
}

double Roundish(double value) {
  return static_cast<double>(static_cast<int64_t>(value * 1000)) / 1000;
}

std::optional<std::string> IsoOf(const std::string& text) {
  // As JavaScript's new Date() reads it (harTracer.ts): a time zone written
  // as letters is only one V8 knows; another makes no date.
  static constexpr const char* kZones[] = {"GMT", "UTC", "UT", "Z", "EST", "EDT",
                                           "CST", "CDT", "MST", "MDT", "PST", "PDT"};
  for (const std::string& token : base::SplitString(text, " ", base::TRIM_WHITESPACE,
                                                     base::SPLIT_WANT_NONEMPTY)) {
    if (token.size() < 2 || !std::all_of(token.begin(), token.end(), base::IsAsciiUpper<char>))
      continue;
    if (std::find_if(std::begin(kZones), std::end(kZones),
                     [&](const char* zone) { return token == zone; }) == std::end(kZones)) {
      return std::nullopt;
    }
  }
  base::Time time;
  if (!base::Time::FromString(text.c_str(), &time))
    return std::nullopt;
  return Iso8601(time);
}

// harTracer.ts's parseCookie().
base::DictValue ParseCookie(const std::string& text) {
  base::DictValue cookie;
  cookie.Set("name", "");
  cookie.Set("value", "");
  bool first = true;
  for (const std::string& pair : base::SplitStringUsingSubstr(text, ";", base::KEEP_WHITESPACE,
                                                              base::SPLIT_WANT_ALL)) {
    std::string part(base::TrimWhitespaceASCII(pair, base::TRIM_LEADING));
    const size_t equals = part.find('=');
    const std::string name(base::TrimWhitespaceASCII(
        equals == std::string::npos ? part : part.substr(0, equals), base::TRIM_ALL));
    const std::string value(base::TrimWhitespaceASCII(
        equals == std::string::npos ? std::string() : part.substr(equals + 1), base::TRIM_ALL));
    if (first) {
      first = false;
      cookie.Set("name", name);
      cookie.Set("value", value);
      continue;
    }
    const std::string key = base::ToLowerASCII(name);
    if (key == "domain") {
      cookie.Set("domain", value);
    } else if (key == "expires") {
      if (std::optional<std::string> iso = IsoOf(value))
        cookie.Set("expires", *iso);
    } else if (key == "httponly") {
      cookie.Set("httpOnly", true);
    } else if (key == "max-age") {
      int seconds = 0;
      if (base::StringToInt(value, &seconds))
        cookie.Set("expires", Iso8601(base::Time::Now() + base::Seconds(seconds)));
    } else if (key == "path") {
      cookie.Set("path", value);
    } else if (key == "samesite") {
      cookie.Set("sameSite", value);
    } else if (key == "secure") {
      cookie.Set("secure", true);
    }
  }
  return cookie;
}

std::string HttpVersion(const base::DictValue& response) {
  const std::string* version = response.FindString("httpVersion");
  return "HTTP/" + (version ? *version : std::string("1.1"));
}

std::string Unescape(const std::string& text) {
  return base::UnescapeURLComponent(
      text, base::UnescapeRule::SPACES | base::UnescapeRule::PATH_SEPARATORS |
                base::UnescapeRule::URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS |
                base::UnescapeRule::REPLACE_PLUS_WITH_SPACE);
}

// name=value&... as URLSearchParams reads it, from text that may be anything
// (a page's post data): no URL parser in between.
base::ListValue FormPairs(std::string_view text) {
  base::ListValue pairs;
  for (const std::string& part : base::SplitString(text, "&", base::KEEP_WHITESPACE,
                                                    base::SPLIT_WANT_NONEMPTY)) {
    const size_t equals = part.find('=');
    base::DictValue pair;
    pair.Set("name", Unescape(equals == std::string::npos ? part : part.substr(0, equals)));
    pair.Set("value", equals == std::string::npos ? std::string() : Unescape(part.substr(equals + 1)));
    pairs.Append(std::move(pair));
  }
  return pairs;
}

base::ListValue QueryString(const GURL& url) {
  return url.is_valid() && url.has_query() ? FormPairs(url.query()) : base::ListValue();
}

}  // namespace

HarRecorder::HarRecorder(const base::DictValue& options) {
  if (const std::string* content = options.FindString("content"))
    content_ = *content;
  const std::string* mode = options.FindString("mode");
  minimal_ = mode && *mode == "minimal";
  if (const std::string* glob = options.FindString("urlGlob"))
    url_glob_ = NetworkGlobToRegex(*glob);
  if (const std::string* regex = options.FindString("urlRegexSource")) {
    const std::string* flags = options.FindString("urlRegexFlags");
    url_regex_ = (flags && flags->find('i') != std::string::npos ? "(?i)" : "") + *regex;
  }
  if (const std::string* prefix = options.FindString("resourcesPrefix"))
    resources_prefix_ = *prefix;
  omit_scripts_ = options.FindBool("omitScripts").value_or(false);
  // harRecorder.ts: with harPath and resourcesDir, a content's _file is its
  // path from the HAR's folder; the archive keeps it by its own name, which
  // the client unpacks into resourcesDir.
  const std::string* har_path = options.FindString("harPath");
  const std::string* resources_dir = options.FindString("resourcesDir");
  if (har_path && resources_dir) {
    base::FilePath relative;
    const base::FilePath from = base::FilePath::FromUTF8Unsafe(*har_path).DirName();
    const base::FilePath to = base::FilePath::FromUTF8Unsafe(*resources_dir);
    if (from == to) {
      file_ref_prefix_.clear();
    } else if (from.AppendRelativePath(to, &relative)) {
      file_ref_prefix_ = relative.AsUTF8Unsafe() + "/";
    } else {
      // Not under the HAR's folder: from it, as path.relative() writes it.
      std::vector<base::FilePath::StringType> up = from.GetComponents(), down = to.GetComponents();
      size_t common = 0;
      while (common < up.size() && common < down.size() && up[common] == down[common])
        ++common;
      std::string prefix;
      for (size_t i = common; i < up.size(); ++i)
        prefix += "../";
      for (size_t i = common; i < down.size(); ++i)
        prefix += base::FilePath(down[i]).AsUTF8Unsafe() + "/";
      file_ref_prefix_ = prefix;
    }
  }
}

HarRecorder::~HarRecorder() = default;
HarRecorder::HarRecorder(HarRecorder&&) = default;
HarRecorder& HarRecorder::operator=(HarRecorder&&) = default;

bool HarRecorder::Includes(const std::string& url) const {
  if (!url_glob_.empty() && !RE2::PartialMatch(url, RE2(url_glob_)))
    return false;
  if (!url_regex_.empty() && !RE2::PartialMatch(url, RE2(url_regex_)))
    return false;
  return true;
}

void HarRecorder::AddPage(const std::string& page_ref) {
  if (minimal_ || page_ref.empty())
    return;
  for (const Page& page : pages_) {
    if (page.ref == page_ref)
      return;
  }
  pages_.push_back({page_ref, base::Time::Now()});
}

void HarRecorder::PageLoadState(const std::string& page_ref,
                                const std::string& state,
                                const std::string& title) {
  for (Page& page : pages_) {
    if (page.ref != page_ref)
      continue;
    page.title = title;
    const double since = (base::Time::Now() - page.started).InMillisecondsF();
    if (state == "domcontentloaded")
      page.on_content_load = since;
    else if (state == "load")
      page.on_load = since;
  }
}

void HarRecorder::Record(const std::string& event,
                         const base::DictValue& params,
                         const std::string& page_ref,
                         const std::string& frame_ref) {
  const std::string* id = params.FindString("request");
  if (!id)
    return;
  if (event == "Page.request") {
    const std::string* url = params.FindString("url");
    if (!url || !Includes(*url))
      return;
    AddPage(page_ref);
    Entry& entry = entries_[*id];
    entry.started = base::Time::Now();
    entry.start = base::TimeTicks::Now();
    entry.page_ref = page_ref;
    const std::string* frame = params.FindString("frame");
    entry.frame_ref = !frame_ref.empty() ? frame_ref : frame ? *frame : std::string();
    entry.request = params.Clone();
    order_.push_back(*id);
    if (const std::string* from = params.FindString("redirectedFrom")) {
      auto previous = entries_.find(*from);
      if (previous != entries_.end())
        previous->second.redirect_url = *url;
    }
    return;
  }
  auto found = entries_.find(*id);
  if (found == entries_.end())
    return;
  Entry& entry = found->second;
  if (event == "Page.response") {
    entry.response = params.Clone();
  } else if (event == "Page.requestContinued") {
    entry.overrides = params.Clone();
  } else if (event == "Page.requestResourceType") {
    if (const std::string* type = params.FindString("resourceType"))
      entry.request.Set("resourceType", *type);
  } else if (event == "Page.requestExtraInfo") {
    if (const base::ListValue* headers = params.FindList("headers"))
      entry.raw_request_headers = headers->Clone();
  } else if (event == "Page.responseExtraInfo") {
    if (const base::ListValue* headers = params.FindList("headers"))
      entry.raw_response_headers = headers->Clone();
  } else if (event == "Page.requestFinished" || event == "Page.requestFailed") {
    entry.ended = base::TimeTicks::Now();
    entry.finished_params = params.Clone();
    if (const std::string* failure = params.FindString("failureText"))
      entry.failure = *failure;
  }
}

void HarRecorder::RecordFetch(const base::DictValue& request, const base::DictValue& response) {
  const std::string* url = request.FindString("url");
  if (!url || !Includes(*url))
    return;
  // harTracer.ts's _onAPIRequest: an entry for each exchange, a redirect's
  // without a body, with the headers that went out.
  const base::ListValue* hops = response.FindList("hops");
  if (hops && !hops->empty()) {
    const base::ListValue* sent = response.FindList("requestHeaders");
    for (size_t i = 0; i < hops->size(); ++i) {
      const base::DictValue& hop = (*hops)[i].GetDict();
      const bool last = i + 1 == hops->size();
      base::DictValue hop_request = request.Clone();
      if (const std::string* hop_url = hop.FindString("url"))
        hop_request.Set("url", *hop_url);
      if (sent && !sent->empty())
        hop_request.Set("headers", sent->Clone());
      if (i > 0) {
        // A redirect is followed with GET and no body (303, 302 of a POST).
        const int status = (*hops)[i - 1].GetDict().FindInt("status").value_or(0);
        const std::string* method = hop_request.FindString("method");
        if ((status == 303 || ((status == 301 || status == 302) && method && *method == "POST"))) {
          hop_request.Set("method", "GET");
          hop_request.Remove("postData");
        }
      }
      base::DictValue hop_response = hop.Clone();
      if (last) {
        if (const std::string* body = response.FindString("body"))
          hop_response.Set("body", *body);
      }
      if (last) {
        base::DictValue single = response.Clone();
        single.Remove("hops");
        hop_response.Merge(std::move(single));
      }
      RecordOneFetch(hop_request, hop_response, last);
    }
    return;
  }
  RecordOneFetch(request, response, true);
}

void HarRecorder::RecordOneFetch(const base::DictValue& request, const base::DictValue& response, bool with_body) {
  const std::string id = "fetch" + base::NumberToString(++next_fetch_);
  Entry& entry = entries_[id];
  entry.fetch = true;
  entry.started = base::Time::Now();
  entry.start = entry.ended = base::TimeTicks::Now();
  entry.request = request.Clone();
  entry.response = response.Clone();
  entry.response.Remove("body");
  entry.finished_params.Set("request", id);
  entry.response.Remove("requestHeaders");
  // Its time: until its response's headers, as the network measured it.
  if (std::optional<double> received = response.FindDoubleByDottedPath("timing.responseStart");
      received && *received > 0) {
    entry.ended = entry.start + base::Milliseconds(*received);
  } else if (std::optional<double> duration = response.FindDouble("duration"); duration && *duration > 0) {
    entry.ended = entry.start + base::Milliseconds(*duration);
  }
  if (const std::string* body = with_body ? response.FindString("body") : nullptr) {
    std::string bytes;
    if (base::Base64Decode(*body, &bytes)) {
      entry.has_body = true;
      entry.body = std::move(bytes);
    }
  }
  order_.push_back(id);
}

void HarRecorder::SetBody(const std::string& request, std::optional<std::string> body) {
  auto found = entries_.find(request);
  if (found == entries_.end())
    return;
  found->second.has_body = true;
  found->second.body = std::move(body);
}

bool HarRecorder::WantsBody(const std::string& request) const {
  return entries_.contains(request);
}

base::DictValue HarRecorder::EntryValue(const std::string& id,
                                        const Entry& entry,
                                        const BodyFunc& body_func,
                                        std::map<std::string, std::string>* files) const {
  const base::DictValue& given = entry.overrides ? *entry.overrides : entry.request;
  const std::string* url_text = given.FindString("url");
  if (!url_text)
    url_text = entry.request.FindString("url");
  const std::string url = url_text ? *url_text : std::string();
  const std::string* method = given.FindString("method");
  if (!method)
    method = entry.request.FindString("method");
  // The network's own headers when it said them (harTracer.ts's
  // internalRawRequestHeaders), else what the page sent.
  const base::ListValue* request_headers =
      entry.raw_request_headers && !entry.overrides ? &*entry.raw_request_headers : given.FindList("headers");
  if (!request_headers)
    request_headers = entry.request.FindList("headers");
  const bool responded = !entry.response.empty();
  const std::string http_version = responded ? HttpVersion(entry.response) : "HTTP/1.1";
  const base::DictValue* sizes = entry.finished_params.FindDict("sizes");

  // What a client's file of the contents is called, and the file.
  auto blob = [&](const std::string& bytes, const std::string& mime) {
    const std::string name = resources_prefix_ +
                             base::ToLowerASCII(base::HexEncode(base::SHA1Hash(base::as_byte_span(bytes)))) +
                             "." + Extension(mime);
    if (files)
      (*files)[name] = bytes;
    // Relative to the HAR file when the contents go elsewhere (resourcesDir).
    return file_ref_prefix_ + name;
  };

  base::DictValue request;
  request.Set("method", method ? *method : "GET");
  request.Set("url", url);
  request.Set("httpVersion", http_version);
  base::ListValue request_cookies;
  // The page's own Cookie, not a route's override (harTracer.ts records the
  // overrides apart).
  const base::ListValue* cookie_headers =
      entry.overrides ? entry.request.FindList("headers") : request_headers;
  if (!minimal_ && cookie_headers) {
    for (const base::Value& header : *cookie_headers) {
      const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
      if (!name || !value || !base::EqualsCaseInsensitiveASCII(*name, "cookie"))
        continue;
      for (const std::string& part : base::SplitString(*value, ";", base::KEEP_WHITESPACE,
                                                        base::SPLIT_WANT_ALL)) {
        request_cookies.Append(ParseCookie(part));
      }
    }
  }
  request.Set("cookies", std::move(request_cookies));
  request.Set("headers", request_headers ? request_headers->Clone() : base::ListValue());
  request.Set("queryString", QueryString(GURL(url)));
  request.Set("headersSize", !minimal_ && sizes ? sizes->FindDouble("requestHeadersSize").value_or(-1) : -1);
  std::string post_data;
  const std::string* post = given.FindString("postData");
  if (!post)
    post = entry.request.FindString("postData");
  const bool has_post = post && base::Base64Decode(*post, &post_data);
  request.Set("bodySize", minimal_ ? -1 : static_cast<double>(has_post ? post_data.size() : 0));
  if (has_post) {
    // harTracer.ts's _postDataForBuffer().
    std::string mime = HeaderValue(request_headers, "content-type");
    if (mime.empty())
      mime = "application/octet-stream";
    base::DictValue data;
    data.Set("mimeType", mime);
    data.Set("text", content_ == "embed" && mime != "application/octet-stream" ? post_data : "");
    if (content_ == "attach")
      data.Set("_file", blob(post_data, mime));
    data.Set("params", mime == "application/x-www-form-urlencoded" ? FormPairs(post_data)
                                                                     : base::ListValue());
    request.Set("postData", std::move(data));
  }

  base::DictValue response;
  response.Set("status", responded ? entry.response.FindInt("status").value_or(-1) : -1);
  const std::string* status_text = entry.response.FindString("statusText");
  response.Set("statusText", status_text ? *status_text : "");
  response.Set("httpVersion", http_version);
  const base::ListValue* response_headers =
      entry.raw_response_headers ? &*entry.raw_response_headers : entry.response.FindList("headers");
  base::ListValue response_cookies;
  if (!minimal_ && response_headers) {
    for (const base::Value& header : *response_headers) {
      const std::string* name = header.is_dict() ? header.GetDict().FindString("name") : nullptr;
      const std::string* value = header.is_dict() ? header.GetDict().FindString("value") : nullptr;
      if (!name || !value || !base::EqualsCaseInsensitiveASCII(*name, "set-cookie"))
        continue;
      // Some report several cookies in one header, a line each.
      for (const std::string& line : base::SplitString(*value, "\n", base::TRIM_WHITESPACE,
                                                        base::SPLIT_WANT_NONEMPTY)) {
        response_cookies.Append(ParseCookie(line));
      }
    }
  }
  response.Set("cookies", std::move(response_cookies));
  response.Set("headers", response_headers ? response_headers->Clone() : base::ListValue());
  base::DictValue content;
  std::string mime = HeaderValue(response_headers, "content-type");
  if (mime.empty())
    mime = "x-unknown";
  content.Set("size", -1);
  content.Set("mimeType", mime);
  std::optional<std::string> bytes;
  if (entry.has_body)
    bytes = entry.body;
  else if (responded && body_func && !entry.fetch)
    bytes = body_func.Run(id);
  const double encoded = sizes ? sizes->FindDouble("responseBodySize").value_or(-1) : -1;
  if (bytes && responded) {
    if (!minimal_) {
      content.Set("size", static_cast<double>(bytes->size()));
      if (encoded >= 0)
        content.Set("compression", std::max(0.0, static_cast<double>(bytes->size()) - encoded));
    }
    const std::string* type = entry.request.FindString("resourceType");
    if (content_ == "embed") {
      if (IsTextualMimeType(mime) && !(type && *type == "font")) {
        content.Set("text", *bytes);
      } else {
        content.Set("text", base::Base64Encode(*bytes));
        content.Set("encoding", "base64");
      }
    } else if (content_ == "attach" &&
               !(omit_scripts_ && entry.request.FindString("resourceType") &&
                 *entry.request.FindString("resourceType") == "script")) {
      // A trace's omits scripts' (harTracer.ts's omitScripts).
      content.Set("_file", blob(*bytes, mime));
    }
  }
  response.Set("content", std::move(content));
  response.Set("headersSize", !minimal_ && sizes ? sizes->FindDouble("responseHeadersSize").value_or(-1) : -1);
  response.Set("bodySize", minimal_ ? -1
                           : entry.fetch && bytes ? static_cast<double>(bytes->size())
                                                  : encoded);
  response.Set("redirectURL", entry.redirect_url.empty()
                                  ? (entry.fetch ? HeaderValue(response_headers, "location") : "")
                                  : entry.redirect_url);
  if (!minimal_) {
    const double headers_size = sizes ? sizes->FindDouble("responseHeadersSize").value_or(-1) : -1;
    response.Set("_transferSize", headers_size >= 0 && encoded >= 0 ? headers_size + encoded : -1);
  }
  if (!entry.failure.empty())
    response.Set("_failureText", entry.failure);

  base::DictValue har;
  if (!minimal_ && !entry.page_ref.empty())
    har.Set("pageref", entry.page_ref);
  base::Time started = entry.started;
  const base::DictValue* timing = entry.response.FindDict("timing");
  if (timing) {
    if (std::optional<double> start = timing->FindDouble("startTime"); start && *start > 0)
      started = base::Time::FromMillisecondsSinceUnixEpoch(*start);
  }
  har.Set("startedDateTime", Iso8601(started));
  base::DictValue timings;
  double time = -1;
  if (minimal_ || !timing) {
    timings.Set("send", -1);
    timings.Set("wait", -1);
    timings.Set("receive", -1);
    if (!minimal_ && !entry.ended.is_null())
      time = (entry.ended - entry.start).InMillisecondsF();
  } else {
    auto at = [&](const char* key) { return timing->FindDouble(key).value_or(-1); };
    const double dns = at("domainLookupEnd") != -1 ? Roundish(at("domainLookupEnd") - at("domainLookupStart")) : -1;
    const double connect = at("connectEnd") != -1 ? Roundish(at("connectEnd") - at("connectStart")) : -1;
    const double ssl = at("connectEnd") != -1 ? Roundish(at("connectEnd") - at("secureConnectionStart")) : -1;
    const double wait = at("responseStart") != -1 ? Roundish(at("responseStart") - at("requestStart")) : -1;
    const double end = entry.finished_params.FindDouble("responseEndTiming").value_or(-1);
    const double receive = !entry.ended.is_null() && entry.failure.empty() && end != -1
                               ? Roundish(end - at("responseStart"))
                               : -1;
    timings.Set("dns", dns);
    timings.Set("connect", connect);
    timings.Set("ssl", ssl);
    timings.Set("send", 0);
    timings.Set("wait", wait);
    timings.Set("receive", receive);
    time = 0;
    for (double part : {dns, connect, ssl, wait, receive}) {
      if (part > 0)
        time += part;
    }
  }
  har.Set("time", time);
  har.Set("request", std::move(request));
  har.Set("response", std::move(response));
  har.Set("cache", base::DictValue());
  har.Set("timings", std::move(timings));
  if (!minimal_) {
    if (const std::string* address = entry.response.FindString("serverAddr"))
      har.Set("serverIPAddress", *address);
    if (std::optional<int> port = entry.response.FindInt("serverPort"))
      har.Set("_serverPort", *port);
    // harTracer.ts: the response's security details, {} without TLS.
    if (const base::DictValue* security = entry.response.FindDict("securityDetails"))
      har.Set("_securityDetails", security->Clone());
    else if (responded && !entry.fetch)
      har.Set("_securityDetails", base::DictValue());
  }
  if (!entry.frame_ref.empty())
    har.Set("_frameref", entry.frame_ref);
  if (const std::string* type = entry.request.FindString("resourceType"))
    har.Set("_resourceType", *type);
  return har;
}

base::ListValue HarRecorder::Entries(const BodyFunc& body,
                                     std::map<std::string, std::string>* files) const {
  base::ListValue entries;
  for (const std::string& id : order_) {
    auto found = entries_.find(id);
    if (found != entries_.end())
      entries.Append(EntryValue(id, found->second, body, files));
  }
  return entries;
}

void HarRecorder::Export(base::FilePath path,
                         const BodyFunc& body,
                         const std::string& browser_version,
                         const std::string& creator_version,
                         bool archive,
                         WrittenCallback done) const {
  base::DictValue creator;
  // As Playwright's har tracer, whose format and readers this follows.
  creator.Set("name", "Playwright");
  creator.Set("version", creator_version);
  base::DictValue browser;
  browser.Set("name", "chromium");
  browser.Set("version", browser_version);
  base::DictValue log;
  log.Set("version", "1.2");
  log.Set("creator", std::move(creator));
  log.Set("browser", std::move(browser));
  if (!pages_.empty()) {
    base::ListValue pages;
    for (const Page& page : pages_) {
      base::DictValue entry;
      entry.Set("startedDateTime", Iso8601(page.started));
      entry.Set("id", page.ref);
      entry.Set("title", page.title);
      base::DictValue timings;
      timings.Set("onContentLoad", page.on_content_load);
      timings.Set("onLoad", page.on_load);
      entry.Set("pageTimings", std::move(timings));
      pages.Append(std::move(entry));
    }
    log.Set("pages", std::move(pages));
  }
  std::map<std::string, std::string> files;
  base::ListValue entries;
  for (const std::string& id : order_) {
    auto found = entries_.find(id);
    if (found != entries_.end())
      entries.Append(EntryValue(id, found->second, body, &files));
  }
  log.Set("entries", std::move(entries));
  base::DictValue har;
  har.Set("log", std::move(log));
  path = OutputPath(path, archive ? ".zip" : ".har");
  std::string text = Json(base::Value(std::move(har)));
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path, bool archive, std::string text,
             std::map<std::string, std::string> files) {
            if (!archive) {
              // The attached contents beside the HAR, as harRecorder.ts writes them.
              for (const auto& [name, bytes] : files) {
                if (!base::WriteFile(path.DirName().AppendASCII(name), bytes))
                  return false;
              }
              return base::WriteFile(path, text);
            }
            base::ScopedTempDir directory;
            if (!directory.CreateUniqueTempDir() ||
                !base::WriteFile(directory.GetPath().AppendASCII("har.har"), text)) {
              return false;
            }
            for (const auto& [name, bytes] : files) {
              if (!base::WriteFile(directory.GetPath().AppendASCII(name), bytes))
                return false;
            }
            return zip::Zip(directory.GetPath(), path, /*include_hidden_files=*/true);
          },
          path, archive, std::move(text), std::move(files)),
      base::BindOnce(&Reply, std::move(done), path));
}

// ---- Traces -------------------------------------------------------------

TraceRecorder::TraceRecorder(std::string page_id, const base::DictValue& options)
    : page_id_(std::move(page_id)),
      screenshots_(options.FindBool("screenshots").value_or(false)),
      origin_(base::TimeTicks::Now()),
      network_(base::DictValue()) {
  if (const std::string* title = options.FindString("title"))
    title_ = *title;
  StartChunk(title_);
}

TraceRecorder::~TraceRecorder() = default;

double TraceRecorder::Now() const {
  return (base::TimeTicks::Now() - origin_).InMillisecondsF();
}

void TraceRecorder::Line(base::DictValue event) {
  lines_.push_back(Json(base::Value(std::move(event))));
}

void TraceRecorder::StartChunk(const std::string& title) {
  lines_.clear();
  resources_.clear();
  network_ = HarRecorder(base::DictValue());
  base::DictValue event;
  event.Set("version", 10);
  event.Set("type", "context-options");
  event.Set("origin", "library");
  event.Set("browserName", "chromium");
  event.Set("platform",
#if BUILDFLAG(IS_MAC)
            "darwin"
#elif BUILDFLAG(IS_WIN)
            "win32"
#else
            "linux"
#endif
  );
  event.Set("wallTime", base::Time::Now().InMillisecondsFSinceUnixEpoch());
  event.Set("monotonicTime", Now());
  if (!title.empty())
    event.Set("title", title);
  event.Set("options", base::DictValue());
  event.Set("sdkLanguage", "javascript");
  Line(std::move(event));
}

std::string TraceRecorder::Before(const std::string& method,
                                  const base::DictValue& params) {
  const std::string call_id = "call@" + base::NumberToString(++next_call_);
  const size_t dot = method.find('.');
  base::DictValue event;
  event.Set("type", "before");
  event.Set("callId", call_id);
  event.Set("startTime", Now());
  event.Set("class", method.substr(0, dot));
  event.Set("method", dot == std::string::npos ? method : method.substr(dot + 1));
  event.Set("params", params.Clone());
  event.Set("pageId", page_id_);
  Line(std::move(event));
  return call_id;
}

void TraceRecorder::After(const std::string& call_id, const std::string& error) {
  base::DictValue event;
  event.Set("type", "after");
  event.Set("callId", call_id);
  event.Set("endTime", Now());
  if (!error.empty()) {
    base::DictValue failure;
    failure.Set("message", error);
    failure.Set("name", error.rfind("Timeout ", 0) == 0 ? "TimeoutError" : "Error");
    event.Set("error", std::move(failure));
  }
  Line(std::move(event));
}

void TraceRecorder::Log(const std::string& call_id, const std::string& message) {
  base::DictValue event;
  event.Set("type", "log");
  event.Set("callId", call_id);
  event.Set("time", Now());
  event.Set("message", message);
  Line(std::move(event));
}

void TraceRecorder::Screencast(std::vector<uint8_t> jpeg, int width, int height) {
  const std::string name =
      page_id_ + "-" + base::NumberToString(base::TimeTicks::Now().since_origin().InMicroseconds()) +
      ".jpeg";
  base::DictValue event;
  event.Set("type", "screencast-frame");
  event.Set("pageId", page_id_);
  event.Set("file", name);
  event.Set("width", width);
  event.Set("height", height);
  event.Set("timestamp", Now());
  resources_["resources/" + name] = std::move(jpeg);
  Line(std::move(event));
}

void TraceRecorder::Event(const std::string& event, const base::DictValue& params) {
  network_.Record(event, params);
  if (event != "BrowserContext.console")
    return;
  base::DictValue console;
  console.Set("type", "console");
  console.Set("time", Now());
  console.Set("pageId", page_id_);
  const std::string* type = params.FindString("type");
  console.Set("messageType", type ? *type : "log");
  const std::string* text = params.FindString("text");
  console.Set("text", text ? *text : "");
  base::DictValue location;
  if (const base::DictValue* given = params.FindDict("location"))
    location = given->Clone();
  location.Set("columnNumber", 0);
  console.Set("location", std::move(location));
  Line(std::move(console));
}

void TraceRecorder::Export(base::FilePath path,
                           const HarRecorder::BodyFunc& body,
                           WrittenCallback done) {
  std::string trace;
  for (const std::string& line : lines_)
    trace += line + "\n";
  std::string network;
  base::ListValue entries = network_.Entries(body);
  for (base::Value& entry : entries) {
    base::DictValue event;
    event.Set("type", "resource-snapshot");
    event.Set("snapshot", std::move(entry));
    network += Json(base::Value(std::move(event))) + "\n";
  }
  path = OutputPath(path, ".zip");
  std::map<std::string, std::vector<uint8_t>> files = std::move(resources_);
  files["trace.trace"] = std::vector<uint8_t>(trace.begin(), trace.end());
  files["trace.network"] = std::vector<uint8_t>(network.begin(), network.end());
  StartChunk(title_);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(
          [](base::FilePath path,
             std::map<std::string, std::vector<uint8_t>> files) {
            base::ScopedTempDir directory;
            if (!directory.CreateUniqueTempDir())
              return false;
            for (const auto& [name, bytes] : files) {
              const base::FilePath file = directory.GetPath().AppendASCII(name);
              if (!base::CreateDirectory(file.DirName()) ||
                  !base::WriteFile(file, bytes)) {
                return false;
              }
            }
            return zip::Zip(directory.GetPath(), path, /*include_hidden_files=*/true);
          },
          path, std::move(files)),
      base::BindOnce(&Reply, std::move(done), path));
}

// ---- Video ----------------------------------------------------------------

// The encoder and the file, on a sequence of the thread pool's.
struct VideoRecorder::Encoder {
  Encoder(base::FilePath path, int width, int height)
      : path(std::move(path)), width(width & ~1), height(height & ~1) {}

  ~Encoder() {
    if (ready)
      vpx_codec_destroy(&codec);
    if (image)
      vpx_img_free(image);
    if (format) {
      if (format->pb)
        avio_closep(&format->pb);
      avformat_free_context(format);
    }
  }

  bool Open() {
    vpx_codec_enc_cfg_t config;
    if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), &config, 0) != VPX_CODEC_OK)
      return false;
    config.g_w = width;
    config.g_h = height;
    config.g_timebase.num = 1;
    config.g_timebase.den = 1000;
    config.rc_target_bitrate = 1000;
    config.g_lag_in_frames = 0;
    config.g_threads = 2;
    config.kf_max_dist = 120;
    if (vpx_codec_enc_init(&codec, vpx_codec_vp8_cx(), &config, 0) != VPX_CODEC_OK)
      return false;
    ready = true;
    image = vpx_img_alloc(nullptr, VPX_IMG_FMT_I420, width, height, 1);
    if (!image)
      return false;
    if (avformat_alloc_output_context2(&format, nullptr, "webm",
                                       path.AsUTF8Unsafe().c_str()) < 0 ||
        !format) {
      return false;
    }
    stream = avformat_new_stream(format, nullptr);
    if (!stream)
      return false;
    stream->time_base = AVRational{1, 1000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id = AV_CODEC_ID_VP8;
    stream->codecpar->width = width;
    stream->codecpar->height = height;
    if (avio_open(&format->pb, path.AsUTF8Unsafe().c_str(), AVIO_FLAG_WRITE) < 0)
      return false;
    return avformat_write_header(format, nullptr) >= 0;
  }

  // A frame at @ms, the view's pixels (N32, which is BGRA here: libyuv's
  // ARGB).
  void Frame(int64_t ms, SkBitmap bitmap) {
    if (!ready || !image || bitmap.drawsNothing())
      return;
    std::vector<uint8_t> scaled;
    const uint8_t* pixels = static_cast<const uint8_t*>(bitmap.getPixels());
    int stride = static_cast<int>(bitmap.rowBytes());
    if (bitmap.width() != width || bitmap.height() != height) {
      scaled.resize(static_cast<size_t>(width) * height * 4);
      libyuv::ARGBScale(pixels, stride, bitmap.width(), bitmap.height(),
                        scaled.data(), width * 4, width, height,
                        libyuv::kFilterBilinear);
      pixels = scaled.data();
      stride = width * 4;
    }
    libyuv::ARGBToI420(pixels, stride, image->planes[0], image->stride[0],
                       image->planes[1], image->stride[1], image->planes[2],
                       image->stride[2], width, height);
    if (ms <= last_ms)
      ms = last_ms + 1;
    const int64_t duration = last_ms < 0 ? 40 : ms - last_ms;
    last_ms = ms;
    if (vpx_codec_encode(&codec, image, ms, duration, 0, VPX_DL_REALTIME) != VPX_CODEC_OK)
      return;
    Drain();
  }

  void Drain() {
    vpx_codec_iter_t iterator = nullptr;
    while (const vpx_codec_cx_pkt_t* packet = vpx_codec_get_cx_data(&codec, &iterator)) {
      if (packet->kind != VPX_CODEC_CX_FRAME_PKT)
        continue;
      AVPacket* av = av_packet_alloc();
      if (av_new_packet(av, static_cast<int>(packet->data.frame.sz)) < 0) {
        av_packet_free(&av);
        continue;
      }
      memcpy(av->data, packet->data.frame.buf, packet->data.frame.sz);
      av->stream_index = stream->index;
      av->pts = av->dts = av_rescale_q(packet->data.frame.pts, AVRational{1, 1000},
                                       stream->time_base);
      if (packet->data.frame.flags & VPX_FRAME_IS_KEY)
        av->flags |= AV_PKT_FLAG_KEY;
      av_interleaved_write_frame(format, av);
      av_packet_free(&av);
    }
  }

  bool Finish() {
    if (!ready || !format)
      return false;
    vpx_codec_encode(&codec, nullptr, last_ms + 1, 1, 0, VPX_DL_REALTIME);
    Drain();
    const bool written = av_write_trailer(format) >= 0;
    avio_closep(&format->pb);
    return written;
  }

  base::FilePath path;
  int width;
  int height;
  vpx_codec_ctx_t codec;
  bool ready = false;
  vpx_image_t* image = nullptr;
  AVFormatContext* format = nullptr;
  AVStream* stream = nullptr;
  int64_t last_ms = -1;
};

namespace {

scoped_refptr<base::SequencedTaskRunner> VideoSequence() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}));
  return *runner;
}

}  // namespace

VideoRecorder::VideoRecorder(content::WebContents* contents,
                             base::FilePath path,
                             int width,
                             int height)
    : contents_(contents->GetWeakPtr()),
      path_(OutputPath(path, ".webm")),
      start_(base::TimeTicks::Now()) {
  auto encoder = std::make_unique<Encoder>(path_, std::max(width, 2), std::max(height, 2));
  // Opened on its sequence before any frame.
  Encoder* raw = encoder.get();
  VideoSequence()->PostTask(FROM_HERE, base::BindOnce(
                                           [](Encoder* encoder) {
                                             if (!encoder->Open())
                                               encoder->ready = false;
                                           },
                                           raw));
  encoder_ = std::move(encoder);
  timer_.Start(FROM_HERE, base::Milliseconds(40),
               base::BindRepeating(&VideoRecorder::Capture, base::Unretained(this)));
}

VideoRecorder::~VideoRecorder() {
  timer_.Stop();
  if (encoder_)
    VideoSequence()->DeleteSoon(FROM_HERE, std::move(encoder_));
}

void VideoRecorder::Capture() {
  if (capturing_ || !contents_)
    return;
  content::RenderWidgetHostView* view = contents_->GetRenderWidgetHostView();
  if (!view)
    return;
  capturing_ = true;
  const base::TimeTicks when = base::TimeTicks::Now();
  view->CopyFromSurface(
      gfx::Rect(), gfx::Size(), base::Seconds(1),
      base::BindOnce(
          [](base::WeakPtr<VideoRecorder> self, base::TimeTicks when,
             const content::CopyFromSurfaceResult& result) {
            if (!self)
              return;
            self->capturing_ = false;
            if (result.has_value())
              self->Captured(when, result->bitmap);
          },
          weak_factory_.GetWeakPtr(), when));
}

void VideoRecorder::Captured(base::TimeTicks when, const SkBitmap& bitmap) {
  if (!encoder_)
    return;
  VideoSequence()->PostTask(
      FROM_HERE,
      base::BindOnce(&Encoder::Frame, base::Unretained(encoder_.get()),
                     (when - start_).InMilliseconds(), bitmap));
}

void VideoRecorder::Stop(WrittenCallback done) {
  timer_.Stop();
  if (!encoder_) {
    std::move(done).Run(path_, "the video could not be made");
    return;
  }
  std::unique_ptr<Encoder> encoder = std::move(encoder_);
  Encoder* raw = encoder.get();
  VideoSequence()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](std::unique_ptr<Encoder> encoder) { return encoder->Finish(); },
          std::move(encoder)),
      base::BindOnce(
          [](base::FilePath path, WrittenCallback done, bool ok) {
            std::move(done).Run(path, ok ? std::string() : "the video could not be written");
          },
          path_, std::move(done)));
  (void)raw;
}

}  // namespace zebkit
