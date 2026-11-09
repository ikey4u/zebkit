#include "zebkit/engine/device_choosers.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/values.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/hid_chooser.h"
#include "content/public/browser/hid_delegate.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/serial_chooser.h"
#include "content/public/browser/serial_delegate.h"
#include "content/public/browser/usb_chooser.h"
#include "content/public/browser/usb_delegate.h"
#include "mojo/public/cpp/bindings/associated_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/cpp/usb/usb_utils.h"
#include "services/device/public/mojom/hid.mojom.h"
#include "services/device/public/mojom/serial.mojom.h"
#include "services/device/public/mojom/usb_manager.mojom.h"
#include "services/device/public/mojom/usb_manager_client.mojom.h"
#include "third_party/blink/public/mojom/hid/hid.mojom.h"
#include "third_party/blink/public/mojom/serial/serial.mojom.h"
#include "third_party/blink/public/mojom/usb/web_usb_service.mojom.h"
#include "url/origin.h"
#include "zebkit/engine/page_hooks.h"

namespace zebkit {

namespace {

// What an origin was given, by its top frame's origin, for the engine's run.
class Grants {
 public:
  using Key = std::pair<content::BrowserContext*, url::Origin>;
  void Add(const Key& key, const std::string& id) { granted_[key].insert(id); }
  bool Has(const Key& key, const std::string& id) const {
    auto it = granted_.find(key);
    return it != granted_.end() && it->second.contains(id);
  }
  bool Remove(const Key& key, const std::string& id) {
    auto it = granted_.find(key);
    return it != granted_.end() && it->second.erase(id);
  }

 private:
  std::map<Key, std::set<std::string>> granted_;
};

Grants::Key KeyOf(content::RenderFrameHost* frame) {
  return {frame->GetBrowserContext(), frame->GetMainFrame()->GetLastCommittedOrigin()};
}

// One page's chooser while the application decides; its list follows the
// devices that come and go (Refresh), and it ends what was asked when content
// drops it.
class Asking {
 public:
  explicit Asking(content::RenderFrameHost* frame) : frame_(frame->GetGlobalId()) {}
  ~Asking() {
    if (id_)
      AbandonDeviceChooser(*id_);
  }
  content::RenderFrameHost* frame() const { return content::RenderFrameHost::FromID(frame_); }

  // Asks; false without a function (or a frame), @choice unrun.
  bool Ask(const char* kind, base::Value filters, base::ListValue devices, DeviceChoice& choice) {
    content::RenderFrameHost* host = frame();
    if (!host)
      return false;
    id_ = AskDeviceChooser(host, kind, std::move(filters), std::move(devices), choice);
    return id_.has_value();
  }
  void Refresh(base::ListValue devices) {
    if (id_ && *id_)
      UpdateDeviceChooser(*id_, std::move(devices));
  }
  void Done() { id_.reset(); }

 private:
  content::GlobalRenderFrameHostId frame_;
  std::optional<uintptr_t> id_;
};

void Later(base::OnceClosure task) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(FROM_HERE, std::move(task));
}

std::string Text(const std::optional<std::u16string>& text) {
  return text ? base::UTF16ToUTF8(*text) : std::string();
}

// ---- WebUSB ---------------------------------------------------------------

class UsbDevices : public content::UsbDelegate, public device::mojom::UsbDeviceManagerClient {
 public:
  class Chooser : public content::UsbChooser {
   public:
    Chooser(UsbDevices* owner, content::RenderFrameHost& frame, blink::mojom::WebUsbRequestDeviceOptionsPtr options,
            blink::mojom::WebUsbService::GetPermissionCallback callback)
        : owner_(owner), asking_(&frame), options_(std::move(options)), callback_(std::move(callback)) {
      owner_->choosers_.insert(this);
    }
    ~Chooser() override { owner_->choosers_.erase(this); }

    void Ask() {
      DeviceChoice choice = base::BindOnce(&Chooser::Chosen, weak_.GetWeakPtr());
      if (!asking_.Ask("usb", Filters(), owner_->List(*options_), choice))
        Later(base::BindOnce(&Chooser::Chosen, weak_.GetWeakPtr(), std::nullopt));
    }
    void Refresh() { asking_.Refresh(owner_->List(*options_)); }
    base::WeakPtr<Chooser> GetWeakPtr() { return weak_.GetWeakPtr(); }

