// JavaScript libraries as normal libraries: <zebkit/library.h>, the
// backend "isolate" (docs/JS_LIBRARIES.md).
//
// Each library has its instances: threads of their own, each with one V8
// isolate made and used there, and a queue of calls, each taken by the first
// instance free and run there, one at a time on each. The application's
// threads queue and answer the library's requests (host.call()); the engine's
// thread tells (every callback runs there), closes, and watches deadlines: a
// call whose script is still running at its deadline is stopped with
// TerminateExecution(), which any thread may call. An idle instance makes the
// modules' code cache (kept in memory and in the profile) and gives memory back.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and gives C arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <utility>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "gin/array_buffer.h"
#include "gin/public/isolate_holder.h"
#include "url/gurl.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-external.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-typed-array.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/library_bootstrap.h"
#include "zebkit/engine/library_page.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/profile.h"

namespace zebkit {

namespace {

constexpr int kMaxModules = 4096;

// The path absolute with every link resolved (symbolic links, and on Windows
// junctions and short names too); empty when nothing is there. On Windows
// base::MakeAbsoluteFilePath() is only _wfullpath(): it neither resolves a link
// nor asks whether the path exists, so there the final path of the file or
// folder as opened, the way GetFinalPathNameByHandle() tells it.
base::FilePath RealPath(const base::FilePath& path) {
#if BUILDFLAG(IS_WIN)
  base::FilePath real;
  if (!base::NormalizeFilePath(path, &real))
    return base::FilePath();
  return real;
#else
  return base::MakeAbsoluteFilePath(path);
#endif
}

v8::Local<v8::String> String8(v8::Isolate* isolate, std::string_view text) {
  return v8::String::NewFromUtf8(isolate, text.data(), v8::NewStringType::kNormal, static_cast<int>(text.size()))
      .ToLocalChecked();
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

const char* CodeName(zebkit_library_code code) {
  switch (code) {
    case ZEBKIT_LIBRARY_OK: return "OK";
    case ZEBKIT_LIBRARY_THROWN: return "THROWN";
    case ZEBKIT_LIBRARY_LOAD_FAILED: return "LOAD_FAILED";
    case ZEBKIT_LIBRARY_NO_FUNCTION: return "NO_FUNCTION";
    case ZEBKIT_LIBRARY_BAD_ARGUMENTS: return "BAD_ARGUMENTS";
    case ZEBKIT_LIBRARY_BAD_RESULT: return "BAD_RESULT";
    case ZEBKIT_LIBRARY_TIMEOUT: return "TIMEOUT";
    case ZEBKIT_LIBRARY_CANCELLED: return "CANCELLED";
    case ZEBKIT_LIBRARY_QUEUE_FULL: return "QUEUE_FULL";
    case ZEBKIT_LIBRARY_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case ZEBKIT_LIBRARY_GONE: return "GONE";
  }
  return "ERROR";
}

// The JSON of a code the engine gives, not the script.
std::string CodeJson(zebkit_library_code code, const std::string& message) {
  const char* name = code == ZEBKIT_LIBRARY_TIMEOUT     ? "TimeoutError"
                     : code == ZEBKIT_LIBRARY_CANCELLED ? "AbortError"
                                                        : "Error";
  return LibraryErrorJson(message, name);
}

// ArrayBuffers' memory, counted against the library's heap limit: past it
// an allocation fails, which the script sees as a RangeError.
}  // namespace

class CountingAllocator : public v8::ArrayBuffer::Allocator {
 public:
  explicit CountingAllocator(size_t limit) : limit_(limit), inner_(gin::ArrayBufferAllocator::SharedInstance()) {}

  void* Allocate(size_t length) override {
    if (!Reserve(length))
      return nullptr;
    void* data = inner_->Allocate(length);
    if (!data)
      used_.fetch_sub(length);
    return data;
  }
  void* AllocateUninitialized(size_t length) override {
    if (!Reserve(length))
      return nullptr;
    void* data = inner_->AllocateUninitialized(length);
    if (!data)
      used_.fetch_sub(length);
    return data;
  }
  void Free(void* data, size_t length) override {
    inner_->Free(data, length);
    used_.fetch_sub(length);
  }
  size_t MaxAllocationSize() const override { return inner_->MaxAllocationSize(); }
  v8::PageAllocator* GetPageAllocator() override { return inner_->GetPageAllocator(); }

 private:
  bool Reserve(size_t length) {
    size_t used = used_.load();
    do {
      if (length > limit_ || used > limit_ - length)
        return false;
    } while (!used_.compare_exchange_weak(used, used + length));
    return true;
  }

  const size_t limit_;
  std::atomic<size_t> used_{0};
  raw_ptr<v8::ArrayBuffer::Allocator> inner_;
};

namespace {

// An instance's thread for V8's own tasks, run with the isolate entered (as
// script.cc's); those still queued when the isolate goes are dropped.
class EnteringTaskRunner : public base::SingleThreadTaskRunner {
 public:
  explicit EnteringTaskRunner(scoped_refptr<base::SingleThreadTaskRunner> runner) : runner_(std::move(runner)) {}

  void set_isolate(v8::Isolate* isolate) { isolate_ = isolate; }

  bool PostDelayedTask(const base::Location& from_here, base::OnceClosure task, base::TimeDelta delay) override {
    return runner_->PostDelayedTask(from_here, Entering(std::move(task)), delay);
  }
  bool PostNonNestableDelayedTask(const base::Location& from_here, base::OnceClosure task,
                                  base::TimeDelta delay) override {
    return runner_->PostNonNestableDelayedTask(from_here, Entering(std::move(task)), delay);
  }
  bool RunsTasksInCurrentSequence() const override { return runner_->RunsTasksInCurrentSequence(); }

 private:
  ~EnteringTaskRunner() override = default;

  base::OnceClosure Entering(base::OnceClosure task) {
    return base::BindOnce(
        [](scoped_refptr<EnteringTaskRunner> self, base::OnceClosure task) {
          if (!self->isolate_)
            return;
          v8::Isolate::Scope isolate_scope(self->isolate_);
          std::move(task).Run();
        },
        base::WrapRefCounted(this), std::move(task));
  }

