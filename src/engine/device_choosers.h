// The page's device choosers (docs/SYSTEM_INTEGRATION.md, 10): WebUSB, WebHID
// and Web Serial, each a content delegate whose chooser is the application's
// (zebkit_view_on_device_chooser, page_hooks.cc). A device chosen is the
// origin's (its top frame's) for the engine's run, until the page forgets it;
// without a function the page gets none, as before. Web Bluetooth is not
// here: its scanning and pairing prompts are a chooser of their own.

#ifndef ZEBKIT_ENGINE_DEVICE_CHOOSERS_H_
#define ZEBKIT_ENGINE_DEVICE_CHOOSERS_H_

namespace content {
class HidDelegate;
class SerialDelegate;
class UsbDelegate;
}  // namespace content

namespace zebkit {

content::UsbDelegate* UsbChooserDelegate();
content::HidDelegate* HidChooserDelegate();
content::SerialDelegate* SerialChooserDelegate();

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_DEVICE_CHOOSERS_H_