   private:
    base::Value Filters() const {
      base::ListValue list;
      for (const auto& filter : options_->filters) {
        base::DictValue entry;
        if (filter->vendor_id) entry.Set("vendorId", *filter->vendor_id);
        if (filter->product_id) entry.Set("productId", *filter->product_id);
        if (filter->class_code) entry.Set("classCode", *filter->class_code);
        if (filter->subclass_code) entry.Set("subclassCode", *filter->subclass_code);
        if (filter->protocol_code) entry.Set("protocolCode", *filter->protocol_code);
        if (filter->serial_number) entry.Set("serialNumber", base::UTF16ToUTF8(*filter->serial_number));
        list.Append(std::move(entry));
      }
      return base::Value(std::move(list));
    }
    void Chosen(std::optional<std::string> id) {
      asking_.Done();
      if (!callback_)
        return;
      content::RenderFrameHost* frame = asking_.frame();
      auto it = id ? owner_->devices_.find(*id) : owner_->devices_.end();
      if (!frame || it == owner_->devices_.end()) {
        std::move(callback_).Run(nullptr);
        return;
      }
      owner_->grants_.Add(KeyOf(frame), it->first);
      std::move(callback_).Run(it->second.Clone());
    }

    raw_ptr<UsbDevices> owner_;
    Asking asking_;
    blink::mojom::WebUsbRequestDeviceOptionsPtr options_;
    blink::mojom::WebUsbService::GetPermissionCallback callback_;
    base::WeakPtrFactory<Chooser> weak_{this};
  };

  void AdjustProtectedInterfaceClasses(content::BrowserContext*, const url::Origin&, content::RenderFrameHost*,
                                       std::vector<uint8_t>&) override {}

  std::unique_ptr<content::UsbChooser> RunChooser(content::RenderFrameHost& frame,
                                                  blink::mojom::WebUsbRequestDeviceOptionsPtr options,
                                                  blink::mojom::WebUsbService::GetPermissionCallback callback) override {
    Connect();
    auto chooser = std::make_unique<Chooser>(this, frame, std::move(options), std::move(callback));
    if (enumerated_)
      chooser->Ask();
    else
      waiting_.push_back(base::BindOnce(&Chooser::Ask, chooser->GetWeakPtr()));
    return chooser;
  }

  bool PageMayUseUsb(content::Page&) override { return true; }
  bool CanRequestDevicePermission(content::BrowserContext*, const url::Origin&) override { return true; }

  void RevokeDevicePermissionWebInitiated(content::BrowserContext* context, const url::Origin& origin,
                                          const device::mojom::UsbDeviceInfo& device) override {
    if (grants_.Remove({context, origin}, device.guid))
      for (auto& observer : observers_)
        observer.OnPermissionRevoked(origin);
  }

  const device::mojom::UsbDeviceInfo* GetDeviceInfo(content::BrowserContext*, const std::string& guid) override {
    auto it = devices_.find(guid);
    return it == devices_.end() ? nullptr : it->second.get();
  }

  bool HasDevicePermission(content::BrowserContext* context, content::RenderFrameHost*, const url::Origin& origin,
                           const device::mojom::UsbDeviceInfo& device) override {
    return grants_.Has({context, origin}, device.guid);
  }

  void GetDevices(content::BrowserContext*, blink::mojom::WebUsbService::GetDevicesCallback callback) override {
    Connect();
    auto give = [](UsbDevices* self, blink::mojom::WebUsbService::GetDevicesCallback callback) {
      std::vector<device::mojom::UsbDeviceInfoPtr> list;
      for (const auto& [guid, device] : self->devices_)
        list.push_back(device.Clone());
      std::move(callback).Run(std::move(list));
    };
    if (enumerated_)
      give(this, std::move(callback));
    else
      waiting_.push_back(base::BindOnce(give, base::Unretained(this), std::move(callback)));
  }

  void GetDevice(content::BrowserContext*, const std::string& guid, base::span<const uint8_t> blocked_interface_classes,
                 mojo::PendingReceiver<device::mojom::UsbDevice> device_receiver,
                 mojo::PendingRemote<device::mojom::UsbDeviceClient> device_client) override {
    Connect();
    manager_->GetDevice(guid, std::vector<uint8_t>(blocked_interface_classes.begin(), blocked_interface_classes.end()),
                        std::move(device_receiver), std::move(device_client));
  }