  const scoped_refptr<base::SingleThreadTaskRunner> runner_;
  raw_ptr<v8::Isolate> isolate_ = nullptr;
};

bool ParseOptions(const char* json, LibraryOptions* options, std::string* error) {
  if (!json || !*json)
    return true;
  std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) {
    *error = "the options are not a JSON object";
    return false;
  }
  for (const auto [key, value] : parsed->GetDict()) {
    const bool number = value.is_int() || value.is_double();
    const double n = number ? value.GetDouble() : 0;
    if (key == "backend") {
      if (!value.is_string() || (value.GetString() != "isolate" && value.GetString() != "page")) {
        *error = "backend is \"isolate\" or \"page\"";
        return false;
      }
      // The backend "page" (library_page.cc).
      options->page = value.GetString() == "page";
    } else if (key == "timeoutMs") {
      if (!number || n < 1 || n > 86400000) {
        *error = "timeoutMs is a number of milliseconds, 1 to 86400000";
        return false;
      }
      options->timeout_ms = static_cast<int64_t>(n);
    } else if (key == "queue") {
      if (!number || n < 1 || n > 1000000) {
        *error = "queue is a number of calls, 1 to 1000000";
        return false;
      }
      options->queue = static_cast<size_t>(n);
    } else if (key == "heapLimitMB") {
      if (!number || n < 16 || n > 16384) {
        *error = "heapLimitMB is a number, 16 to 16384";
        return false;
      }
      options->heap_limit = static_cast<size_t>(n) << 20;
    } else if (key == "maxBytes") {
      if (!number || n < 1024 || n > 4294967296.0) {
        *error = "maxBytes is a number of bytes, 1024 to 4294967296";
        return false;
      }
      options->max_bytes = static_cast<size_t>(n);
    } else if (key == "network") {
      if (!value.is_list()) {
        *error = "network is a list of origins";
        return false;
      }
      for (const base::Value& item : value.GetList()) {
        GURL url(item.is_string() ? item.GetString() : std::string());
        url::Origin origin = url::Origin::Create(url);
        if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || origin.opaque() ||
            origin.Serialize() != base::TrimString(item.GetString(), "/", base::TRIM_TRAILING)) {
          *error = "network: \"" + (item.is_string() ? item.GetString() : std::string("?")) +
                   "\" is not an origin such as https://example.com or http://127.0.0.1:8080";
          return false;
        }
        options->network.push_back(origin);
      }
    } else if (key == "instances") {
      if (!number || n < 1 || n > 64 || n != static_cast<int>(n)) {
        *error = "instances is a number of isolates, 1 to 64";
        return false;
      }
      options->instances = static_cast<int>(n);
    } else if (key == "codeCache") {
      if (!value.is_bool()) {
        *error = "codeCache is true or false";
        return false;
      }
      options->code_cache = value.GetBool();
    } else if (key == "host") {
      if (!value.is_list()) {
        *error = "host is a list of the names of the application's functions";
        return false;
      }
      for (const base::Value& item : value.GetList()) {
        if (!item.is_string() || item.GetString().empty() || item.GetString().size() > 256) {
          *error = "host: a name is a string of 1 to 256 characters";
          return false;
        }
        if (std::find(options->host.begin(), options->host.end(), item.GetString()) == options->host.end())
          options->host.push_back(item.GetString());
      }
    } else if (key == "width" || key == "height") {
      // The backend "page"'s (library_page.cc); checked against it below.
      if (!number || n < 1 || n > 16384 || n != static_cast<int>(n)) {
        *error = std::string(key) + " is a number of CSS pixels, 1 to 16384";
        return false;
      }
      (key == "width" ? options->width : options->height) = static_cast<int>(n);
    } else {
      *error = "unknown option \"" + std::string(key) + "\"";
      return false;
    }
  }
  if (!options->page && (options->width || options->height)) {
    *error = "width and height are the backend \"page\"'s";
    return false;
  }
  // Not built for pages yet: refused rather than ignored (docs/JS_LIBRARIES.md).
  if (options->page && options->instances != 1) {
    *error = "instances is the backend \"isolate\"'s";
    return false;
  }
  if (options->page && !options->host.empty()) {
    *error = "host is the backend \"isolate\"'s";
    return false;
  }
  return true;
}

void Deliver(scoped_refptr<zebkit_library> library, zebkit_library_func func, void* data,
             zebkit_library_code code, std::string json, std::vector<std::string> parts) {
  std::vector<zebkit_part> list;
  list.reserve(parts.size());
  for (const std::string& part : parts)
    list.push_back({reinterpret_cast<const uint8_t*>(part.data()), part.size()});
  func(library.get(), code, json.c_str(), list.empty() ? nullptr : list.data(), list.size(), data);
}

// V8's code cache of libraries' modules (docs/JS_LIBRARIES.md, "Load"): the
// bytecode V8 made of a module, kept by the module's content (SHA-256) and
// V8's tag (its version and flags), so that a load of the same module
// compiles nothing V8 compiled before. In memory for the process, at most
// kMemoryBytes, the oldest dropped first; and in the profile, a file per
// module path in "Library Code Cache", which holds the content's key: a
// changed module replaces its file, so the folder never grows past the
// modules loaded. Never in the library's folder.
class CodeCaches {
 public:
  static constexpr size_t kMemoryBytes = size_t{64} << 20;
  static constexpr size_t kMaxFile = size_t{64} << 20;

  static CodeCaches& Get() {
    static base::NoDestructor<CodeCaches> caches;
    return *caches;
  }

  // The key of @text's cache.
  static std::string Key(std::string_view text) {
    return base::HexEncode(crypto::hash::Sha256(text)) + "-" +
           base::NumberToString(v8::ScriptCompiler::CachedDataVersionTag());
  }

  // The cache of @key: in memory, else in the profile's file for @path.
  std::shared_ptr<const std::string> Find(const std::string& key, const base::FilePath& path) {
    {
      base::AutoLock lock(lock_);
      auto it = memory_.find(key);
      if (it != memory_.end())
        return it->second;
    }
    const base::FilePath file = File(path);
    std::string text;
    if (file.empty() || !base::ReadFileToStringWithMaxSize(file, &text, kMaxFile + 4096))
      return nullptr;
    const std::string head = Head(key);
    if (!text.starts_with(head))
      return nullptr;
    auto data = std::make_shared<const std::string>(text.substr(head.size()));
    Remember(key, data);
    return data;
  }

  bool Has(const std::string& key) {
    base::AutoLock lock(lock_);
    return memory_.contains(key);
  }

  void Put(const std::string& key, const base::FilePath& path, std::string data) {
    if (data.empty() || data.size() > kMaxFile)
      return;
    const base::FilePath file = File(path);
    if (!file.empty() && base::CreateDirectory(file.DirName()))
      base::ImportantFileWriter::WriteFileAtomically(file, Head(key) + data);
    Remember(key, std::make_shared<const std::string>(std::move(data)));
  }

  // V8 refused it: gone from memory and the profile.
  void Drop(const std::string& key, const base::FilePath& path) {
    {
      base::AutoLock lock(lock_);
      auto it = memory_.find(key);
      if (it != memory_.end()) {
        bytes_ -= it->second->size();
        memory_.erase(it);
        std::erase(order_, key);
      }
    }
    const base::FilePath file = File(path);
    if (!file.empty())
      base::DeleteFile(file);
  }

 private:
  static std::string Head(const std::string& key) { return "zebkit library code cache 1 " + key + "\n"; }

  // The profile's file for the module at @path; empty when the engine has no
  // profile folder.
  static base::FilePath File(const base::FilePath& path) {
    base::FilePath profile;
    if (!base::PathService::Get(kUserDataPath, &profile) || profile.empty())
      return base::FilePath();
    const std::string name = base::HexEncode(crypto::hash::Sha256(PathToUTF8(path))).substr(0, 32);
    return profile.AppendASCII("Library Code Cache").AppendASCII(name + ".v8");
  }

  void Remember(const std::string& key, std::shared_ptr<const std::string> data) {
    base::AutoLock lock(lock_);
    if (memory_.contains(key) || data->size() > kMemoryBytes)
      return;
    bytes_ += data->size();
    memory_[key] = std::move(data);
    order_.push_back(key);
    while (bytes_ > kMemoryBytes && !order_.empty()) {
      auto it = memory_.find(order_.front());
      bytes_ -= it->second->size();
      memory_.erase(it);
      order_.pop_front();
    }
  }

  base::Lock lock_;
  std::map<std::string, std::shared_ptr<const std::string>> memory_ GUARDED_BY(lock_);
  std::deque<std::string> order_ GUARDED_BY(lock_);
  size_t bytes_ GUARDED_BY(lock_) = 0;
};

// How long an instance is idle (no call started since its last ended)
// before it makes its code cache and V8 gives memory back
// (docs/JS_LIBRARIES.md, "Memory": what it gains and costs). After each call
// it costs too much: a full collection is most of a millisecond.
constexpr base::TimeDelta kIdle = base::Seconds(2);

}  // namespace

// The backend "page" checks its files as imports are checked here.
base::FilePath LibraryRealPath(const base::FilePath& path) {
  return RealPath(path);
}

std::string LibraryErrorJson(const std::string& message, const std::string& name, const std::string& stack) {
  base::DictValue dict;
  dict.Set("message", message);
  dict.Set("name", name);
  dict.Set("stack", stack);
  return base::WriteJson(dict).value_or("{}");
}

// ---- LibraryIsolate: an instance's thread ----

LibraryIsolate::Scope::Scope(LibraryIsolate* host)
    : host(host), isolate_scope(host->isolate()), handles(host->isolate()), context_scope(host->context()) {
  if (host->depth_++ == 0)
    host->library_->Entered(host->index_);
}

LibraryIsolate::Scope::~Scope() {
  if (--host->depth_ != 0)
    return;
  v8::Isolate* isolate = host->isolate();
  if (!isolate->IsExecutionTerminating()) {
    ++host->depth_;
    isolate->PerformMicrotaskCheckpoint();
    --host->depth_;
  }
  host->ReportRejections();
  const zebkit_library_code ending = host->library_->Left(host->index_);
  if (ending != ZEBKIT_LIBRARY_OK && !host->ended_) {
    host->ended_ = true;
    host->library_->End(host->index_, ending);
  }
}

LibraryIsolate::LibraryIsolate(scoped_refptr<zebkit_library> library, int index)
    : library_(std::move(library)), index_(index) {}

LibraryIsolate::~LibraryIsolate() {
  if (!isolate_)
    return;
  {
    base::AutoLock lock(library_->lock_);
    library_->instances[index_]->isolate = nullptr;
  }
  {
    v8::Isolate::Scope isolate_scope(isolate_);
    v8::HandleScope handles(isolate_);
    isolate_->RemoveNearHeapLimitCallback(&NearHeapLimit, 0);
    timers_.clear();
    fetches_.clear();
    requests_.clear();
    modules_.clear();
    host_module_.Reset();
    unhandled_.clear();
    run_.Reset();
    runner_.Reset();
    context_.Reset();
  }
  static_cast<EnteringTaskRunner*>(tasks_.get())->set_isolate(nullptr);
  isolate_ = nullptr;
  holder_.reset();
}

