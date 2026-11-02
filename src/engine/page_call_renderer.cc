// The page's side of zebkit.* calls (call.cc has the browser's): a native
// function in each frame's own world, __zebkit_call(module, method, json,
// parts), which sends the call over the frame's channel to the browser and
// answers with a promise of [ok, text, bytes]. zebkit's bridge script
// (view.cc) takes the function and deletes it before any of the page's own
// script runs; where the view has no bridge, it is deleted here, so a page
// never sees it.
//
// The arguments go as their UTF-8, copied once into the message or, past
// 64 KiB, into shared memory; bytes the same, each view copied once. No
// JSON is parsed or written here, nor in the browser for zebkit.app: the
// browser checks it and hands it on (call.cc).

#ifdef UNSAFE_BUFFERS_BUILD
// Strings and bytes are copied by length.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>
#include <string.h>

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/common/isolated_world_ids.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/converter.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/page_call.mojom.h"

namespace zebkit {

namespace {

constexpr char kFunction[] = "__zebkit_call";

class Agent;

std::map<content::RenderFrame*, Agent*>& Agents() {
  static base::NoDestructor<std::map<content::RenderFrame*, Agent*>> agents;
  return *agents;
}

// @string as UTF-8: copied as it is when it is ASCII, as JSON nearly
// always is.
mojo_base::BigBuffer Utf8(v8::Isolate* isolate, v8::Local<v8::String> string) {
  std::string converted;
  {
    v8::String::ValueView view(isolate, string);
    if (view.is_one_byte()) {
      const auto* data = reinterpret_cast<const char*>(view.data8());
      const std::string_view text(data, view.length());
      if (base::IsStringASCII(text)) {
        mojo_base::BigBuffer out(text.size());
        memcpy(out.data(), text.data(), text.size());
        return out;
      }
      converted.reserve(text.size() * 2);
      for (unsigned char c : text) {
        if (c < 0x80) {
          converted.push_back(static_cast<char>(c));
        } else {
          converted.push_back(static_cast<char>(0xc0 | (c >> 6)));
          converted.push_back(static_cast<char>(0x80 | (c & 0x3f)));
        }
      }
    } else {
      // A lone surrogate becomes U+FFFD; JSON.stringify writes none.
      base::UTF16ToUTF8(reinterpret_cast<const char16_t*>(view.data16()),
                        view.length(), &converted);
    }
  }
  return mojo_base::BigBuffer(base::as_byte_span(converted));
}

// JSON of an array of strings, numbers, booleans and nulls, written
// straight into the buffer that goes to the browser: no string of it in
// the page's heap first, as JSON.stringify would make. It writes what
// JSON.stringify writes, but for a lone surrogate, which becomes U+FFFD
// here as in Utf8().
class JsonWriter {
 public:
  // The bytes @value takes, or nothing for what is not a primitive.
  static std::optional<size_t> Size(v8::Isolate* isolate,
                                    v8::Local<v8::Context> context,
                                    v8::Local<v8::Value> value,
                                    std::string* number) {
    if (value->IsString()) {
      v8::String::ValueView view(isolate, value.As<v8::String>());
      return 2 + (view.is_one_byte()
                      ? Count(view.data8(), view.length())
                      : Count(view.data16(), view.length()));
    }
    if (value->IsNumber()) {
      *number = Number(isolate, context, value);
      return number->size();
    }
    if (value->IsBoolean())
      return value->IsTrue() ? 4 : 5;
    if (value->IsNull() || value->IsUndefined())
      return 4;
    return std::nullopt;
  }

  explicit JsonWriter(uint8_t* out) : out_(out) {}

  void Write(v8::Isolate* isolate,
             v8::Local<v8::Value> value,
             const std::string& number) {
    if (value->IsString()) {
      v8::String::ValueView view(isolate, value.As<v8::String>());
      Put('"');
      if (view.is_one_byte())
        Escape(view.data8(), view.length());
      else
        Escape(view.data16(), view.length());
      Put('"');
    } else if (value->IsNumber()) {
      Put(number);
    } else if (value->IsBoolean()) {
      Put(value->IsTrue() ? "true" : "false");
    } else {
      Put("null");
    }
  }

  void Put(char c) { *out_++ = static_cast<uint8_t>(c); }
  void Put(std::string_view text) {
    memcpy(out_, text.data(), text.size());
    out_ += text.size();
  }