  void AddObserver(content::BrowserContext*, Observer* observer) override { observers_.AddObserver(observer); }
  void RemoveObserver(content::BrowserContext*, Observer* observer) override { observers_.RemoveObserver(observer); }
  bool IsServiceWorkerAllowedForOrigin(const url::Origin&) override { return false; }
  void IncrementConnectionCount(content::BrowserContext*, const url::Origin&) override {}
  void DecrementConnectionCount(content::BrowserContext*, const url::Origin&) override {}

  // device::mojom::UsbDeviceManagerClient
  void OnDeviceAdded(device::mojom::UsbDeviceInfoPtr device) override {
    const device::mojom::UsbDeviceInfo& info = *device;
    devices_[device->guid] = std::move(device);
    for (auto& observer : observers_)
      observer.OnDeviceAdded(info);
    Changed();
  }
  void OnDeviceRemoved(device::mojom::UsbDeviceInfoPtr device) override {
    devices_.erase(device->guid);
    for (auto& observer : observers_)
      observer.OnDeviceRemoved(*device);
    Changed();
  }

 private:
  void Connect() {
    if (manager_)
      return;
    content::GetDeviceService().BindUsbDeviceManager(manager_.BindNewPipeAndPassReceiver());
    manager_.set_disconnect_handler(base::BindOnce(&UsbDevices::Lost, base::Unretained(this)));
    manager_->EnumerateDevicesAndSetClient(client_.BindNewEndpointAndPassRemote(),
                                           base::BindOnce(&UsbDevices::Enumerated, base::Unretained(this)));
  }
  void Enumerated(std::vector<device::mojom::UsbDeviceInfoPtr> devices) {
    for (auto& device : devices)
      devices_[device->guid] = std::move(device);
    enumerated_ = true;
    auto waiting = std::move(waiting_);
    for (auto& task : waiting)
      std::move(task).Run();
  }
  void Lost() {
    manager_.reset();
    client_.reset();
    devices_.clear();
    enumerated_ = false;
    for (auto& observer : observers_)
      observer.OnDeviceManagerConnectionError();
    // What waited gets an empty list rather than nothing.
    enumerated_ = true;
    auto waiting = std::move(waiting_);
    for (auto& task : waiting)
      std::move(task).Run();
    enumerated_ = false;
  }
  void Changed() {
    for (Chooser* chooser : choosers_)
      chooser->Refresh();
  }

  base::ListValue List(const blink::mojom::WebUsbRequestDeviceOptions& options) const {
    base::ListValue list;
    for (const auto& [guid, device] : devices_) {
      if (!options.filters.empty() && !device::UsbDeviceFilterMatchesAny(options.filters, *device))
        continue;
      if (!options.exclusion_filters.empty() && device::UsbDeviceFilterMatchesAny(options.exclusion_filters, *device))
        continue;
      base::DictValue entry;
      entry.Set("id", guid);
      const std::string product = Text(device->product_name);
      entry.Set("name", product.empty() ? "USB device" : product);
      entry.Set("vendorId", device->vendor_id);
      entry.Set("productId", device->product_id);
      entry.Set("manufacturer", Text(device->manufacturer_name));
      entry.Set("serialNumber", Text(device->serial_number));
      list.Append(std::move(entry));
    }
    return list;
  }