v8::Local<v8::Context> LibraryIsolate::context() {
  return context_.Get(isolate_);
}

LibraryIsolate* LibraryIsolate::From(const v8::FunctionCallbackInfo<v8::Value>& info) {
  return static_cast<LibraryIsolate*>(info.Data().As<v8::External>()->Value(v8::kExternalPointerTypeTagDefault));
}

void LibraryIsolate::SetMethod(v8::Local<v8::Object> object, const char* name, v8::FunctionCallback callback) {
  v8::Local<v8::External> self = v8::External::New(isolate_, this, v8::kExternalPointerTypeTagDefault);
  std::ignore = object->Set(context(), String8(isolate_, name),
                            v8::Function::New(context(), callback, self).ToLocalChecked());
}

std::string LibraryIsolate::Describe(v8::Local<v8::Value> exception) {
  v8::Local<v8::Context> context = this->context();
  if (exception.IsEmpty())
    return LibraryErrorJson("exception", "Error");
  if (exception->IsNativeError() || exception->IsObject()) {
    v8::Local<v8::Object> object = exception.As<v8::Object>();
    auto field = [&](const char* name) {
      v8::Local<v8::Value> value;
      v8::TryCatch try_catch(isolate_);
      if (object->Get(context, String8(isolate_, name)).ToLocal(&value) && !value->IsUndefined())
        return Utf8(isolate_, value);
      return std::string();
    };
    if (exception->IsNativeError())
      return LibraryErrorJson(field("message"), field("name"), field("stack"));
  }
  return LibraryErrorJson(Utf8(isolate_, exception), "", "");
}

void LibraryIsolate::Uncaught(const std::string& text) {
  fprintf(stderr, "zebkit library %s: %s\n", PathToUTF8(library_->entry).c_str(), text.c_str());
  fflush(stderr);
}

void LibraryIsolate::ReportRejections() {
  if (unhandled_.empty())
    return;
  auto unhandled = std::move(unhandled_);
  unhandled_.clear();
  v8::HandleScope handles(isolate_);
  for (const auto& [promise, value] : unhandled) {
    std::optional<base::Value> parsed = base::JSONReader::Read(Describe(value.Get(isolate_)), base::JSON_PARSE_RFC);
    const std::string* stack = parsed && parsed->is_dict() ? parsed->GetDict().FindString("stack") : nullptr;
    const std::string* message = parsed && parsed->is_dict() ? parsed->GetDict().FindString("message") : nullptr;
    Uncaught("Uncaught (in promise) " + (stack && !stack->empty() ? *stack : message ? *message : std::string()));
  }
}

bool LibraryIsolate::Create(std::string* error) {
  start_ = base::TimeTicks::Now();
  std::unique_ptr<v8::Isolate::CreateParams> params = gin::IsolateHolder::getDefaultIsolateParams();
  params->constraints.ConfigureDefaultsFromHeapSize(0, library_->options.heap_limit);
  allocator_ = std::make_unique<CountingAllocator>(library_->options.heap_limit);
  params->array_buffer_allocator = allocator_.get();
  tasks_ = base::MakeRefCounted<EnteringTaskRunner>(base::SingleThreadTaskRunner::GetCurrentDefault());
  holder_ = std::make_unique<gin::IsolateHolder>(tasks_, gin::IsolateHolder::kSingleThread,
                                                 gin::IsolateHolder::IsolateType::kUtility, std::move(params));
  isolate_ = holder_->isolate();
  static_cast<EnteringTaskRunner*>(tasks_.get())->set_isolate(isolate_);
  {
    base::AutoLock lock(library_->lock_);
    library_->instances[index_]->isolate = isolate_.get();
  }
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  isolate_->SetMicrotasksPolicy(v8::MicrotasksPolicy::kExplicit);
  isolate_->AddNearHeapLimitCallback(&NearHeapLimit, this);
  isolate_->SetPromiseRejectCallback([](v8::PromiseRejectMessage message) {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    if (context.IsEmpty())
      return;
    auto* self = static_cast<LibraryIsolate*>(
        context->GetAlignedPointerFromEmbedderData(1, v8::kEmbedderDataTypeTagDefault));
    if (!self)
      return;
    v8::Local<v8::Promise> promise = message.GetPromise();
    if (message.GetEvent() == v8::kPromiseRejectWithNoHandler) {
      self->unhandled_.emplace_back(v8::Global<v8::Promise>(isolate, promise),
                                    v8::Global<v8::Value>(isolate, message.GetValue()));
    } else if (message.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
      std::erase_if(self->unhandled_, [&](const auto& entry) { return entry.first == promise; });
    }
  });
  v8::Local<v8::Context> context = v8::Context::New(isolate_);
  context->SetAlignedPointerInEmbedderData(1, this, v8::kEmbedderDataTypeTagDefault);
  context_.Reset(isolate_, context);

  Scope scope(this);
  v8::Local<v8::Object> native = v8::Object::New(isolate_);
  SetMethod(native, "print", &Print);
  SetMethod(native, "setTimer", &SetTimer);
  SetMethod(native, "clearTimer", &ClearTimer);
  SetMethod(native, "settle", &SettleCall);
  SetMethod(native, "loaded", &LoadedCall);
  SetMethod(native, "fetch", &Fetch);
  SetMethod(native, "hostCall", &HostCall);
  InstallWeb(native);
  v8::Local<v8::Object> codes = v8::Object::New(isolate_);
  for (int code = ZEBKIT_LIBRARY_OK; code <= ZEBKIT_LIBRARY_GONE; ++code) {
    std::ignore = codes->Set(context, String8(isolate_, CodeName(static_cast<zebkit_library_code>(code))),
                             v8::Integer::New(isolate_, code));
  }
  std::ignore = native->Set(context, String8(isolate_, "codes"), codes);
  const std::vector<std::string>& names = library_->options.host;
  v8::Local<v8::Array> host = v8::Array::New(isolate_, static_cast<int>(names.size()));
  for (size_t i = 0; i < names.size(); ++i)
    std::ignore = host->Set(context, static_cast<uint32_t>(i), String8(isolate_, names[i]));
  std::ignore = native->Set(context, String8(isolate_, "hostFunctions"), host);

  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Script> script;
  v8::Local<v8::Value> bootstrap, runner;
  v8::Local<v8::Value> argv[] = {native};
  if (!v8::Script::Compile(context, String8(isolate_, kLibraryBootstrap)).ToLocal(&script) ||
      !script->Run(context).ToLocal(&bootstrap) || !bootstrap->IsFunction() ||
      !bootstrap.As<v8::Function>()->Call(context, v8::Undefined(isolate_), 1, argv).ToLocal(&runner) ||
      !runner->IsObject()) {
    *error = try_catch.HasCaught() ? Describe(try_catch.Exception()) : LibraryErrorJson("no runner", "Error");
    return false;
  }
  runner_.Reset(isolate_, runner.As<v8::Object>());
  v8::Local<v8::Value> run;
  if (!runner.As<v8::Object>()->Get(context, String8(isolate_, "run")).ToLocal(&run) || !run->IsFunction()) {
    *error = LibraryErrorJson("no runner", "Error");
    return false;
  }
  run_.Reset(isolate_, run.As<v8::Function>());
  return true;
}

void LibraryIsolate::Load() {
  std::string error;
  if (!Create(&error)) {
    library_->Loaded(index_, ZEBKIT_LIBRARY_LOAD_FAILED, error);
    return;
  }
  if (ended_)
    return;
  Scope scope(this);
  v8::Local<v8::Context> context = this->context();
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Module> module;
  v8::Local<v8::Value> evaluated;
  if (!LoadModule(library_->entry).ToLocal(&module) || !module->InstantiateModule(context, &Resolve).FromMaybe(false) ||
      !module->Evaluate(context).ToLocal(&evaluated)) {
    if (!isolate_->IsExecutionTerminating())
      library_->Loaded(index_, ZEBKIT_LIBRARY_LOAD_FAILED, Describe(try_catch.Exception()));
    return;
  }
  v8::Local<v8::Value> loaded;
  v8::Local<v8::Value> argv[] = {evaluated, module->GetModuleNamespace()};
  if (!runner_.Get(isolate_)->Get(context, String8(isolate_, "loaded")).ToLocal(&loaded) || !loaded->IsFunction() ||
      loaded.As<v8::Function>()->Call(context, v8::Undefined(isolate_), 2, argv).IsEmpty()) {
    if (!isolate_->IsExecutionTerminating())
      library_->Loaded(index_, ZEBKIT_LIBRARY_LOAD_FAILED, Describe(try_catch.Exception()));
  }
}