 private:
  static std::string Number(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            v8::Local<v8::Value> value) {
    const double number = value.As<v8::Number>()->Value();
    if (!std::isfinite(number))
      return "null";
    v8::Local<v8::String> text;
    std::string out;
    if (value->ToString(context).ToLocal(&text))
      gin::ConvertFromV8(isolate, text, &out);
    return out.empty() ? "null" : out;
  }

  // What a character takes in JSON's UTF-8, escaped as JSON.stringify does.
  static size_t Escaped(uint32_t c) {
    if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' ||
        c == '\r' || c == '\t') {
      return 2;
    }
    if (c < 0x20)
      return 6;
    return c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
  }

  // Eight bytes of one-byte text with nothing to escape and nothing past
  // ASCII.
  static bool Plain8(const uint8_t* p) {
    constexpr uint64_t kOnes = 0x0101010101010101ull;
    constexpr uint64_t kHigh = 0x8080808080808080ull;
    uint64_t word;
    memcpy(&word, p, 8);
    const uint64_t quote = word ^ (kOnes * '"');
    const uint64_t slash = word ^ (kOnes * '\\');
    return !((((quote - kOnes) & ~quote) | ((slash - kOnes) & ~slash) |
              ((word - kOnes * 0x20) & ~word) | word) &
             kHigh);
  }

  static size_t Count(const uint8_t* p, size_t n) {
    size_t size = 0, i = 0;
    while (i < n) {
      if (n - i >= 8 && Plain8(p + i)) {
        i += 8;
        size += 8;
        continue;
      }
      size += Escaped(p[i++]);
    }
    return size;
  }