  mojo::Remote<device::mojom::UsbDeviceManager> manager_;
  mojo::AssociatedReceiver<device::mojom::UsbDeviceManagerClient> client_{this};
  bool enumerated_ = false;
  std::vector<base::OnceClosure> waiting_;
  std::map<std::string, device::mojom::UsbDeviceInfoPtr> devices_;
  std::set<Chooser*> choosers_;
  Grants grants_;
  base::ObserverList<Observer> observers_;
};

// ---- WebHID ---------------------------------------------------------------

bool HidMatches(const blink::mojom::HidDeviceFilter& filter, const device::mojom::HidDeviceInfo& device) {
  if (filter.device_ids) {
    if (filter.device_ids->is_vendor() && filter.device_ids->get_vendor() != device.vendor_id)
      return false;
    if (filter.device_ids->is_vendor_and_product()) {
      const auto& wanted = filter.device_ids->get_vendor_and_product();
      if (wanted->vendor != device.vendor_id || wanted->product != device.product_id)
        return false;
    }
  }
  if (filter.usage) {
    bool found = false;
    for (const auto& collection : device.collections) {
      if (filter.usage->is_page())
        found |= collection->usage->usage_page == filter.usage->get_page();
      else if (filter.usage->is_usage_and_page())
        found |= collection->usage->usage_page == filter.usage->get_usage_and_page()->usage_page &&
                 collection->usage->usage == filter.usage->get_usage_and_page()->usage;
    }
    if (!found)
      return false;
  }
  return true;
}

// Whether the page could use any of @device's reports: content drops the
// collections whose reports are all protected (HidService::RemoveProtectedReports),
// keyboards' and mice's, so a device of only those is not offered, as in Chrome.
bool HidUsable(const device::mojom::HidDeviceInfo& device) {
  auto open = [](const std::vector<device::mojom::HidReportDescriptionPtr>& reports,
                 const std::optional<std::vector<uint8_t>>& protected_ids) {
    for (const auto& report : reports)
      if (!protected_ids || !std::ranges::contains(*protected_ids, report->report_id))
        return true;
    return false;
  };
  for (const auto& collection : device.collections) {
    if (open(collection->input_reports, device.protected_input_report_ids) ||
        open(collection->output_reports, device.protected_output_report_ids) ||
        open(collection->feature_reports, device.protected_feature_report_ids))
      return true;
  }
  return false;
}

bool HidMatchesAny(const std::vector<blink::mojom::HidDeviceFilterPtr>& filters,
                   const device::mojom::HidDeviceInfo& device) {
  for (const auto& filter : filters)
    if (HidMatches(*filter, device))
      return true;
  return false;
}

// What the page sees as one device: a physical device's interfaces together.
std::string PhysicalId(const device::mojom::HidDeviceInfo& device) {
  return device.physical_device_id.empty() ? device.guid : device.physical_device_id;
}

class HidDevices : public content::HidDelegate, public device::mojom::HidManagerClient {
 public:
  class Chooser : public content::HidChooser {
   public:
    Chooser(HidDevices* owner, content::RenderFrameHost* frame, std::vector<blink::mojom::HidDeviceFilterPtr> filters,
            std::vector<blink::mojom::HidDeviceFilterPtr> exclusions, content::HidChooser::Callback callback)
        : owner_(owner),
          asking_(frame),
          filters_(std::move(filters)),
          exclusions_(std::move(exclusions)),
          callback_(std::move(callback)) {
      owner_->choosers_.insert(this);
    }
    ~Chooser() override { owner_->choosers_.erase(this); }

    void Ask() {
      DeviceChoice choice = base::BindOnce(&Chooser::Chosen, weak_.GetWeakPtr());
      if (!asking_.Ask("hid", Filters(), owner_->List(filters_, exclusions_), choice))
        Later(base::BindOnce(&Chooser::Chosen, weak_.GetWeakPtr(), std::nullopt));
    }
    void Refresh() { asking_.Refresh(owner_->List(filters_, exclusions_)); }
    base::WeakPtr<Chooser> GetWeakPtr() { return weak_.GetWeakPtr(); }

   private:
    base::Value Filters() const {
      base::ListValue list;
      for (const auto& filter : filters_) {
        base::DictValue entry;
        if (filter->device_ids && filter->device_ids->is_vendor())
          entry.Set("vendorId", filter->device_ids->get_vendor());
        if (filter->device_ids && filter->device_ids->is_vendor_and_product()) {
          entry.Set("vendorId", filter->device_ids->get_vendor_and_product()->vendor);
          entry.Set("productId", filter->device_ids->get_vendor_and_product()->product);
        }
        if (filter->usage && filter->usage->is_page())
          entry.Set("usagePage", filter->usage->get_page());
        if (filter->usage && filter->usage->is_usage_and_page()) {
          entry.Set("usagePage", filter->usage->get_usage_and_page()->usage_page);
          entry.Set("usage", filter->usage->get_usage_and_page()->usage);
        }
        list.Append(std::move(entry));
      }
      return base::Value(std::move(list));
    }
    void Chosen(std::optional<std::string> id) {
      asking_.Done();
      if (!callback_)
        return;
      std::vector<device::mojom::HidDeviceInfoPtr> chosen;
      content::RenderFrameHost* frame = asking_.frame();
      if (frame && id) {
        for (const auto& [guid, device] : owner_->devices_) {
          if (PhysicalId(*device) != *id || !owner_->Shown(*device, filters_, exclusions_))
            continue;
          owner_->grants_.Add(KeyOf(frame), guid);
          chosen.push_back(device.Clone());
        }
      }
      std::move(callback_).Run(std::move(chosen));
    }

