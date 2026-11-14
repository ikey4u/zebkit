// zebkit.* calls from a page, the browser's side: each comes over the
// frame's own channel (mojom::PageCallHost, page_call_renderer.cc has the
// renderer's side), so the browser knows which frame, and which origin, a
// call comes from without asking the page. Many calls are under way at once.
//
// A call is its module and method, its arguments as JSON (UTF-8) and the
// bytes the JSON refers to as {"$part": [offset, length]}; the answer is the
// same. Past 64 KiB each goes through shared memory rather than in the
// message. A zebkit.app call without bytes is not parsed here: its JSON is
// checked, as it is taken out of the shared memory, and handed to the
// application as it is; so is the application's answer, back to the page.

#ifdef UNSAFE_BUFFERS_BUILD
// JSON is read, and parts are taken, by offset.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>
#include <string.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "zebkit/engine/call.h"
#include "zebkit/engine/library_page.h"
#include "zebkit/engine/page_call.mojom.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

// One JSON value, as RFC 8259 has it, that can go on as it is: UTF-8, no
// \u escape of a lone surrogate (JSONReader makes it U+FFFD), at most as
// deep as JSONReader takes. A string is scanned eight bytes at a time.
class JsonCheck {
 public:
  explicit JsonCheck(std::string_view text)
      : p_(reinterpret_cast<const uint8_t*>(text.data())),
        end_(p_ + text.size()) {}

  bool Run(bool array) {
    Space();
    if (array && (p_ == end_ || *p_ != '['))
      return false;
    if (!Value(0))
      return false;
    Space();
    return p_ == end_;
  }

  // A key "$bytes", or one with an escape that could be it.
  bool bytes_key() const { return bytes_key_; }

 private:
  static constexpr int kMaxDepth = 200;

  void Space() {
    while (p_ < end_ &&
           (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) {
      ++p_;
    }
  }

  bool Value(int depth) {
    if (p_ == end_)
      return false;
    switch (*p_) {
      case '{':
        return Object(depth + 1);
      case '[':
        return Array(depth + 1);
      case '"':
        return String(nullptr);
      case 't':
        return Word("true");
      case 'f':
        return Word("false");
      case 'n':
        return Word("null");
      default:
        return Number();
    }
  }

  bool Array(int depth) {
    if (depth > kMaxDepth)
      return false;
    ++p_;
    Space();
    if (p_ < end_ && *p_ == ']') {
      ++p_;
      return true;
    }
    for (;;) {
      if (!Value(depth))
        return false;
      Space();
      if (p_ == end_)
        return false;
      if (*p_ == ']') {
        ++p_;
        return true;
      }
      if (*p_++ != ',')
        return false;
      Space();
    }
  }

  bool Object(int depth) {
    if (depth > kMaxDepth)
      return false;
    ++p_;
    Space();
    if (p_ < end_ && *p_ == '}') {
      ++p_;
      return true;
    }
    for (;;) {
      if (p_ == end_ || *p_ != '"')
        return false;
      const uint8_t* key = p_ + 1;
      bool escaped = false;
      if (!String(&escaped))
        return false;
      const std::string_view name(reinterpret_cast<const char*>(key),
                                  p_ - 1 - key);
      if (escaped || name == "$bytes")
        bytes_key_ = true;
      Space();
      if (p_ == end_ || *p_++ != ':')
        return false;
      Space();
      if (!Value(depth))
        return false;
      Space();
      if (p_ == end_)
        return false;
      if (*p_ == '}') {
        ++p_;
        return true;
      }
      if (*p_++ != ',')
        return false;
      Space();
    }
  }

  bool String(bool* escaped) {
    ++p_;
    constexpr uint64_t kOnes = 0x0101010101010101ull;
    constexpr uint64_t kHigh = 0x8080808080808080ull;
    for (;;) {
      // Eight bytes that are none of '"', '\\', a control or past ASCII.
      while (end_ - p_ >= 8) {
        uint64_t word;
        memcpy(&word, p_, 8);
        const uint64_t quote = word ^ (kOnes * '"');
        const uint64_t slash = word ^ (kOnes * '\\');
        const uint64_t special = ((quote - kOnes) & ~quote) |
                                 ((slash - kOnes) & ~slash) |
                                 ((word - kOnes * 0x20) & ~word) | word;
        if (special & kHigh)
          break;
        p_ += 8;
      }
      if (p_ == end_)
        return false;
      const uint8_t c = *p_;
      if (c == '"') {
        ++p_;
        return true;
      }
      if (c < 0x20)
        return false;
      if (c == '\\') {
        if (escaped)
          *escaped = true;
        if (!Escape())
          return false;
      } else if (c < 0x80) {
        ++p_;
      } else if (!Utf8()) {
        return false;
      }
    }
  }

  bool Hex(uint32_t* unit) {
    if (end_ - p_ < 4)
      return false;
    *unit = 0;
    for (int i = 0; i < 4; ++i) {
      const uint8_t c = *p_++;
      uint32_t digit;
      if (c >= '0' && c <= '9')
        digit = c - '0';
      else if (c >= 'a' && c <= 'f')
        digit = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F')
        digit = c - 'A' + 10;
      else
        return false;
      *unit = *unit << 4 | digit;
    }
    return true;
  }

  bool Escape() {
    ++p_;
    if (p_ == end_)
      return false;
    switch (*p_++) {
      case '"':
      case '\\':
      case '/':
      case 'b':
      case 'f':
      case 'n':
      case 'r':
      case 't':
        return true;
      case 'u': {
        uint32_t unit;
        if (!Hex(&unit) || (unit >= 0xdc00 && unit <= 0xdfff))
          return false;
        if (unit < 0xd800 || unit > 0xdbff)
          return true;
        if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u')
          return false;
        p_ += 2;
        return Hex(&unit) && unit >= 0xdc00 && unit <= 0xdfff;
      }
      default:
        return false;
    }
  }

  // One character past ASCII, as Unicode allows it in UTF-8.
  bool Utf8() {
    const uint8_t lead = *p_;
    int follow;
    uint8_t low = 0x80, high = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
      follow = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      follow = 2;
      if (lead == 0xe0)
        low = 0xa0;
      else if (lead == 0xed)
        high = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      follow = 3;
      if (lead == 0xf0)
        low = 0x90;
      else if (lead == 0xf4)
        high = 0x8f;
    } else {
      return false;
    }
    if (end_ - p_ <= follow)
      return false;
    if (p_[1] < low || p_[1] > high)
      return false;
    for (int i = 2; i <= follow; ++i) {
      if (p_[i] < 0x80 || p_[i] > 0xbf)
        return false;
    }
    p_ += follow + 1;
    return true;
  }