v8::MaybeLocal<v8::Module> LibraryIsolate::LoadModule(const base::FilePath& path) {
  const std::string key = PathToUTF8(path);
  auto it = modules_.find(key);
  if (it != modules_.end())
    return it->second.module.Get(isolate_);
  if (modules_.size() >= kMaxModules) {
    isolate_->ThrowException(v8::Exception::Error(String8(isolate_, "more than 4096 modules")));
    return {};
  }
  std::string text;
  if (!base::ReadFileToString(path, &text)) {
    isolate_->ThrowException(v8::Exception::Error(String8(isolate_, "cannot read the module " + key)));
    return {};
  }
  v8::ScriptOrigin origin(String8(isolate_, "file://" + key), 0, 0, false, -1, v8::Local<v8::Value>(), false, false,
                          /*is_module=*/true);
  // With the code cache V8 made of the same text before, when there is one.
  std::string cache_key;
  std::shared_ptr<const std::string> cached;
  if (library_->options.code_cache) {
    cache_key = CodeCaches::Key(text);
    cached = CodeCaches::Get().Find(cache_key, path);
  }
  v8::ScriptCompiler::Source source(
      String8(isolate_, text), origin,
      cached ? new v8::ScriptCompiler::CachedData(reinterpret_cast<const uint8_t*>(cached->data()),
                                                  static_cast<int>(cached->size()))
             : nullptr);
  v8::Local<v8::Module> module;
  if (!v8::ScriptCompiler::CompileModule(isolate_, &source,
                                         cached ? v8::ScriptCompiler::kConsumeCodeCache
                                                : v8::ScriptCompiler::kNoCompileOptions)
           .ToLocal(&module)) {
    return {};
  }
  const bool taken = cached && !source.GetCachedData()->rejected;
  if (cached && !taken)
    CodeCaches::Get().Drop(cache_key, path);
  Module& entry = modules_[key];
  entry.module.Reset(isolate_, module);
  entry.cache_key = cache_key;
  entry.cached = taken;
  module_paths_.emplace(module->GetIdentityHash(), key);
  return module;
}

void LibraryIsolate::MakeCodeCache() {
  if (cache_made_ || ended_ || !isolate_)
    return;
  cache_made_ = true;
  v8::Isolate::Scope isolate_scope(isolate_);
  v8::HandleScope handles(isolate_);
  for (auto& [path, module] : modules_) {
    if (module.cache_key.empty() || module.cached || CodeCaches::Get().Has(module.cache_key))
      continue;
    std::unique_ptr<v8::ScriptCompiler::CachedData> data(
        v8::ScriptCompiler::CreateCodeCache(module.module.Get(isolate_)->GetUnboundModuleScript()));
    if (data && data->length > 0) {
      CodeCaches::Get().Put(module.cache_key, PathFromUTF8(path),
                            std::string(reinterpret_cast<const char*>(data->data), data->length));
    }
  }
}

void LibraryIsolate::Shrink() {
  if (ended_ || !isolate_)
    return;
  v8::Isolate::Scope isolate_scope(isolate_);
  // Full collections, compacting, until nothing more is freed; the free pages
  // go back. MemoryPressureNotification() gave back a quarter as much.
  isolate_->LowMemoryNotification();
}

// "zebkit:host": host (its default export), host.call() and host.functions.
v8::Local<v8::Module> LibraryIsolate::HostModule() {
  if (host_module_.IsEmpty()) {
    v8::Local<v8::String> names[] = {String8(isolate_, "default"), String8(isolate_, "call"),
                                     String8(isolate_, "functions")};
    host_module_.Reset(isolate_, v8::Module::CreateSyntheticModule(isolate_, String8(isolate_, "zebkit:host"),
                                                                   std::span<const v8::Local<v8::String>>(names),
                                                                   &EvaluateHost));
  }
  return host_module_.Get(isolate_);
}

v8::MaybeLocal<v8::Promise> LibraryIsolate::EvaluateHost(v8::Local<v8::Context> context,
                                                         v8::Local<v8::Module> module) {
  auto* self = static_cast<LibraryIsolate*>(
      context->GetAlignedPointerFromEmbedderData(1, v8::kEmbedderDataTypeTagDefault));
  v8::Isolate* isolate = self->isolate_;
  v8::Local<v8::Value> host;
  if (!self->runner_.Get(isolate)->Get(context, String8(isolate, "host")).ToLocal(&host) || !host->IsObject())
    return {};
  v8::Local<v8::Value> call, functions;
  if (!host.As<v8::Object>()->Get(context, String8(isolate, "call")).ToLocal(&call) ||
      !host.As<v8::Object>()->Get(context, String8(isolate, "functions")).ToLocal(&functions) ||
      module->SetSyntheticModuleExport(isolate, String8(isolate, "default"), host).IsNothing() ||
      module->SetSyntheticModuleExport(isolate, String8(isolate, "call"), call).IsNothing() ||
      module->SetSyntheticModuleExport(isolate, String8(isolate, "functions"), functions).IsNothing()) {
    return {};
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
      resolver->Resolve(context, v8::Undefined(isolate)).IsNothing()) {
    return {};
  }
  return resolver->GetPromise();
}

v8::MaybeLocal<v8::Module> LibraryIsolate::Resolve(v8::Local<v8::Context> context, v8::Local<v8::String> specifier,
                                                   v8::Local<v8::FixedArray> attributes,
                                                   v8::Local<v8::Module> referrer) {
  auto* self = static_cast<LibraryIsolate*>(
      context->GetAlignedPointerFromEmbedderData(1, v8::kEmbedderDataTypeTagDefault));
  v8::Isolate* isolate = self->isolate_;
  const std::string name = Utf8(isolate, specifier);
  auto refuse = [&](const std::string& why) {
    isolate->ThrowException(v8::Exception::Error(String8(isolate, "cannot import \"" + name + "\": " + why)));
    return v8::MaybeLocal<v8::Module>();
  };
  // The application's functions, for every library.
  if (name == "zebkit:host")
    return self->HostModule();
  if (self->library_->single_file)
    return refuse("a library of one file imports nothing; make it a folder with index.mjs");
  if (!(name.starts_with("./") || name.starts_with("../")))
    return refuse("a library imports only files of its folder, by a path starting with ./ or ../");
  const std::string* from = nullptr;
  auto [first, last] = self->module_paths_.equal_range(referrer->GetIdentityHash());
  for (auto it = first; it != last && !from; ++it) {
    auto module = self->modules_.find(it->second);
    if (module != self->modules_.end() && module->second.module == referrer)
      from = &it->second;
  }
  if (!from)
    return refuse("its importer is unknown");
  const base::FilePath path = PathFromUTF8(*from).DirName().Append(PathFromUTF8(name));
  // Links resolved: the file itself must be inside the folder.
  const base::FilePath absolute = RealPath(path);
  if (absolute.empty())
    return refuse("no such file");
  if (!self->library_->root.IsParent(absolute))
    return refuse("it is outside the library's folder");
  return self->LoadModule(absolute);
}

size_t LibraryIsolate::NearHeapLimit(void* data, size_t current, size_t initial) {
  auto* self = static_cast<LibraryIsolate*>(data);
  self->library_->OutOfMemory(self->index_);
  // Room for the script to unwind once stopped; the instance ends after.
  return current + std::max(current / 2, size_t{32} << 20);
}

void LibraryIsolate::Run(uint64_t id, const std::string& function, const std::string& args,
                         const std::vector<std::string>& parts) {
  if (ended_)
    return;
  Scope scope(this);
  v8::Local<v8::Context> context = this->context();
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Array> list = v8::Array::New(isolate_, static_cast<int>(parts.size()));
  for (size_t i = 0; i < parts.size(); ++i) {
    v8::Local<v8::ArrayBuffer> buffer;
    if (!v8::ArrayBuffer::MaybeNew(isolate_, parts[i].size()).ToLocal(&buffer)) {
      library_->Settled(index_, id, ZEBKIT_LIBRARY_BAD_ARGUMENTS,
                        LibraryErrorJson("no memory for the parts within the library's heap limit", "RangeError"),
                        {});
      return;
    }
    if (!parts[i].empty())
      memcpy(buffer->Data(), parts[i].data(), parts[i].size());
    std::ignore = list->Set(context, static_cast<uint32_t>(i), v8::Uint8Array::New(buffer, 0, parts[i].size()));
  }
  v8::Local<v8::String> json;
  if (!v8::String::NewFromUtf8(isolate_, args.data(), v8::NewStringType::kNormal, static_cast<int>(args.size()))
           .ToLocal(&json)) {
    library_->Settled(index_, id, ZEBKIT_LIBRARY_BAD_ARGUMENTS,
                      LibraryErrorJson("the arguments are too long", "RangeError"), {});
    return;
  }
  v8::Local<v8::Value> argv[] = {v8::Number::New(isolate_, static_cast<double>(id)), String8(isolate_, function), json,
                                 list};
  if (run_.Get(isolate_)->Call(context, v8::Undefined(isolate_), 4, argv).IsEmpty() &&
      !isolate_->IsExecutionTerminating()) {
    library_->Settled(index_, id, ZEBKIT_LIBRARY_THROWN, Describe(try_catch.Exception()), {});
  }
}