    raw_ptr<HidDevices> owner_;
    Asking asking_;
    std::vector<blink::mojom::HidDeviceFilterPtr> filters_;
    std::vector<blink::mojom::HidDeviceFilterPtr> exclusions_;
    content::HidChooser::Callback callback_;
    base::WeakPtrFactory<Chooser> weak_{this};
  };

  std::unique_ptr<content::HidChooser> RunChooser(content::RenderFrameHost* frame,
                                                  std::vector<blink::mojom::HidDeviceFilterPtr> filters,
                                                  std::vector<blink::mojom::HidDeviceFilterPtr> exclusions,
                                                  content::HidChooser::Callback callback) override {
    Connect();
    auto chooser =
        std::make_unique<Chooser>(this, frame, std::move(filters), std::move(exclusions), std::move(callback));
    if (enumerated_)
      chooser->Ask();
    else
      waiting_.push_back(base::BindOnce(&Chooser::Ask, chooser->GetWeakPtr()));
    return chooser;
  }

  bool IsHidAllowedForFrame(content::RenderFrameHost*) override { return true; }
  bool CanRequestDevicePermission(content::BrowserContext*, const url::Origin&) override { return true; }
  bool HasDevicePermission(content::BrowserContext* context, content::RenderFrameHost*, const url::Origin& origin,
                           const device::mojom::HidDeviceInfo& device) override {
    return grants_.Has({context, origin}, device.guid);
  }
  void RevokeDevicePermission(content::BrowserContext* context, content::RenderFrameHost*, const url::Origin& origin,
                              const device::mojom::HidDeviceInfo& device) override {
    // The physical device's interfaces go together, as they were given.
    bool revoked = false;
    for (const auto& [guid, known] : devices_)
      if (PhysicalId(*known) == PhysicalId(device))
        revoked |= grants_.Remove({context, origin}, guid);
    revoked |= grants_.Remove({context, origin}, device.guid);
    if (revoked)
      for (auto& observer : observers_)
        observer.OnPermissionRevoked(origin);
  }
  device::mojom::HidManager* GetHidManager(content::BrowserContext*) override {
    Connect();
    return manager_.get();
  }
  void AddObserver(content::BrowserContext*, Observer* observer) override { observers_.AddObserver(observer); }
  void RemoveObserver(content::BrowserContext*, Observer* observer) override { observers_.RemoveObserver(observer); }
  bool IsFidoAllowedForOrigin(content::BrowserContext*, const url::Origin&) override { return false; }
  bool IsServiceWorkerAllowedForOrigin(const url::Origin&) override { return false; }
  const device::mojom::HidDeviceInfo* GetDeviceInfo(content::BrowserContext*, const std::string& guid) override {
    auto it = devices_.find(guid);
    return it == devices_.end() ? nullptr : it->second.get();
  }
  void IncrementConnectionCount(content::BrowserContext*, const url::Origin&) override {}
  void DecrementConnectionCount(content::BrowserContext*, const url::Origin&) override {}

  // device::mojom::HidManagerClient
  void DeviceAdded(device::mojom::HidDeviceInfoPtr device) override {
    const device::mojom::HidDeviceInfo& info = *device;
    devices_[device->guid] = std::move(device);
    for (auto& observer : observers_)
      observer.OnDeviceAdded(info);
    Changed();
  }
  void DeviceRemoved(device::mojom::HidDeviceInfoPtr device) override {
    devices_.erase(device->guid);
    for (auto& observer : observers_)
      observer.OnDeviceRemoved(*device);
    Changed();
  }
  void DeviceChanged(device::mojom::HidDeviceInfoPtr device) override {
    const device::mojom::HidDeviceInfo& info = *device;
    devices_[device->guid] = std::move(device);
    for (auto& observer : observers_)
      observer.OnDeviceChanged(info);
    Changed();
  }

