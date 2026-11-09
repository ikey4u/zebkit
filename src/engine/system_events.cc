// What the system tells of itself (docs/SYSTEM_INTEGRATION.md, section 14):
// suspend and resume and the power source, from base's PowerMonitor which the
// engine runs; the screens, from display::Screen; and keeping the system
// awake for the application, through the device service's wake locks.

#include <string.h>

#include <map>
#include <optional>
#include <memory>
#include <string>
#include <utility>

#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/power_monitor/power_monitor.h"
#include "base/power_monitor/power_observer.h"
#include "base/strings/string_number_conversions.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/device_service.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/mojom/wake_lock.mojom.h"
#include "services/device/public/mojom/wake_lock_provider.mojom.h"
#include "ui/base/idle/idle.h"
#include "ui/display/display.h"
#include "ui/display/display_observer.h"
#include "ui/display/screen.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

namespace {

bool OnEngineThread() {
  return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI);
}

void Tell(zebkit_system_event_func func, void* data, base::DictValue event) {
  if (!func)
    return;
  const std::string json = base::WriteJson(event).value_or("{}");
  func(json.c_str(), data);
}

class PowerEvents : public base::PowerSuspendObserver, public base::PowerStateObserver {
 public:
  void Set(zebkit_system_event_func func, void* data) {
    auto* monitor = base::PowerMonitor::GetInstance();
    if (func && !func_) {
      monitor->AddPowerSuspendObserver(this);
      monitor->AddPowerStateObserver(this);
      // The session's lock: the system's own state (ui/base/idle), asked each
      // second while the application listens, told when it changes.
      locked_ = ui::CheckIdleStateIsLocked();
      lock_check_.Start(FROM_HERE, base::Seconds(1), this, &PowerEvents::CheckLock);
    } else if (!func && func_) {
      monitor->RemovePowerSuspendObserver(this);
      monitor->RemovePowerStateObserver(this);
      lock_check_.Stop();
    }
    func_ = func;
    data_ = data;
  }
  void OnSuspend() override { Event("suspend"); }
  void OnResume() override { Event("resume"); }
  void OnBatteryPowerStatusChange(BatteryPowerStatus status) override {
    base::DictValue event;
    event.Set("event", "power-source");
    event.Set("onBattery", status == BatteryPowerStatus::kBatteryPower);
    Tell(func_, data_, std::move(event));
  }

 private:
  void Event(const char* name) {
    base::DictValue event;
    event.Set("event", name);
    Tell(func_, data_, std::move(event));
  }
  void CheckLock() {
    const bool locked = ui::CheckIdleStateIsLocked();
    if (locked == locked_)
      return;
    locked_ = locked;
    Event(locked ? "lock" : "unlock");
  }
  bool locked_ = false;
  base::RepeatingTimer lock_check_;
  zebkit_system_event_func func_ = nullptr;
  void* data_ = nullptr;
};

PowerEvents& Power() {
  static base::NoDestructor<PowerEvents> power;
  return *power;
}

base::DictValue RectValue(const gfx::Rect& rect) {
  base::DictValue value;
  value.Set("x", rect.x());
  value.Set("y", rect.y());
  value.Set("width", rect.width());
  value.Set("height", rect.height());
  return value;
}

base::ListValue Displays() {
  base::ListValue list;
  display::Screen* screen = display::Screen::Get();
  if (!screen)
    return list;
  const int64_t primary = screen->GetPrimaryDisplay().id();
  for (const display::Display& display : screen->GetAllDisplays()) {
    base::DictValue value;
    value.Set("id", base::NumberToString(display.id()));
    value.Set("bounds", RectValue(display.bounds()));
    value.Set("workArea", RectValue(display.work_area()));
    value.Set("scale", display.device_scale_factor());
    value.Set("rotation", display.RotationAsDegree());
    value.Set("primary", display.id() == primary);
    list.Append(std::move(value));
  }
  return list;
}

class DisplayEvents : public display::DisplayObserver {
 public:
  void Set(zebkit_system_event_func func, void* data) {
    display::Screen* screen = display::Screen::Get();
    if (func && !func_ && screen)
      observation_.emplace(this);
    else if (!func)
      observation_.reset();
    func_ = func;
    data_ = data;
  }
  void OnDisplayAdded(const display::Display&) override { Changed(); }
  void OnDisplaysRemoved(const display::Displays&) override { Changed(); }
  void OnDisplayMetricsChanged(const display::Display&, uint32_t) override { Changed(); }

 private:
  void Changed() {
    base::DictValue event;
    event.Set("event", "displays");
    event.Set("displays", Displays());
    Tell(func_, data_, std::move(event));
  }
  zebkit_system_event_func func_ = nullptr;
  void* data_ = nullptr;
  std::optional<display::ScopedDisplayObserver> observation_;
};

DisplayEvents& DisplayWatch() {
  static base::NoDestructor<DisplayEvents> displays;
  return *displays;
}

struct SleepBlocks {
  uint64_t next = 1;
  std::map<uint64_t, mojo::Remote<device::mojom::WakeLock>> held;
  mojo::Remote<device::mojom::WakeLockProvider> provider;
};

SleepBlocks& Blocks() {
  static base::NoDestructor<SleepBlocks> blocks;
  return *blocks;
}

}  // namespace

}  // namespace zebkit

extern "C" {

void zebkit_on_power_event(zebkit_system_event_func func, void* data) {
  if (zebkit::OnEngineThread())
    zebkit::Power().Set(func, data);
}

char* zebkit_get_displays(void) {
  if (!zebkit::OnEngineThread())
    return nullptr;
  return strdup(base::WriteJson(zebkit::Displays()).value_or("[]").c_str());
}

void zebkit_on_displays_changed(zebkit_system_event_func func, void* data) {
  if (zebkit::OnEngineThread())
    zebkit::DisplayWatch().Set(func, data);
}

uint64_t zebkit_prevent_sleep(bool keep_display_on, const char* reason) {
  if (!zebkit::OnEngineThread())
    return 0;
  auto& blocks = zebkit::Blocks();
  if (!blocks.provider)
    content::GetDeviceService().BindWakeLockProvider(blocks.provider.BindNewPipeAndPassReceiver());
  mojo::Remote<device::mojom::WakeLock> lock;
  blocks.provider->GetWakeLockWithoutContext(
      keep_display_on ? device::mojom::WakeLockType::kPreventDisplaySleep
                      : device::mojom::WakeLockType::kPreventAppSuspension,
      device::mojom::WakeLockReason::kOther, reason && *reason ? reason : "zebkit application",
      lock.BindNewPipeAndPassReceiver());
  lock->RequestWakeLock();
  const uint64_t token = blocks.next++;
  blocks.held[token] = std::move(lock);
  return token;
}

bool zebkit_allow_sleep(uint64_t token) {
  if (!zebkit::OnEngineThread())
    return false;
  auto& held = zebkit::Blocks().held;
  auto it = held.find(token);
  if (it == held.end())
    return false;
  it->second->CancelWakeLock();
  held.erase(it);
  return true;
}

}  // extern "C"
