// Keep Chromium's platform pump: input, native dialogs and IO watches still
// work. Only scheduling is observed; the application decides when to pump.
#include "zebkit/engine/host_loop.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "base/message_loop/message_pump.h"
#include "base/message_loop/message_pump_for_ui.h"
#include "base/no_destructor.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_APPLE)
#include "base/message_loop/message_pump_apple.h"
#endif

namespace zebkit {
namespace {

struct Wakeup {
  base::Lock lock;
  base::ConditionVariable idle{&lock};
  zebkit_wakeup_func func = nullptr;
  void* data = nullptr;
  int calling = 0;
  base::PlatformThreadRef thread;
};

Wakeup& State() {
  static base::NoDestructor<Wakeup> state;
  return *state;
}

void Notify(int64_t delay_ms) {
  auto& state = State();
  zebkit_wakeup_func func;
  void* data;
  {
    base::AutoLock guard(state.lock);
    if (!state.func)
      return;
    func = state.func;
    data = state.data;
    ++state.calling;
  }
  // Not under the lock: the callback takes the host's own mutex, and the host
  // may hold that mutex while posting a task, which comes back here.
  func(delay_ms, data);
  base::AutoLock guard(state.lock);
  if (!--state.calling)
    state.idle.Broadcast();
}

void Notify(const base::MessagePump::Delegate::NextWorkInfo& info) {
  if (info.is_immediate())
    Notify(0);
  else if (!info.delayed_run_time.is_max())
    Notify(std::max<int64_t>(0,
        (info.delayed_run_time - base::TimeTicks::Now()).InMillisecondsRoundedUp()));
}

class ObservedDelegate : public base::MessagePump::Delegate {
 public:
  ObservedDelegate(Delegate* inner, bool* late) : late_(late), inner_(inner) {}
  NextWorkInfo DoWork() override {
    next_ = inner_->DoWork();
    *late_ = false;
    return next_;
  }
  const NextWorkInfo& next() const { return next_; }
  void DoIdleWork() override { inner_->DoIdleWork(); }
  void BeforeWait() override { inner_->BeforeWait(); }
  void BeginNativeWorkBeforeDoWork() override { inner_->BeginNativeWorkBeforeDoWork(); }
  int RunDepth() override { return inner_->RunDepth(); }

 private:
  void OnBeginWorkItem() override { work_.push_back(inner_->BeginWorkItem()); }
  void OnEndWorkItem(int) override { work_.pop_back(); }
  NextWorkInfo next_;
  bool* late_;
  Delegate* inner_;
  std::vector<ScopedDoWorkItem> work_;
};

// A MessagePumpForUI itself where that is a class of its own: Chromium casts
// the UI thread's pump to it (CurrentUIThread::WatchFileDescriptor, for Wayland's
// syncobj timelines), so anything else there is type confusion. On macOS it is
// just MessagePump, and the pump to wrap is chosen by the run loop.
#if BUILDFLAG(IS_APPLE)
using HostPumpBase = base::MessagePump;
#else
using HostPumpBase = base::MessagePumpForUI;
#endif

class HostPump : public HostPumpBase {
 public:
#if BUILDFLAG(IS_APPLE)
  explicit HostPump(std::unique_ptr<MessagePump> native) : native_(std::move(native)) {}
#endif
  void Run(Delegate* delegate) override {
    ObservedDelegate observed(delegate, &late_);
    ++depth_;
    NativeRun(&observed);
    if (!--depth_) {
      // Work scheduled after the last DoWork is in no deadline: without this
      // the host would go to sleep with it pending.
      if (late_)
        Notify(0);
      else
        Notify(observed.next());
      late_ = false;
    }
  }
  void ScheduleWork() override {
    NativeScheduleWork();
    // The current pump already processes owner-thread work. Reporting each
    // DoWork to a GLib idle source would itself create more native work,
    // preventing RunUntilIdle from ever reaching idle. Report the final
    // deadline after Run returns; worker notifications still wake the host.
    if (base::PlatformThread::CurrentRef() != State().thread || !depth_)
      Notify(0);
    else
      late_ = true;
  }
  void ScheduleDelayedWork(const Delegate::NextWorkInfo& info) override {
    NativeScheduleDelayedWork(info);
    if (base::PlatformThread::CurrentRef() != State().thread || !depth_)
      Notify(info);
    else
      late_ = true;
  }
#if BUILDFLAG(IS_APPLE)
  void Quit() override { native_->Quit(); }
  bool HandleNestedNativeLoopWithApplicationTasks(bool desired) override {
    return native_->HandleNestedNativeLoopWithApplicationTasks(desired);
  }
  base::IOWatcher* GetIOWatcher() override { return native_->GetIOWatcher(); }
  bool IsAsyncIOSupported() override { return native_->IsAsyncIOSupported(); }
#endif

 private:
#if BUILDFLAG(IS_APPLE)
  void NativeRun(Delegate* delegate) { native_->Run(delegate); }
  void NativeScheduleWork() { native_->ScheduleWork(); }
  void NativeScheduleDelayedWork(const Delegate::NextWorkInfo& info) { native_->ScheduleDelayedWork(info); }
  std::unique_ptr<MessagePump> native_;
#else
  void NativeRun(Delegate* delegate) { HostPumpBase::Run(delegate); }
  void NativeScheduleWork() { HostPumpBase::ScheduleWork(); }
  void NativeScheduleDelayedWork(const Delegate::NextWorkInfo& info) { HostPumpBase::ScheduleDelayedWork(info); }
#endif
  int depth_ = 0;
  bool late_ = false;
};

std::unique_ptr<base::MessagePump> CreatePump() {
#if BUILDFLAG(IS_APPLE)
  auto native = base::message_pump_apple::Create();
  // Other UI threads keep running their own ordinary loops.
  if (base::PlatformThread::CurrentRef() != State().thread)
    return native;
  return std::make_unique<HostPump>(std::move(native));
#else
  if (base::PlatformThread::CurrentRef() != State().thread)
    return std::make_unique<base::MessagePumpForUI>();
  return std::make_unique<HostPump>();
#endif
}

}  // namespace

void ConfigureHostWakeup(zebkit_wakeup_func func, void* data) {
  auto& state = State();
  base::AutoLock guard(state.lock);
  state.func = func;
  state.data = data;
  // A lifetime barrier for the previous callback's data.
  while (state.calling)
    state.idle.Wait();
}

bool InstallHostLoop() {
  if (base::MessagePump::IsMessagePumpForUIFactoryOveridden())
    return false;
  State().thread = base::PlatformThread::CurrentRef();
  base::MessagePump::OverrideMessagePumpForUIFactory(&CreatePump);
  return true;
}

void StopHostLoop() { ConfigureHostWakeup(nullptr, nullptr); }

}  // namespace zebkit