  static size_t Count(const uint16_t* p, size_t n) {
    size_t size = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint16_t c = p[i];
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && p[i + 1] >= 0xdc00 &&
          p[i + 1] <= 0xdfff) {
        size += 4;
        ++i;
      } else {
        size += Escaped(c);
      }
    }
    return size;
  }

  void Escape(uint32_t c) {
    switch (c) {
      case '"': Put("\\\""); return;
      case '\\': Put("\\\\"); return;
      case '\b': Put("\\b"); return;
      case '\f': Put("\\f"); return;
      case '\n': Put("\\n"); return;
      case '\r': Put("\\r"); return;
      case '\t': Put("\\t"); return;
    }
    if (c < 0x20) {
      static constexpr char kHex[] = "0123456789abcdef";
      Put("\\u00");
      Put(kHex[c >> 4]);
      Put(kHex[c & 0xf]);
    } else if (c < 0x80) {
      Put(static_cast<char>(c));
    } else if (c < 0x800) {
      Put(static_cast<char>(0xc0 | (c >> 6)));
      Put(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
      Put(static_cast<char>(0xe0 | (c >> 12)));
      Put(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      Put(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
      Put(static_cast<char>(0xf0 | (c >> 18)));
      Put(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
      Put(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
      Put(static_cast<char>(0x80 | (c & 0x3f)));
    }
  }

  void Escape(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
      size_t run = i;
      while (n - run >= 8 && Plain8(p + run))
        run += 8;
      if (run > i) {
        memcpy(out_, p + i, run - i);
        out_ += run - i;
        i = run;
        continue;
      }
      Escape(p[i++]);
    }
  }

  void Escape(const uint16_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      uint32_t c = p[i];
      if (c >= 0xd800 && c <= 0xdbff && i + 1 < n && p[i + 1] >= 0xdc00 &&
          p[i + 1] <= 0xdfff) {
        c = 0x10000 + ((c - 0xd800) << 10) + (p[++i] - 0xdc00);
      } else if (c >= 0xd800 && c <= 0xdfff) {
        c = 0xfffd;
      }
      Escape(c);
    }
  }

  uint8_t* out_;
};

// An answer of the browser's as a string of the page's, without a copy:
// the shared memory it came in.
class ExternalAnswer : public v8::String::ExternalOneByteStringResource {
 public:
  explicit ExternalAnswer(mojo_base::BigBuffer buffer)
      : buffer_(std::move(buffer)) {}
  const char* data() const override {
    return reinterpret_cast<const char*>(buffer_.data());
  }
  size_t length() const override { return buffer_.size(); }

 private:
  mojo_base::BigBuffer buffer_;
};

void Reject(v8::Isolate* isolate,
            v8::Local<v8::Context> context,
            v8::Local<v8::Promise::Resolver> resolver,
            const char* message) {
  std::ignore = resolver->Reject(
      context, v8::Exception::TypeError(gin::StringToV8(isolate, message)));
}

class Agent : public content::RenderFrameObserver {
 public:
  explicit Agent(content::RenderFrame* render_frame)
      : content::RenderFrameObserver(render_frame), frame_(render_frame) {
    Agents()[frame_] = this;
  }
  ~Agent() override { Agents().erase(frame_); }

  static Agent* ForContext(v8::Local<v8::Context> context) {
    blink::WebLocalFrame* web_frame =
        blink::WebLocalFrame::FrameForContext(context);
    if (!web_frame || web_frame->MainWorldScriptContext() != context)
      return nullptr;
    auto it = Agents().find(content::RenderFrame::FromWebFrame(web_frame));
    return it == Agents().end() ? nullptr : it->second;
  }

  mojom::PageCallHost* host() {
    if (!host_) {
      frame_->GetRemoteAssociatedInterfaces()->GetInterface(&host_);
      host_.reset_on_disconnect();
    }
    return host_.get();
  }

  // content::RenderFrameObserver
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override {
    if (world_id != content::ISOLATED_WORLD_ID_GLOBAL)
      return;
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Context::Scope scope(context);
    v8::Local<v8::Function> function;
    if (!v8::Function::New(context, &Call, v8::Local<v8::Value>(), 4,
                           v8::ConstructorBehavior::kThrow)
             .ToLocal(&function)) {
      return;
    }
    std::ignore = context->Global()->DefineOwnProperty(
        context, gin::StringToSymbol(isolate, kFunction), function,
        v8::DontEnum);
  }

  // After the scripts the view adds ran, and before any of the page's.
  void DidClearWindowObject() override {
    blink::WebLocalFrame* web_frame = frame_->GetWebFrame();
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::HandleScope handles(isolate);
    v8::Local<v8::Context> context = web_frame->MainWorldScriptContext();
    if (context.IsEmpty())
      return;
    v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kDoNotRunMicrotasks);
    v8::Context::Scope scope(context);
    std::ignore = context->Global()->Delete(
        context, gin::StringToSymbol(isolate, kFunction));
  }

  void OnDestruct() override { delete this; }

 private:
  // __zebkit_call(module, method, json, [bytes...]).
  static void Call(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Promise::Resolver> resolver;
    if (!v8::Promise::Resolver::New(context).ToLocal(&resolver))
      return;
    info.GetReturnValue().Set(resolver->GetPromise());
    Agent* agent = ForContext(context);
    if (!agent || !agent->host()) {
      Reject(isolate, context, resolver,
             "zebkit is not available in this frame");
      return;
    }
    if (info.Length() < 4 || !info[0]->IsString() || !info[1]->IsString() ||
        !(info[2]->IsString() || info[2]->IsArray()) || !info[3]->IsArray()) {
      Reject(isolate, context, resolver, "not a call");
      return;
    }
    std::string module, method;
    gin::ConvertFromV8(isolate, info[0], &module);
    gin::ConvertFromV8(isolate, info[1], &method);
    // The arguments' JSON, or the arguments themselves when each is a
    // string, a number, a boolean or null, written here.
    mojo_base::BigBuffer json;
    if (info[2]->IsString()) {
      json = Utf8(isolate, info[2].As<v8::String>());
    } else {
      v8::Local<v8::Array> args = info[2].As<v8::Array>();
      const uint32_t count = args->Length();
      std::vector<v8::Local<v8::Value>> values(count);
      std::vector<std::string> numbers(count);
      size_t size = 2 + (count ? count - 1 : 0);
      for (uint32_t i = 0; i < count; ++i) {
        std::optional<size_t> bytes;
        if (args->Get(context, i).ToLocal(&values[i]))
          bytes = JsonWriter::Size(isolate, context, values[i], &numbers[i]);
        if (!bytes) {
          Reject(isolate, context, resolver, "not a call");
          return;
        }
        size += *bytes;
      }
      json = mojo_base::BigBuffer(size);
      JsonWriter writer(json.data());
      writer.Put('[');
      for (uint32_t i = 0; i < count; ++i) {
        if (i)
          writer.Put(',');
        writer.Write(isolate, values[i], numbers[i]);
      }
      writer.Put(']');
    }
    v8::Local<v8::Array> views = info[3].As<v8::Array>();
    size_t size = 0;
    for (uint32_t i = 0; i < views->Length(); ++i) {
      v8::Local<v8::Value> view;
      if (!views->Get(context, i).ToLocal(&view) || !view->IsArrayBufferView()) {
        Reject(isolate, context, resolver, "not a call");
        return;
      }
      size += view.As<v8::ArrayBufferView>()->ByteLength();
    }
    mojo_base::BigBuffer parts(size);
    size_t at = 0;
    for (uint32_t i = 0; i < views->Length() && at < size; ++i) {
      v8::Local<v8::Value> view = views->Get(context, i).ToLocalChecked();
      at += view.As<v8::ArrayBufferView>()->CopyContents(parts.data() + at,
                                                         size - at);
    }
    agent->host()->Call(
        module, method, std::move(json), std::move(parts),
        base::BindOnce(&Answer, isolate,
                       v8::Global<v8::Context>(isolate, context),
                       v8::Global<v8::Promise::Resolver>(isolate, resolver)));
  }

  static void Answer(v8::Isolate* isolate,
                     v8::Global<v8::Context> saved_context,
                     v8::Global<v8::Promise::Resolver> saved_resolver,
                     bool ok,
                     mojo_base::BigBuffer answer,
                     mojo_base::BigBuffer answer_parts) {
    v8::HandleScope handles(isolate);
    v8::Local<v8::Context> context = saved_context.Get(isolate);
    // Not into a document that has gone.
    if (!ForContext(context))
      return;
    v8::Context::Scope scope(context);
    // The page's script goes on from its await when this ends.
    v8::MicrotasksScope microtasks(isolate, context->GetMicrotaskQueue(),
                                   v8::MicrotasksScope::kRunMicrotasks);
    v8::Local<v8::Promise::Resolver> resolver = saved_resolver.Get(isolate);
    const auto* data = reinterpret_cast<const char*>(answer.data());
    const std::string_view text(data, answer.size());
    v8::Local<v8::String> string;
    bool made = false;
    if (text.size() <= static_cast<size_t>(v8::String::kMaxLength)) {
      if (!base::IsStringASCII(text)) {
        made = v8::String::NewFromUtf8(isolate, text.data(),
                                       v8::NewStringType::kNormal,
                                       static_cast<int>(text.size()))
                   .ToLocal(&string);
      } else if (answer.storage_type() ==
                 mojo_base::BigBuffer::StorageType::kSharedMemory) {
        // Large: the string is the shared memory, not a copy of it.
        made = v8::String::NewExternalOneByte(
                   isolate, new ExternalAnswer(std::move(answer)))
                   .ToLocal(&string);
      } else {
        made = v8::String::NewFromOneByte(
                   isolate, reinterpret_cast<const uint8_t*>(text.data()),
                   v8::NewStringType::kNormal, static_cast<int>(text.size()))
                   .ToLocal(&string);
      }
    }
    if (!made) {
      Reject(isolate, context, resolver, "the answer is too large");
      return;
    }
    v8::Local<v8::Value> bytes = v8::Undefined(isolate);
    if (answer_parts.size()) {
      v8::Local<v8::ArrayBuffer> buffer;
      if (!v8::ArrayBuffer::MaybeNew(
               isolate, answer_parts.size(),
               v8::BackingStoreInitializationMode::kUninitialized)
               .ToLocal(&buffer)) {
        Reject(isolate, context, resolver, "the answer is too large");
        return;
      }
      memcpy(buffer->Data(), answer_parts.data(), answer_parts.size());
      bytes = buffer;
    }
    v8::Local<v8::Value> items[] = {v8::Boolean::New(isolate, ok), string,
                                    bytes};
    std::ignore = resolver->Resolve(
        context, v8::Array::New(isolate, items, std::size(items)));
  }

  content::RenderFrame* const frame_;
  mojo::AssociatedRemote<mojom::PageCallHost> host_;
};

}  // namespace

void InstallPageCallAgent(content::RenderFrame* render_frame) {
  new Agent(render_frame);
}

}  // namespace zebkit
