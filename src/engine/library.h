// JavaScript libraries as normal libraries (<zebkit/library.h>,
// docs/JS_LIBRARIES.md): the backend "isolate".
//
// zebkit_library is what the application holds: its options, its queue of
// calls and their state, shared by the engine's thread (load, close, the
// callbacks, deadlines), the threads that call and cancel, and the
// library's own thread, under one lock. LibraryIsolate is the library's
// thread's: a V8 isolate made and used only there (gin's kSingleThread), its
// module, timers and fetches. library_web.cc has the Web APIs' native side.

#ifndef ZEBKIT_ENGINE_LIBRARY_H_
#define ZEBKIT_ENGINE_LIBRARY_H_

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "base/time/time.h"
#include "url/origin.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-function-callback.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"
#include "zebkit/engine/include/zebkit/library.h"

namespace base {
class Thread;
}
namespace gin {
class IsolateHolder;
}

namespace zebkit {

struct LibraryOptions {
  int64_t timeout_ms = 30000;
  size_t queue = 64;
  size_t heap_limit = size_t{128} << 20;
  size_t max_bytes = size_t{64} << 20;
  // The origins fetch() may reach.
  std::vector<url::Origin> network;
};

class LibraryIsolate;
class CountingAllocator;

// A call the application made, until it is told and has ended.
struct LibraryCall {
  uint64_t id = 0;
  std::string function;
  std::string args;
  std::vector<std::string> parts;
  int64_t timeout_ms = 0;
  zebkit_library_func func = nullptr;
  raw_ptr<void> data = nullptr;
  bool told = false;
};

using LibraryPosts = std::vector<base::OnceClosure>;

}  // namespace zebkit

struct zebkit_library : public base::RefCountedThreadSafe<zebkit_library> {
  enum class State { kLoading, kReady, kFailed, kEnded, kClosed };

  zebkit_library();

  // ---- Fixed by zebkit_library_load(), before the thread starts ----
  zebkit::LibraryOptions options;
  base::FilePath root;   // the folder imports stay inside
  base::FilePath entry;  // index.mjs, or the one file
  bool single_file = false;
  zebkit_library_func ready_func = nullptr;
  raw_ptr<void> ready_data = nullptr;
  scoped_refptr<base::SingleThreadTaskRunner> runner;  // the library's thread
  // The engine thread's; handed to the thread pool to be joined at close.
  std::unique_ptr<base::Thread> thread;
  // The library's thread's own; deleted there (DestroyHost()).
  raw_ptr<zebkit::LibraryIsolate> host = nullptr;

  // ---- The engine's thread ----
  void Close();
  // The deadline of call @id (0: the load's).
  void Deadline(uint64_t id);

  // ---- Any thread ----
  uint64_t Call(const char* function, const char* args_json, const zebkit_part* parts, size_t part_count,
                int64_t timeout_ms, zebkit_library_func func, void* data);
  void Cancel(uint64_t id);

  // ---- The library's thread ----
  void StartLoad();
  void StartNext();
  void DestroyHost();
  // The load ended: OK, or LOAD_FAILED with why.
  void Loaded(zebkit_library_code code, std::string json);
  // Call @id's function settled.
  void Settled(uint64_t id, zebkit_library_code code, std::string json, std::vector<std::string> parts);
  // Script is entered and left (the outermost entry); Left() says whether
  // the library is to end now, and with which code.
  void Entered();
  zebkit_library_code Left();
  // The heap reached its limit: the script is stopped.
  void OutOfMemory();
  // The script was stopped (Left() said so): the library ends.
  void End(zebkit_library_code code);

 private:
  friend class base::RefCountedThreadSafe<zebkit_library>;
  ~zebkit_library();

  void TellLocked(zebkit::LibraryCall* call, zebkit_library_code code, std::string json,
                  std::vector<std::string> parts, zebkit::LibraryPosts* posts) EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void TellReadyLocked(zebkit_library_code code, std::string json, zebkit::LibraryPosts* posts)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);
  // Every call not told yet told @code, the queue emptied.
  void FailAllLocked(zebkit_library_code code, const std::string& json, zebkit::LibraryPosts* posts)
      EXCLUSIVE_LOCKS_REQUIRED(lock_);
  void ScheduleNextLocked() EXCLUSIVE_LOCKS_REQUIRED(lock_);
  static void Post(zebkit::LibraryPosts posts);

  base::Lock lock_;
  State state_ GUARDED_BY(lock_) = State::kLoading;
  bool ready_told_ GUARDED_BY(lock_) = false;
  std::deque<uint64_t> queue_ GUARDED_BY(lock_);
  // Calls not told, and the one running, told or not.
  std::map<uint64_t, std::unique_ptr<zebkit::LibraryCall>> calls_ GUARDED_BY(lock_);
  uint64_t current_ GUARDED_BY(lock_) = 0;
  bool next_posted_ GUARDED_BY(lock_) = false;
  uint64_t next_id_ GUARDED_BY(lock_) = 1;
  // Script running on the library's thread (entries in progress).
  int depth_ GUARDED_BY(lock_) = 0;
  // Why the script was stopped: TIMEOUT, OUT_OF_MEMORY, GONE (closed).
  zebkit_library_code ending_ GUARDED_BY(lock_) = ZEBKIT_LIBRARY_OK;
  // For TerminateExecution() from other threads, while it lives.
  raw_ptr<v8::Isolate> isolate_ GUARDED_BY(lock_) = nullptr;

  friend class zebkit::LibraryIsolate;
};