void LibraryIsolate::Answered(int local, zebkit_library_code code, const std::string& json,
                              const std::vector<std::string>& parts) {
  auto it = requests_.find(local);
  if (it == requests_.end() || ended_)
    return;
  Scope scope(this);
  v8::Local<v8::Context> context = this->context();
  v8::Local<v8::Function> done = it->second.Get(isolate_);
  requests_.erase(it);
  v8::TryCatch try_catch(isolate_);
  v8::Local<v8::Array> list = v8::Array::New(isolate_, static_cast<int>(parts.size()));
  for (size_t i = 0; i < parts.size(); ++i) {
    v8::Local<v8::ArrayBuffer> buffer;
    if (!v8::ArrayBuffer::MaybeNew(isolate_, parts[i].size()).ToLocal(&buffer)) {
      code = ZEBKIT_LIBRARY_BAD_ARGUMENTS;
      list = v8::Array::New(isolate_, 0);
      break;
    }
    if (!parts[i].empty())
      memcpy(buffer->Data(), parts[i].data(), parts[i].size());
    std::ignore = list->Set(context, static_cast<uint32_t>(i), v8::Uint8Array::New(buffer, 0, parts[i].size()));
  }
  v8::Local<v8::String> text;
  if (code == ZEBKIT_LIBRARY_BAD_ARGUMENTS ||
      !v8::String::NewFromUtf8(isolate_, json.data(), v8::NewStringType::kNormal, static_cast<int>(json.size()))
           .ToLocal(&text)) {
    code = ZEBKIT_LIBRARY_BAD_ARGUMENTS;
    text = String8(isolate_, LibraryErrorJson("the answer does not fit the library's heap limit", "RangeError"));
  }
  v8::Local<v8::Value> argv[] = {v8::Integer::New(isolate_, code), text, list};
  if (done->Call(context, v8::Undefined(isolate_), 3, argv).IsEmpty() && !isolate_->IsExecutionTerminating())
    Uncaught("Uncaught " + Utf8(isolate_, try_catch.Exception()));
}

// ---- Native functions ----

void LibraryIsolate::Print(const v8::FunctionCallbackInfo<v8::Value>& info) {
  const std::string text = Utf8(info.GetIsolate(), info[1]);
  FILE* out = info[0]->IsInt32() && info[0].As<v8::Int32>()->Value() ? stderr : stdout;
  fprintf(out, "%s\n", text.c_str());
  fflush(out);
}

void LibraryIsolate::SetTimer(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LibraryIsolate* self = From(info);
  if (!info[0]->IsFunction())
    return;
  const int id = self->next_timer_++;
  Timer& timer = self->timers_[id];
  timer.function.Reset(info.GetIsolate(), info[0].As<v8::Function>());
  timer.repeat = info[2]->BooleanValue(info.GetIsolate());
  const double ms = info[1]->IsNumber() ? info[1].As<v8::Number>()->Value() : 0;
  timer.ms = static_cast<int>(std::clamp(ms, 0.0, 2147483647.0));
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&LibraryIsolate::FireTimer, self->weak_factory_.GetWeakPtr(), id),
      base::Milliseconds(timer.ms));
  info.GetReturnValue().Set(id);
}

void LibraryIsolate::FireTimer(int id) {
  auto it = timers_.find(id);
  if (it == timers_.end() || ended_)
    return;
  Scope scope(this);
  v8::Local<v8::Function> function = it->second.function.Get(isolate_);
  if (it->second.repeat) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, base::BindOnce(&LibraryIsolate::FireTimer, weak_factory_.GetWeakPtr(), id),
        base::Milliseconds(std::max(it->second.ms, 1)));
  } else {
    timers_.erase(it);
  }
  v8::TryCatch try_catch(isolate_);
  if (function->Call(context(), v8::Undefined(isolate_), 0, nullptr).IsEmpty() &&
      !isolate_->IsExecutionTerminating()) {
    Uncaught("Uncaught " + Utf8(isolate_, try_catch.Exception()));
  }
}

void LibraryIsolate::ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& info) {
  if (info[0]->IsNumber())
    From(info)->timers_.erase(static_cast<int>(info[0].As<v8::Number>()->Value()));
}

namespace {

// The bytes of a list of typed arrays; false past @max_bytes (with @total).
bool PartsOf(v8::Local<v8::Context> context, v8::Local<v8::Value> value, size_t max_bytes, size_t* total,
             std::vector<std::string>* parts) {
  if (!value->IsArray())
    return true;
  v8::Local<v8::Array> list = value.As<v8::Array>();
  for (uint32_t i = 0; i < list->Length(); ++i) {
    v8::Local<v8::Value> item;
    if (!list->Get(context, i).ToLocal(&item) || !item->IsArrayBufferView())
      continue;
    v8::Local<v8::ArrayBufferView> view = item.As<v8::ArrayBufferView>();
    *total += view->ByteLength();
    if (*total > max_bytes)
      return false;
    std::string bytes(view->ByteLength(), '\0');
    view->CopyContents(bytes.data(), bytes.size());
    parts->push_back(std::move(bytes));
  }
  return true;
}

}  // namespace

// settle(id, code, json, parts): a call's end, from the runner.
void LibraryIsolate::SettleCall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LibraryIsolate* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  const uint64_t id = static_cast<uint64_t>(info[0]->IsNumber() ? info[0].As<v8::Number>()->Value() : 0);
  auto code = static_cast<zebkit_library_code>(info[1]->IsInt32() ? info[1].As<v8::Int32>()->Value() : 1);
  std::string json = Utf8(isolate, info[2]);
  std::vector<std::string> parts;
  size_t total = json.size();
  if (total > self->library_->options.max_bytes ||
      !PartsOf(isolate->GetCurrentContext(), info[3], self->library_->options.max_bytes, &total, &parts)) {
    code = ZEBKIT_LIBRARY_BAD_RESULT;
    json = LibraryErrorJson("the value is larger than maxBytes (" +
                                base::NumberToString(self->library_->options.max_bytes) + " bytes)",
                            "RangeError");
    parts.clear();
  }
  self->library_->Settled(self->index_, id, code, std::move(json), std::move(parts));
}

void LibraryIsolate::LoadedCall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LibraryIsolate* self = From(info);
  auto code = static_cast<zebkit_library_code>(info[0]->IsInt32() ? info[0].As<v8::Int32>()->Value() : 2);
  self->library_->Loaded(self->index_, code, Utf8(info.GetIsolate(), info[1]));
}

// hostCall(name, json, parts, done): a request to the application; null, or
// why it was refused at once. done(code, json, parts) is its answer.
void LibraryIsolate::HostCall(const v8::FunctionCallbackInfo<v8::Value>& info) {
  LibraryIsolate* self = From(info);
  v8::Isolate* isolate = info.GetIsolate();
  if (!info[3]->IsFunction())
    return;
  std::string json = Utf8(isolate, info[1]);
  std::vector<std::string> parts;
  size_t total = json.size();
  if (total > self->library_->options.max_bytes ||
      !PartsOf(isolate->GetCurrentContext(), info[2], self->library_->options.max_bytes, &total, &parts)) {
    info.GetReturnValue().Set(String8(isolate, "the arguments are larger than maxBytes (" +
                                                   base::NumberToString(self->library_->options.max_bytes) +
                                                   " bytes)"));
    return;
  }
  const int local = self->next_request_++;
  std::string why;
  if (!self->library_->Request(self->index_, local, Utf8(isolate, info[0]), std::move(json), std::move(parts),
                               &why)) {
    info.GetReturnValue().Set(String8(isolate, why));
    return;
  }
  self->requests_[local].Reset(isolate, info[3].As<v8::Function>());
  info.GetReturnValue().SetNull();
}

}  // namespace zebkit

// ---- zebkit_library: shared ----

using zebkit::LibraryCall;
using zebkit::LibraryPosts;

zebkit_library::zebkit_library() = default;
zebkit_library::~zebkit_library() = default;

void zebkit_library::Post(LibraryPosts posts) {
  for (base::OnceClosure& post : posts)
    zebkit::PostEngineTask(std::move(post), base::TimeDelta());
}