  bool Digits() {
    const uint8_t* start = p_;
    while (p_ < end_ && *p_ >= '0' && *p_ <= '9')
      ++p_;
    return p_ != start;
  }

  bool Number() {
    if (p_ < end_ && *p_ == '-')
      ++p_;
    if (p_ < end_ && *p_ == '0')
      ++p_;
    else if (!Digits())
      return false;
    if (p_ < end_ && *p_ == '.') {
      ++p_;
      if (!Digits())
        return false;
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-'))
        ++p_;
      if (!Digits())
        return false;
    }
    return true;
  }

  bool Word(std::string_view word) {
    if (static_cast<size_t>(end_ - p_) < word.size() ||
        memcmp(p_, word.data(), word.size()) != 0) {
      return false;
    }
    p_ += word.size();
    return true;
  }

  const uint8_t* p_;
  const uint8_t* const end_;
  bool bytes_key_ = false;
};

// {"$part": [offset, length]} in @value, from @parts, as binary values.
bool TakeParts(base::Value& value, std::string_view parts) {
  if (value.is_list()) {
    for (base::Value& item : value.GetList()) {
      if (!TakeParts(item, parts))
        return false;
    }
    return true;
  }
  if (!value.is_dict())
    return true;
  base::DictValue& dict = value.GetDict();
  if (dict.size() == 1) {
    if (const base::ListValue* part = dict.FindList("$part")) {
      if (part->size() != 2 || !(*part)[0].is_int() || !(*part)[1].is_int())
        return false;
      const int64_t offset = (*part)[0].GetInt();
      const int64_t length = (*part)[1].GetInt();
      if (offset < 0 || length < 0 ||
          offset + length > static_cast<int64_t>(parts.size())) {
        return false;
      }
      const auto* data = reinterpret_cast<const uint8_t*>(parts.data());
      value = base::Value(
          base::Value::BlobStorage(data + offset, data + offset + length));
      return true;
    }
  }
  for (auto [key, item] : dict) {
    if (!TakeParts(item, parts))
      return false;
  }
  return true;
}