 private:
  void Connect() {
    if (manager_)
      return;
    content::GetDeviceService().BindHidManager(manager_.BindNewPipeAndPassReceiver());
    manager_.set_disconnect_handler(base::BindOnce(&HidDevices::Lost, base::Unretained(this)));
    manager_->GetDevicesAndSetClient(client_.BindNewEndpointAndPassRemote(),
                                     base::BindOnce(&HidDevices::Enumerated, base::Unretained(this)));
  }
  void Enumerated(std::vector<device::mojom::HidDeviceInfoPtr> devices) {
    for (auto& device : devices)
      devices_[device->guid] = std::move(device);
    enumerated_ = true;
    auto waiting = std::move(waiting_);
    for (auto& task : waiting)
      std::move(task).Run();
  }
  void Lost() {
    manager_.reset();
    client_.reset();
    devices_.clear();
    for (auto& observer : observers_)
      observer.OnHidManagerConnectionError();
    enumerated_ = true;
    auto waiting = std::move(waiting_);
    for (auto& task : waiting)
      std::move(task).Run();
    enumerated_ = false;
  }
  void Changed() {
    for (Chooser* chooser : choosers_)
      chooser->Refresh();
  }

  bool Shown(const device::mojom::HidDeviceInfo& device, const std::vector<blink::mojom::HidDeviceFilterPtr>& filters,
             const std::vector<blink::mojom::HidDeviceFilterPtr>& exclusions) const {
    // Keyboards, mice and security keys stay the system's, as in Chrome.
    if (device.is_excluded_by_blocklist || !HidUsable(device))
      return false;
    if (!filters.empty() && !HidMatchesAny(filters, device))
      return false;
    return exclusions.empty() || !HidMatchesAny(exclusions, device);
  }

  base::ListValue List(const std::vector<blink::mojom::HidDeviceFilterPtr>& filters,
                       const std::vector<blink::mojom::HidDeviceFilterPtr>& exclusions) const {
    std::map<std::string, base::DictValue> physical;
    for (const auto& [guid, device] : devices_) {
      if (!Shown(*device, filters, exclusions))
        continue;
      base::DictValue& entry = physical[PhysicalId(*device)];
      if (entry.empty()) {
        entry.Set("id", PhysicalId(*device));
        entry.Set("name", device->product_name.empty() ? "HID device" : device->product_name);
        entry.Set("vendorId", device->vendor_id);
        entry.Set("productId", device->product_id);
        entry.Set("serialNumber", device->serial_number);
        entry.Set("collections", base::ListValue());
      }
      for (const auto& collection : device->collections) {
        base::DictValue usage;
        usage.Set("usagePage", collection->usage->usage_page);
        usage.Set("usage", collection->usage->usage);
        entry.FindList("collections")->Append(std::move(usage));
      }
    }
    base::ListValue list;
    for (auto& [id, entry] : physical)
      list.Append(std::move(entry));
    return list;
  }

  mojo::Remote<device::mojom::HidManager> manager_;
  mojo::AssociatedReceiver<device::mojom::HidManagerClient> client_{this};
  bool enumerated_ = false;
  std::vector<base::OnceClosure> waiting_;
  std::map<std::string, device::mojom::HidDeviceInfoPtr> devices_;
  std::set<Chooser*> choosers_;
  Grants grants_;
  base::ObserverList<Observer> observers_;
};

// ---- Web Serial -----------------------------------------------------------

bool SerialMatches(const blink::mojom::SerialPortFilter& filter, const device::mojom::SerialPortInfo& port) {
  if (filter.bluetooth_service_class_id)
    return port.bluetooth_service_class_id && *port.bluetooth_service_class_id == *filter.bluetooth_service_class_id;
  if (filter.has_vendor_id && (!port.has_vendor_id || port.vendor_id != filter.vendor_id))
    return false;
  if (filter.has_product_id && (!port.has_product_id || port.product_id != filter.product_id))
    return false;
  return true;
}

class SerialPorts : public content::SerialDelegate, public device::mojom::SerialPortManagerClient {
 public:
  class Chooser : public content::SerialChooser {
   public:
    Chooser(SerialPorts* owner, content::RenderFrameHost* frame, std::vector<blink::mojom::SerialPortFilterPtr> filters,
            std::vector<device::BluetoothUUID> services, content::SerialChooser::Callback callback)
        : owner_(owner),
          asking_(frame),
          filters_(std::move(filters)),
          services_(std::move(services)),
          callback_(std::move(callback)) {
      owner_->choosers_.insert(this);
    }
    ~Chooser() override { owner_->choosers_.erase(this); }