namespace zebkit {

// The JSON of an error: {"message", "name", "stack"}.
std::string LibraryErrorJson(const std::string& message, const std::string& name, const std::string& stack = "");

class LibraryIsolate {
 public:
  explicit LibraryIsolate(scoped_refptr<zebkit_library> library);
  ~LibraryIsolate();
  LibraryIsolate(const LibraryIsolate&) = delete;
  LibraryIsolate& operator=(const LibraryIsolate&) = delete;

  // Makes the isolate, loads the module and evaluates it.
  void Load();
  void Run(uint64_t id, const std::string& function, const std::string& args, const std::vector<std::string>& parts);

  v8::Isolate* isolate() const { return isolate_; }
  v8::Local<v8::Context> context();
  zebkit_library* library() { return library_.get(); }
  bool ended() const { return ended_; }

  // Script entered from the library's thread: a microtask checkpoint as the
  // outermost one is left, and the library ends if it was stopped.
  struct Scope {
    explicit Scope(LibraryIsolate* host);
    ~Scope();
    raw_ptr<LibraryIsolate> host;
    v8::Isolate::Scope isolate_scope;
    v8::HandleScope handles;
    v8::Context::Scope context_scope;
  };

  // Native functions (library_web.cc for the Web APIs).
  static LibraryIsolate* From(const v8::FunctionCallbackInfo<v8::Value>& info);
  void SetMethod(v8::Local<v8::Object> object, const char* name, v8::FunctionCallback callback);
  void InstallWeb(v8::Local<v8::Object> native);
  void FetchDone(int id, std::string error, int status, std::string url, std::string headers, std::string body,
                 bool redirected);
  // What was thrown, as LibraryErrorJson().
  std::string Describe(v8::Local<v8::Value> exception);
  void Uncaught(const std::string& text);

 private:
  bool Create(std::string* error);
  v8::MaybeLocal<v8::Module> LoadModule(const base::FilePath& path);
  static v8::MaybeLocal<v8::Module> Resolve(v8::Local<v8::Context> context, v8::Local<v8::String> specifier,
                                            v8::Local<v8::FixedArray> attributes, v8::Local<v8::Module> referrer);
  static size_t NearHeapLimit(void* data, size_t current, size_t initial);
  void FireTimer(int id);
  void ReportRejections();

  static void Print(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SetTimer(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void ClearTimer(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void SettleCall(const v8::FunctionCallbackInfo<v8::Value>& info);
  static void LoadedCall(const v8::FunctionCallbackInfo<v8::Value>& info);
  // library_web.cc
  static void Fetch(const v8::FunctionCallbackInfo<v8::Value>& info);

  scoped_refptr<zebkit_library> library_;
  std::unique_ptr<CountingAllocator> allocator_;
  scoped_refptr<base::SingleThreadTaskRunner> tasks_;
  std::unique_ptr<gin::IsolateHolder> holder_;
  raw_ptr<v8::Isolate> isolate_ = nullptr;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Object> runner_;
  v8::Global<v8::Function> run_;
  int depth_ = 0;
  bool ended_ = false;
  struct Timer {
    v8::Global<v8::Function> function;
    bool repeat = false;
    int ms = 0;
  };
  std::map<int, Timer> timers_;
  int next_timer_ = 1;
  std::map<int, v8::Global<v8::Function>> fetches_;
  int next_fetch_ = 1;
  std::map<std::string, v8::Global<v8::Module>> modules_;
  std::multimap<int, std::string> module_paths_;
  std::vector<std::pair<v8::Global<v8::Promise>, v8::Global<v8::Value>>> unhandled_;
  base::TimeTicks start_;

  base::WeakPtrFactory<LibraryIsolate> weak_factory_{this};
};

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_LIBRARY_H_