void zebkit_library::TellLocked(LibraryCall* call, zebkit_library_code code, std::string json,
                                std::vector<std::string> parts, LibraryPosts* posts) {
  if (call->told)
    return;
  call->told = true;
  if (call->func) {
    posts->push_back(base::BindOnce(&zebkit::Deliver, base::WrapRefCounted(this), call->func, call->data.get(), code,
                                    std::move(json), std::move(parts)));
  }
}

void zebkit_library::TellReadyLocked(zebkit_library_code code, std::string json, LibraryPosts* posts) {
  if (ready_told_)
    return;
  ready_told_ = true;
  if (ready_func) {
    posts->push_back(base::BindOnce(&zebkit::Deliver, base::WrapRefCounted(this), ready_func, ready_data.get(), code,
                                    std::move(json), std::vector<std::string>()));
  }
}

void zebkit_library::FailAllLocked(zebkit_library_code code, const std::string& json, LibraryPosts* posts) {
  for (auto& [id, call] : calls_)
    TellLocked(call.get(), code, json, {}, posts);
  calls_.clear();
  queue_.clear();
  requests_.clear();
  for (auto& instance : instances)
    instance->current = 0;
}

void zebkit_library::FailLoadLocked(const std::string& json, LibraryPosts* posts) {
  state_ = State::kFailed;
  TellReadyLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, posts);
  FailAllLocked(ZEBKIT_LIBRARY_LOAD_FAILED, json, posts);
  // Instances still running script (a top level that never ends) stopped.
  for (auto& instance : instances) {
    if (instance->isolate && instance->depth > 0) {
      if (instance->ending == ZEBKIT_LIBRARY_OK)
        instance->ending = ZEBKIT_LIBRARY_GONE;
      instance->isolate->TerminateExecution();
    }
  }
}

void zebkit_library::DestroyAll() {
  for (auto& instance : instances) {
    instance->runner->PostTask(FROM_HERE,
                               base::BindOnce(&zebkit_library::DestroyRuntime, base::WrapRefCounted(this),
                                              instance->index));
  }
}

void zebkit_library::ScheduleLocked() {
  if (state_ != State::kReady)
    return;
  for (auto& instance : instances) {
    if (queue_.empty())
      return;
    if (!instance->loaded || instance->ended || instance->current)
      continue;
    const uint64_t id = queue_.front();
    queue_.pop_front();
    instance->current = id;
    ++instance->runs;
    instance->runner->PostTask(
        FROM_HERE, base::BindOnce(&zebkit_library::StartCall, base::WrapRefCounted(this), instance->index, id));
  }
}

uint64_t zebkit_library::Call(const char* function, const char* args_json, const zebkit_part* parts,
                              size_t part_count, int64_t timeout_ms, zebkit_library_func func, void* data) {
  if (!function || !*function || (part_count && !parts))
    return 0;
  auto call = std::make_unique<LibraryCall>();
  call->function = function;
  call->args = args_json ? args_json : "[]";
  size_t total = call->args.size();
  for (size_t i = 0; i < part_count; ++i)
    total += parts[i].size;
  const bool too_large = total > options.max_bytes;
  if (!too_large) {
    call->parts.reserve(part_count);
    for (size_t i = 0; i < part_count; ++i) {
      call->parts.emplace_back(reinterpret_cast<const char*>(parts[i].data),
                               parts[i].data ? parts[i].size : 0);
    }
  }
  call->timeout_ms = timeout_ms == 0 ? options.timeout_ms : timeout_ms;
  call->func = func;
  call->data = data;
  LibraryPosts posts;
  uint64_t id;
  {
    base::AutoLock lock(lock_);
    if (state_ == State::kClosed)
      return 0;
    id = call->id = next_id_++;
    LibraryCall* raw = call.get();
    if (state_ == State::kEnded) {
      TellLocked(raw, ZEBKIT_LIBRARY_GONE, zebkit::CodeJson(ZEBKIT_LIBRARY_GONE, "the library has ended"), {},
                 &posts);
    } else if (state_ == State::kFailed) {
      TellLocked(raw, ZEBKIT_LIBRARY_LOAD_FAILED,
                 zebkit::CodeJson(ZEBKIT_LIBRARY_LOAD_FAILED, "the library did not load"), {}, &posts);
    } else if (too_large) {
      TellLocked(raw, ZEBKIT_LIBRARY_BAD_ARGUMENTS,
                 zebkit::LibraryErrorJson("the arguments are larger than maxBytes (" +
                                              base::NumberToString(options.max_bytes) + " bytes)",
                                          "RangeError"),
                 {}, &posts);
    } else if (queue_.size() >= options.queue) {
      TellLocked(raw, ZEBKIT_LIBRARY_QUEUE_FULL,
                 zebkit::CodeJson(ZEBKIT_LIBRARY_QUEUE_FULL,
                                  base::NumberToString(options.queue) + " calls wait already"),
                 {}, &posts);
    } else {
      calls_[id] = std::move(call);
      queue_.push_back(id);
      ScheduleLocked();
    }
  }
  Post(std::move(posts));
  return id;
}

void zebkit_library::Cancel(uint64_t id) {
  LibraryPosts posts;
  {
    base::AutoLock lock(lock_);
    auto it = calls_.find(id);
    if (it == calls_.end() || it->second->told)
      return;
    TellLocked(it->second.get(), ZEBKIT_LIBRARY_CANCELLED,
               zebkit::CodeJson(ZEBKIT_LIBRARY_CANCELLED, "the call was cancelled"), {}, &posts);
    // One an instance took runs on (or is skipped as it starts); one waiting goes.
    const bool taken = std::any_of(instances.begin(), instances.end(),
                                   [&](const auto& instance) { return instance->current == id; });
    if (!taken) {
      std::erase(queue_, id);
      calls_.erase(it);
    }
  }
  Post(std::move(posts));
}

void zebkit_library::Close() {
  LibraryPosts posts;
  {
    base::AutoLock lock(lock_);
    if (state_ == State::kClosed)
      return;
    state_ = State::kClosed;
    TellReadyLocked(ZEBKIT_LIBRARY_GONE, zebkit::CodeJson(ZEBKIT_LIBRARY_GONE, "the library was closed"), &posts);
    FailAllLocked(ZEBKIT_LIBRARY_GONE, zebkit::CodeJson(ZEBKIT_LIBRARY_GONE, "the library was closed"), &posts);
    for (auto& instance : instances) {
      if (instance->isolate && instance->depth > 0) {
        instance->ending = ZEBKIT_LIBRARY_GONE;
        instance->isolate->TerminateExecution();
      }
    }
  }
  Post(std::move(posts));
  DestroyAll();
  // Joined off the engine's thread, once the isolates are gone.
  for (auto& instance : instances) {
    if (!instance->thread)
      continue;
    instance->thread->DetachFromSequence();
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives(), base::TaskShutdownBehavior::CONTINUE_ON_SHUTDOWN},
        base::BindOnce([](std::unique_ptr<base::Thread> thread) { thread->Stop(); }, std::move(instance->thread)));
  }
  // The application's reference: the last callback frees it.
  Release();
}

void zebkit_library::Deadline(uint64_t id) {
  LibraryPosts posts;
  bool destroy = false;
  {
    base::AutoLock lock(lock_);
    if (id == 0) {
      if (state_ != State::kLoading)
        return;
      bool running = false;
      for (auto& instance : instances) {
        if (!instance->loaded && instance->isolate && instance->depth > 0) {
          instance->ending = ZEBKIT_LIBRARY_TIMEOUT;
          instance->isolate->TerminateExecution();
          running = true;
        }
      }
      // A script running is stopped, and End() fails the load; otherwise it
      // waits (a top-level await): it fails now, and the isolates go.
      if (running)
        return;
      FailLoadLocked(zebkit::CodeJson(ZEBKIT_LIBRARY_LOAD_FAILED, "the load passed its deadline (timeoutMs)"),
                     &posts);
      destroy = true;
    } else {
      if (state_ != State::kReady)
        return;
      auto instance = std::find_if(instances.begin(), instances.end(),
                                   [&](const auto& instance) { return instance->current == id; });
      if (instance == instances.end())
        return;
      if ((*instance)->isolate && (*instance)->depth > 0) {
        // Running: stopped, and the instance ends (End()).
        (*instance)->ending = ZEBKIT_LIBRARY_TIMEOUT;
        (*instance)->isolate->TerminateExecution();
        return;
      }
      // Waiting: TIMEOUT, and the instance takes the next; what it does later is dropped.
      auto it = calls_.find(id);
      if (it != calls_.end()) {
        TellLocked(it->second.get(), ZEBKIT_LIBRARY_TIMEOUT,
                   zebkit::CodeJson(ZEBKIT_LIBRARY_TIMEOUT, "the call passed its deadline while it waited"), {},
                   &posts);
        calls_.erase(it);
      }
      (*instance)->current = 0;
      ScheduleLocked();
    }
  }
  Post(std::move(posts));
  if (destroy)
    DestroyAll();
}