    void Ask() {
      DeviceChoice choice = base::BindOnce(&Chooser::Chosen, weak_.GetWeakPtr());
      if (!asking_.Ask("serial", Filters(), owner_->List(filters_, services_), choice))
        Later(base::BindOnce(&Chooser::Chosen, weak_.GetWeakPtr(), std::nullopt));
    }
    void Refresh() { asking_.Refresh(owner_->List(filters_, services_)); }
    base::WeakPtr<Chooser> GetWeakPtr() { return weak_.GetWeakPtr(); }

   private:
    base::Value Filters() const {
      base::ListValue list;
      for (const auto& filter : filters_) {
        base::DictValue entry;
        if (filter->has_vendor_id) entry.Set("usbVendorId", static_cast<int>(filter->vendor_id));
        if (filter->has_product_id) entry.Set("usbProductId", filter->product_id);
        if (filter->bluetooth_service_class_id)
          entry.Set("bluetoothServiceClassId", filter->bluetooth_service_class_id->canonical_value());
        list.Append(std::move(entry));
      }
      return base::Value(std::move(list));
    }
    void Chosen(std::optional<std::string> id) {
      asking_.Done();
      if (!callback_)
        return;
      content::RenderFrameHost* frame = asking_.frame();
      auto it = id ? owner_->ports_.find(*id) : owner_->ports_.end();
      if (!frame || it == owner_->ports_.end()) {
        std::move(callback_).Run(nullptr);
        return;
      }
      owner_->grants_.Add(KeyOf(frame), it->first);
      std::move(callback_).Run(it->second.Clone());
    }

    raw_ptr<SerialPorts> owner_;
    Asking asking_;
    std::vector<blink::mojom::SerialPortFilterPtr> filters_;
    std::vector<device::BluetoothUUID> services_;
    content::SerialChooser::Callback callback_;
    base::WeakPtrFactory<Chooser> weak_{this};
  };

  std::unique_ptr<content::SerialChooser> RunChooser(content::RenderFrameHost* frame,
                                                     std::vector<blink::mojom::SerialPortFilterPtr> filters,
                                                     std::vector<device::BluetoothUUID> services,
                                                     content::SerialChooser::Callback callback) override {
    Connect();
    auto chooser = std::make_unique<Chooser>(this, frame, std::move(filters), std::move(services), std::move(callback));
    if (enumerated_)
      chooser->Ask();
    else
      waiting_.push_back(base::BindOnce(&Chooser::Ask, chooser->GetWeakPtr()));
    return chooser;
  }

  bool CanRequestPortPermission(content::RenderFrameHost*) override { return true; }
  bool HasPortPermission(content::RenderFrameHost* frame, const device::mojom::SerialPortInfo& port) override {
    return grants_.Has(KeyOf(frame), port.token.ToString());
  }
  void RevokePortPermissionWebInitiated(content::RenderFrameHost* frame, const base::UnguessableToken& token) override {
    const Grants::Key key = KeyOf(frame);
    if (grants_.Remove(key, token.ToString()))
      for (auto& observer : observers_)
        observer.OnPermissionRevoked(key.second);
  }
  const device::mojom::SerialPortInfo* GetPortInfo(content::RenderFrameHost*,
                                                   const base::UnguessableToken& token) override {
    auto it = ports_.find(token.ToString());
    return it == ports_.end() ? nullptr : it->second.get();
  }
  device::mojom::SerialPortManager* GetPortManager(content::RenderFrameHost*) override {
    Connect();
    return manager_.get();
  }
  void AddObserver(content::RenderFrameHost*, Observer* observer) override { observers_.AddObserver(observer); }
  void RemoveObserver(content::RenderFrameHost*, Observer* observer) override { observers_.RemoveObserver(observer); }