// Binary values in @value to {"$part": [offset, length]}, their bytes
// appended to @parts.
base::Value GiveParts(base::Value value, std::string* parts) {
  if (value.is_blob()) {
    const base::Value::BlobStorage& blob = value.GetBlob();
    base::ListValue part;
    part.Append(static_cast<int>(parts->size()));
    part.Append(static_cast<int>(blob.size()));
    parts->append(reinterpret_cast<const char*>(blob.data()), blob.size());
    base::DictValue dict;
    dict.Set("$part", std::move(part));
    return base::Value(std::move(dict));
  }
  if (value.is_list()) {
    for (base::Value& item : value.GetList())
      item = GiveParts(std::move(item), parts);
    return value;
  }
  if (value.is_dict()) {
    for (auto [key, item] : value.GetDict())
      item = GiveParts(std::move(item), parts);
    return value;
  }
  return value;
}

mojo_base::BigBuffer Buffer(std::string_view bytes) {
  return mojo_base::BigBuffer(base::as_byte_span(bytes));
}

// The answer to one call, which the call's reply and its text reply share:
// whichever runs first answers.
class Answer : public base::RefCounted<Answer> {
 public:
  explicit Answer(mojom::PageCallHost::CallCallback callback)
      : callback_(std::move(callback)) {}

  void Settle(bool ok, base::Value value) {
    if (!callback_)
      return;
    if (!ok) {
      Fail(value.is_string() ? value.GetString()
                             : base::WriteJson(value).value_or("failed"));
      return;
    }
    std::string parts;
    const std::string json =
        base::WriteJson(GiveParts(std::move(value), &parts)).value_or("null");
    std::move(callback_).Run(true, Buffer(json), Buffer(parts));
  }

  void SettleText(bool ok, std::string_view text) {
    if (callback_)
      std::move(callback_).Run(ok, Buffer(text), mojo_base::BigBuffer());
  }

  void Fail(const std::string& message) { SettleText(false, message); }

 private:
  friend class base::RefCounted<Answer>;
  ~Answer() {
    // A reply dropped unanswered: the page is not left waiting.
    Fail("the call was not answered");
  }

  mojom::PageCallHost::CallCallback callback_;
};

class PageCallHost : public mojom::PageCallHost {
 public:
  static PageCallHost& Get() {
    static base::NoDestructor<PageCallHost> host;
    return *host;
  }

  void Bind(content::GlobalRenderFrameHostId frame,
            mojo::PendingAssociatedReceiver<mojom::PageCallHost> receiver) {
    receivers_.Add(this, std::move(receiver), frame);
  }

  void Call(const std::string& module,
            const std::string& method,
            mojo_base::BigBuffer json,
            mojo_base::BigBuffer parts,
            CallCallback callback) override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    content::RenderFrameHost* frame =
        content::RenderFrameHost::FromID(receivers_.current_context());
    // The runner of a JavaScript library's page (library_page.cc).
    if (TakeLibraryPageCall(frame, module, method, json, parts, callback))
      return;
    auto answer = base::MakeRefCounted<Answer>(std::move(callback));
    zebkit_view* view =
        frame ? ViewOf(content::WebContents::FromRenderFrameHost(frame))
              : nullptr;
    if (!view) {
      answer->Fail("zebkit is not available in this frame");
      return;
    }
    // Copied out of the memory the renderer shares before it is checked.
    std::string text(reinterpret_cast<const char*>(json.data()), json.size());
    PageCall call;
    call.view = view;
    call.module = module;
    call.method = method;
    if (module == "app" && !parts.size() && IsPlainJson(text, true)) {
      call.args_json = std::move(text);
    } else {
      std::optional<base::Value> args =
          base::JSONReader::Read(text, base::JSON_PARSE_RFC);
      const std::string_view bytes(reinterpret_cast<const char*>(parts.data()),
                                   parts.size());
      if (!args || !args->is_list() || !TakeParts(*args, bytes)) {
        answer->Fail("not a call");
        return;
      }
      call.args = std::move(args->GetList());
    }
    call.reply = base::BindOnce(&Answer::Settle, answer);
    call.text_reply = base::BindOnce(&Answer::SettleText, answer);
    view->TakeCall(frame, std::move(call));
  }

 private:
  mojo::AssociatedReceiverSet<mojom::PageCallHost,
                              content::GlobalRenderFrameHostId>
      receivers_;
};

}  // namespace

bool IsPlainJson(std::string_view text, bool array, bool* bytes_key) {
  JsonCheck check(text);
  if (!check.Run(array))
    return false;
  if (bytes_key)
    *bytes_key = check.bytes_key();
  return true;
}

void BindPageCallHost(
    content::RenderFrameHost* frame,
    mojo::PendingAssociatedReceiver<mojom::PageCallHost> receiver) {
  PageCallHost::Get().Bind(frame->GetGlobalId(), std::move(receiver));
}

}  // namespace zebkit