void zebkit_library::SetHost(zebkit_library_host_func func, void* data) {
  std::vector<uint64_t> waiting;
  {
    base::AutoLock lock(lock_);
    host_func_ = func;
    host_data_ = data;
    if (!func)
      return;
    for (const auto& [id, request] : requests_) {
      if (!request.delivered)
        waiting.push_back(id);
    }
  }
  for (uint64_t id : waiting) {
    zebkit::PostEngineTask(base::BindOnce(&zebkit_library::DeliverRequest, base::WrapRefCounted(this), id),
                           base::TimeDelta());
  }
}

void zebkit_library::DeliverRequest(uint64_t request) {
  zebkit_library_host_func func;
  void* data;
  std::string name, args;
  std::vector<std::string> parts;
  {
    base::AutoLock lock(lock_);
    auto it = requests_.find(request);
    if (it == requests_.end() || it->second.delivered || !host_func_)
      return;
    it->second.delivered = true;
    func = host_func_;
    data = host_data_.get();
    name = it->second.name;
    args = std::move(it->second.args);
    parts = std::move(it->second.parts);
  }
  std::vector<zebkit_part> list;
  list.reserve(parts.size());
  for (const std::string& part : parts)
    list.push_back({reinterpret_cast<const uint8_t*>(part.data()), part.size()});
  func(this, request, name.c_str(), args.c_str(), list.empty() ? nullptr : list.data(), list.size(), data);
}

void zebkit_library::RequestDeadline(uint64_t request) {
  base::AutoLock lock(lock_);
  auto it = requests_.find(request);
  if (it == requests_.end())
    return;
  const int i = it->second.instance, local = it->second.local;
  const std::string json = zebkit::CodeJson(
      ZEBKIT_LIBRARY_TIMEOUT, "the application did not answer host.call(\"" + it->second.name + "\") in timeoutMs");
  requests_.erase(it);
  instances[i]->runner->PostTask(FROM_HERE, base::BindOnce(&zebkit_library::Answered, base::WrapRefCounted(this), i,
                                                           local, ZEBKIT_LIBRARY_TIMEOUT, json,
                                                           std::vector<std::string>()));
}

int zebkit_library::Answer(uint64_t request, zebkit_library_code code, const char* json, const zebkit_part* parts,
                           size_t part_count) {
  if (part_count && !parts)
    return 0;
  std::string text = json && *json ? json : (code == ZEBKIT_LIBRARY_OK ? "null" : "{}");
  std::vector<std::string> list;
  size_t total = text.size();
  for (size_t i = 0; i < part_count; ++i)
    total += parts[i].size;
  if (code != ZEBKIT_LIBRARY_OK)
    code = ZEBKIT_LIBRARY_THROWN;
  if (total > options.max_bytes) {
    code = ZEBKIT_LIBRARY_BAD_ARGUMENTS;
    text = zebkit::LibraryErrorJson(
        "the application's answer is larger than maxBytes (" + base::NumberToString(options.max_bytes) + " bytes)",
        "RangeError");
  } else {
    list.reserve(part_count);
    for (size_t i = 0; i < part_count; ++i)
      list.emplace_back(reinterpret_cast<const char*>(parts[i].data), parts[i].data ? parts[i].size : 0);
  }
  base::AutoLock lock(lock_);
  auto it = requests_.find(request);
  if (it == requests_.end())
    return 0;
  const int i = it->second.instance, local = it->second.local;
  requests_.erase(it);
  instances[i]->runner->PostTask(FROM_HERE, base::BindOnce(&zebkit_library::Answered, base::WrapRefCounted(this), i,
                                                           local, code, std::move(text), std::move(list)));
  return 1;
}

void zebkit_library::StartLoad(int i) {
  {
    base::AutoLock lock(lock_);
    if (state_ != State::kLoading)
      return;
  }
  instances[i]->runtime = new zebkit::LibraryIsolate(base::WrapRefCounted(this), i);
  instances[i]->runtime->Load();
}

void zebkit_library::StartCall(int i, uint64_t id) {
  std::string function, args;
  std::vector<std::string> parts;
  int64_t timeout_ms;
  Instance* instance = instances[i].get();
  {
    base::AutoLock lock(lock_);
    if (instance->current != id || !instance->runtime)
      return;
    auto it = calls_.find(id);
    if (it == calls_.end() || it->second->told) {
      // Cancelled before it started: it never runs.
      if (it != calls_.end())
        calls_.erase(it);
      instance->current = 0;
      ScheduleLocked();
      return;
    }
    LibraryCall* call = it->second.get();
    function = std::move(call->function);
    args = std::move(call->args);
    parts = std::move(call->parts);
    timeout_ms = call->timeout_ms;
  }
  if (timeout_ms > 0) {
    zebkit::PostEngineTask(base::BindOnce(&zebkit_library::Deadline, base::WrapRefCounted(this), id),
                           base::Milliseconds(timeout_ms));
  }
  instance->runtime->Run(id, function, args, parts);
}

void zebkit_library::DestroyRuntime(int i) {
  zebkit::LibraryIsolate* gone = instances[i]->runtime;
  instances[i]->runtime = nullptr;
  if (gone)
    gone->MakeCodeCache();
  delete gone;
}

void zebkit_library::Loaded(int i, zebkit_library_code code, std::string json) {
  LibraryPosts posts;
  bool destroy = false;
  {
    base::AutoLock lock(lock_);
    if (state_ != State::kLoading)
      return;
    if (code == ZEBKIT_LIBRARY_OK) {
      instances[i]->loaded = true;
      if (ready_json_.empty())
        ready_json_ = std::move(json);
      // Ready once every instance is.
      if (++loaded_ == instances.size()) {
        state_ = State::kReady;
        TellReadyLocked(code, std::move(ready_json_), &posts);
        ScheduleLocked();
      }
    } else {
      FailLoadLocked(json, &posts);
      destroy = true;
    }
  }
  Post(std::move(posts));
  if (destroy)
    DestroyAll();
}

void zebkit_library::Settled(int i, uint64_t id, zebkit_library_code code, std::string json,
                             std::vector<std::string> parts) {
  LibraryPosts posts;
  Instance* instance = instances[i].get();
  {
    base::AutoLock lock(lock_);
    if (instance->current != id || id == 0)
      return;  // past its deadline, or the library ended: dropped
    instance->current = 0;
    auto it = calls_.find(id);
    if (it != calls_.end()) {
      TellLocked(it->second.get(), code, std::move(json), std::move(parts), &posts);
      calls_.erase(it);
    }
    ScheduleLocked();
    // Idle a while from now, unless it starts another call.
    if (!instance->current) {
      instance->runner->PostDelayedTask(
          FROM_HERE, base::BindOnce(&zebkit_library::Idle, base::WrapRefCounted(this), i, instance->runs),
          zebkit::kIdle);
    }
  }
  Post(std::move(posts));
}

void zebkit_library::Idle(int i, uint64_t runs) {
  Instance* instance = instances[i].get();
  {
    base::AutoLock lock(lock_);
    if (instance->current || instance->runs != runs || !instance->runtime)
      return;
  }
  // The code cache first, with what the calls compiled, and then memory back.
  instance->runtime->MakeCodeCache();
  instance->runtime->Shrink();
}

void zebkit_library::Entered(int i) {
  base::AutoLock lock(lock_);
  Instance* instance = instances[i].get();
  ++instance->depth;
  // An instance whose library failed or closed while it was not in script.
  if ((state_ == State::kFailed || state_ == State::kClosed) && instance->isolate) {
    if (instance->ending == ZEBKIT_LIBRARY_OK)
      instance->ending = ZEBKIT_LIBRARY_GONE;
    instance->isolate->TerminateExecution();
  }
}

zebkit_library_code zebkit_library::Left(int i) {
  base::AutoLock lock(lock_);
  Instance* instance = instances[i].get();
  --instance->depth;
  return instance->depth == 0 ? instance->ending : ZEBKIT_LIBRARY_OK;
}

void zebkit_library::OutOfMemory(int i) {
  base::AutoLock lock(lock_);
  Instance* instance = instances[i].get();
  if (instance->ending == ZEBKIT_LIBRARY_OK)
    instance->ending = ZEBKIT_LIBRARY_OUT_OF_MEMORY;
  if (instance->isolate)
    instance->isolate->TerminateExecution();
}