  // device::mojom::SerialPortManagerClient
  void OnPortAdded(device::mojom::SerialPortInfoPtr port) override {
    const device::mojom::SerialPortInfo& info = *port;
    ports_[port->token.ToString()] = std::move(port);
    for (auto& observer : observers_)
      observer.OnPortAdded(info);
    Changed();
  }
  void OnPortRemoved(device::mojom::SerialPortInfoPtr port) override {
    ports_.erase(port->token.ToString());
    for (auto& observer : observers_)
      observer.OnPortRemoved(*port);
    Changed();
  }
  void OnPortConnectedStateChanged(device::mojom::SerialPortInfoPtr port) override {
    const device::mojom::SerialPortInfo& info = *port;
    ports_[port->token.ToString()] = std::move(port);
    for (auto& observer : observers_)
      observer.OnPortConnectedStateChanged(info);
    Changed();
  }

 private:
  void Connect() {
    if (manager_)
      return;
    content::GetDeviceService().BindSerialPortManager(manager_.BindNewPipeAndPassReceiver());
    manager_.set_disconnect_handler(base::BindOnce(&SerialPorts::Lost, base::Unretained(this)));
    manager_->SetClient(client_.BindNewPipeAndPassRemote());
    manager_->GetDevices(/*allow_bluetooth_system_prompt=*/false,
                         base::BindOnce(&SerialPorts::Enumerated, base::Unretained(this)));
  }
  void Enumerated(std::vector<device::mojom::SerialPortInfoPtr> ports) {
    for (auto& port : ports)
      ports_[port->token.ToString()] = std::move(port);
    enumerated_ = true;
    auto waiting = std::move(waiting_);
    for (auto& task : waiting)
      std::move(task).Run();
  }
  void Lost() {
    manager_.reset();
    client_.reset();
    ports_.clear();
    for (auto& observer : observers_)
      observer.OnPortManagerConnectionError();
    enumerated_ = true;
    auto waiting = std::move(waiting_);
    for (auto& task : waiting)
      std::move(task).Run();
    enumerated_ = false;
  }
  void Changed() {
    for (Chooser* chooser : choosers_)
      chooser->Refresh();
  }

  base::ListValue List(const std::vector<blink::mojom::SerialPortFilterPtr>& filters,
                       const std::vector<device::BluetoothUUID>& services) const {
    base::ListValue list;
    for (const auto& [token, port] : ports_) {
      if (!port->connected)
        continue;
      // A Bluetooth port only for a service the page named.
      if (port->type == device::mojom::SerialPortType::BLUETOOTH_CLASSIC_RFCOMM &&
          (!port->bluetooth_service_class_id ||
           std::ranges::find(services, *port->bluetooth_service_class_id) == services.end())) {
        bool named = false;
        for (const auto& filter : filters)
          named |= filter->bluetooth_service_class_id && SerialMatches(*filter, *port);
        if (!named)
          continue;
      }
      if (!filters.empty()) {
        bool matched = false;
        for (const auto& filter : filters)
          matched |= SerialMatches(*filter, *port);
        if (!matched)
          continue;
      }
      base::DictValue entry;
      entry.Set("id", token);
      const std::string path = port->path.AsUTF8Unsafe();
      entry.Set("name", port->display_name && !port->display_name->empty() ? *port->display_name
                                                                             : port->path.BaseName().AsUTF8Unsafe());
      entry.Set("path", path);
      if (port->has_vendor_id) entry.Set("vendorId", port->vendor_id);
      if (port->has_product_id) entry.Set("productId", port->product_id);
      if (port->serial_number) entry.Set("serialNumber", *port->serial_number);
      list.Append(std::move(entry));
    }
    return list;
  }

  mojo::Remote<device::mojom::SerialPortManager> manager_;
  mojo::Receiver<device::mojom::SerialPortManagerClient> client_{this};
  bool enumerated_ = false;
  std::vector<base::OnceClosure> waiting_;
  std::map<std::string, device::mojom::SerialPortInfoPtr> ports_;
  std::set<Chooser*> choosers_;
  Grants grants_;
  base::ObserverList<Observer> observers_;
};

}  // namespace

content::UsbDelegate* UsbChooserDelegate() {
  static base::NoDestructor<UsbDevices> delegate;
  return delegate.get();
}

content::HidDelegate* HidChooserDelegate() {
  static base::NoDestructor<HidDevices> delegate;
  return delegate.get();
}

content::SerialDelegate* SerialChooserDelegate() {
  static base::NoDestructor<SerialPorts> delegate;
  return delegate.get();
}

}  // namespace zebkit