void zebkit_library::End(int i, zebkit_library_code code) {
  LibraryPosts posts;
  bool destroy_all = false;
  Instance* instance = instances[i].get();
  {
    base::AutoLock lock(lock_);
    instance->ended = true;
    const std::string what = code == ZEBKIT_LIBRARY_OUT_OF_MEMORY
                                 ? "the library's heap reached heapLimitMB"
                                 : "the script ran past the call's deadline and was stopped";
    if (state_ == State::kLoading) {
      FailLoadLocked(zebkit::LibraryErrorJson(
                         code == ZEBKIT_LIBRARY_OUT_OF_MEMORY
                             ? "the library's heap reached heapLimitMB while it loaded"
                             : "the load passed its deadline (timeoutMs) and was stopped",
                         "Error"),
                     &posts);
      destroy_all = true;
    } else if (state_ == State::kReady) {
      // The library goes on with its other instances; with the last, it ends.
      const bool live = std::any_of(instances.begin(), instances.end(),
                                    [](const auto& other) { return !other->ended; });
      const std::string why = what + (live ? "; its instance has ended, the others go on" : "; the library has ended");
      auto it = calls_.find(instance->current);
      if (it != calls_.end()) {
        TellLocked(it->second.get(), code, zebkit::CodeJson(code, why), {}, &posts);
        calls_.erase(it);
      }
      instance->current = 0;
      std::erase_if(requests_, [&](const auto& entry) { return entry.second.instance == i; });
      if (live) {
        ScheduleLocked();
      } else {
        state_ = State::kEnded;
        FailAllLocked(ZEBKIT_LIBRARY_GONE, zebkit::CodeJson(ZEBKIT_LIBRARY_GONE, why), &posts);
      }
    }
  }
  Post(std::move(posts));
  if (destroy_all) {
    DestroyAll();
  } else {
    instance->runner->PostTask(FROM_HERE,
                               base::BindOnce(&zebkit_library::DestroyRuntime, base::WrapRefCounted(this), i));
  }
}

uint64_t zebkit_library::Request(int i, int local, std::string name, std::string args, std::vector<std::string> parts,
                                 std::string* why) {
  uint64_t id;
  bool deliver;
  {
    base::AutoLock lock(lock_);
    if (state_ == State::kClosed || state_ == State::kFailed || state_ == State::kEnded) {
      *why = "the library is ending";
      return 0;
    }
    if (requests_.size() >= options.queue) {
      *why = base::NumberToString(options.queue) + " requests to the application wait already (queue)";
      return 0;
    }
    id = next_request_++;
    zebkit::LibraryRequest& request = requests_[id];
    request.instance = i;
    request.local = local;
    request.name = std::move(name);
    request.args = std::move(args);
    request.parts = std::move(parts);
    deliver = host_func_ != nullptr;
  }
  if (deliver) {
    zebkit::PostEngineTask(base::BindOnce(&zebkit_library::DeliverRequest, base::WrapRefCounted(this), id),
                           base::TimeDelta());
  }
  zebkit::PostEngineTask(base::BindOnce(&zebkit_library::RequestDeadline, base::WrapRefCounted(this), id),
                         base::Milliseconds(options.timeout_ms));
  return id;
}

void zebkit_library::Answered(int i, int local, zebkit_library_code code, std::string json,
                              std::vector<std::string> parts) {
  zebkit::LibraryIsolate* runtime = instances[i]->runtime;
  if (runtime)
    runtime->Answered(local, code, json, parts);
}

// ---- The C API ----

extern "C" {

zebkit_library* zebkit_library_load(const char* path, const char* options_json, zebkit_library_func ready,
                                    void* data, char** error) {
  auto fail = [&](const std::string& message) -> zebkit_library* {
    if (error)
      *error = strdup(message.c_str());
    return nullptr;
  };
  if (error)
    *error = nullptr;
  if (!zebkit::CurrentlyOnEngineThread())
    return fail("zebkit_library_load() is for the engine's thread, once it runs");
  zebkit::LibraryOptions options;
  std::string message;
  if (!zebkit::ParseOptions(options_json, &options, &message))
    return fail(message);
  if (!path || !*path)
    return fail("no path");
  base::FilePath root, entry;
  bool single_file = false;
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath absolute = zebkit::RealPath(zebkit::PathFromUTF8(path));
    if (absolute.empty())
      return fail(std::string("no library at ") + path);
    if (base::DirectoryExists(absolute)) {
      root = absolute;
      entry = absolute.AppendASCII("index.mjs");
      if (!base::PathExists(entry) || base::DirectoryExists(entry))
        return fail(std::string("the folder ") + path + " has no index.mjs");
      entry = zebkit::RealPath(entry);
      if (entry.empty())
        return fail(std::string("the index.mjs of ") + path + " cannot be opened");
      if (!root.IsParent(entry))
        return fail(std::string("the index.mjs of ") + path + " is outside its folder");
    } else if (absolute.MatchesExtension(FILE_PATH_LITERAL(".mjs"))) {
      single_file = true;
      root = absolute.DirName();
      entry = absolute;
    } else {
      return fail(std::string(path) + " is neither a folder with index.mjs nor a .mjs file");
    }
  }
  // ---- The backend "page" (library_page.cc): a page, not a thread ----
  if (options.page) {
    scoped_refptr<zebkit_library> library = base::MakeRefCounted<zebkit_library>();
    library->options = std::move(options);
    library->root = root;
    library->entry = entry;
    library->single_file = single_file;
    library->ready_func = ready;
    library->ready_data = data;
    library->page = zebkit::LibraryPage::Start(library.get(), &message);
    if (!library->page)
      return fail(message);
    library->AddRef();
    return library.get();
  }
  // V8 as the host script has it (script.cc), its flags the same.
  if (!gin::IsolateHolder::Initialized()) {
    gin::IsolateHolder::Initialize(gin::IsolateHolder::kStrictMode, gin::ArrayBufferAllocator::SharedInstance(),
                                   nullptr,
                                   base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII("js-flags"));
  }
  scoped_refptr<zebkit_library> library = base::MakeRefCounted<zebkit_library>();
  for (int i = 0; i < options.instances; ++i) {
    auto instance = std::make_unique<zebkit_library::Instance>();
    instance->index = i;
    instance->thread = std::make_unique<base::Thread>("zebkit-library");
    base::Thread::Options thread_options;
    // Room under V8's own limit (about 1 MiB) on every system: macOS gives
    // other threads 512 KiB unless asked.
    thread_options.stack_size = size_t{8} << 20;
    if (!instance->thread->StartWithOptions(std::move(thread_options))) {
      for (auto& started : library->instances)
        started->thread->Stop();
      return fail("the library's thread did not start");
    }
    instance->runner = instance->thread->task_runner();
    library->instances.push_back(std::move(instance));
  }
  library->options = std::move(options);
  library->root = root;
  library->entry = entry;
  library->single_file = single_file;
  library->ready_func = ready;
  library->ready_data = data;
  for (auto& instance : library->instances) {
    instance->runner->PostTask(FROM_HERE, base::BindOnce(&zebkit_library::StartLoad, library, instance->index));
  }
  zebkit::PostEngineTask(base::BindOnce(&zebkit_library::Deadline, library, 0),
                         base::Milliseconds(library->options.timeout_ms));
  // The application's reference, let go by zebkit_library_close().
  library->AddRef();
  return library.get();
}

uint64_t zebkit_library_call(zebkit_library* library, const char* function, const char* args_json,
                             const zebkit_part* parts, size_t part_count, int64_t timeout_ms,
                             zebkit_library_func func, void* data) {
  if (!library)
    return 0;
  if (library->page)  // the backend "page"
    return library->page->Call(function, args_json, parts, part_count, timeout_ms, func, data);
  return library->Call(function, args_json, parts, part_count, timeout_ms, func, data);
}

void zebkit_library_cancel(zebkit_library* library, uint64_t call) {
  if (library && call && library->page)  // the backend "page"
    library->page->Cancel(call);
  else if (library && call)
    library->Cancel(call);
}

void zebkit_library_set_host(zebkit_library* library, zebkit_library_host_func func, void* data) {
  if (!library || !zebkit::CurrentlyOnEngineThread() || library->page)
    return;
  library->SetHost(func, data);
}

int zebkit_library_answer(zebkit_library* library, uint64_t request, zebkit_library_code code, const char* json,
                          const zebkit_part* parts, size_t part_count) {
  if (!library || !request || library->page)
    return 0;
  return library->Answer(request, code, json, parts, part_count);
}

void zebkit_library_close(zebkit_library* library) {
  if (!library || !zebkit::CurrentlyOnEngineThread())
    return;
  if (library->page) {  // the backend "page"; then the application's reference
    if (library->page->Close())
      library->Release();
    return;
  }
  library->Close();
}

}  // extern "C"
