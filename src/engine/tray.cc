#include "zebkit/engine/tray.h"

#include <string.h>

#include <optional>
#include <utility>

#include "base/containers/span.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/browser_thread.h"
#include "ui/gfx/codec/png_codec.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace zebkit {

Tray::Tray(SkBitmap image, std::u16string tooltip) : image_(std::move(image)), tooltip_(std::move(tooltip)) {
  platform_ = CreateTrayPlatform(this);
}

Tray::~Tray() = default;

void Tray::SetImage(SkBitmap image) {
  image_ = std::move(image);
  if (platform_)
    platform_->SetImage(image_);
}

void Tray::SetToolTip(std::u16string tooltip) {
  tooltip_ = std::move(tooltip);
  if (platform_)
    platform_->SetToolTip(tooltip_);
}

void Tray::SetMenu(std::vector<MenuItem> menu, base::RepeatingCallback<void(const std::string&)> chosen) {
  menu_ = std::move(menu);
  chosen_ = std::move(chosen);
  if (platform_)
    platform_->MenuChanged();
}

void Tray::Clicked() {
  if (click_)
    click_.Run();
}

void Tray::Chosen(const std::string& id) {
  if (chosen_ && !id.empty())
    chosen_.Run(id);
}

}  // namespace zebkit

struct zebkit_tray {
  std::unique_ptr<zebkit::Tray> tray;
};

namespace {

bool OnEngineThread() {
  return content::BrowserThread::CurrentlyOn(content::BrowserThread::UI);
}

std::optional<SkBitmap> Decode(const uint8_t* png, size_t size) {
  if (!png || !size || size > 16 * 1024 * 1024)
    return std::nullopt;
  // SAFETY: the C API's @size bytes at @png.
  SkBitmap bitmap = gfx::PNGCodec::Decode(UNSAFE_BUFFERS(base::span(png, size)));
  if (bitmap.isNull() || bitmap.width() > 1024 || bitmap.height() > 1024)
    return std::nullopt;
  return bitmap;
}

bool Fail(char** error, const char* message) {
  if (error)
    *error = strdup(message);
  return false;
}

}  // namespace

extern "C" {

zebkit_tray* zebkit_tray_new(const uint8_t* png, size_t size, const char* tooltip, char** error) {
  if (error)
    *error = nullptr;
  if (!OnEngineThread()) {
    Fail(error, "the engine's thread");
    return nullptr;
  }
  std::optional<SkBitmap> image = Decode(png, size);
  if (!image) {
    Fail(error, "the icon: a PNG of at most 1024 by 1024");
    return nullptr;
  }
  auto* tray = new zebkit_tray;
  tray->tray = std::make_unique<zebkit::Tray>(std::move(*image), base::UTF8ToUTF16(tooltip ? tooltip : ""));
  return tray;
}

bool zebkit_tray_set_icon(zebkit_tray* tray, const uint8_t* png, size_t size, char** error) {
  if (error)
    *error = nullptr;
  if (!OnEngineThread() || !tray)
    return Fail(error, "a tray on the engine's thread");
  std::optional<SkBitmap> image = Decode(png, size);
  if (!image)
    return Fail(error, "the icon: a PNG of at most 1024 by 1024");
  tray->tray->SetImage(std::move(*image));
  return true;
}

void zebkit_tray_set_tooltip(zebkit_tray* tray, const char* tooltip) {
  if (OnEngineThread() && tray)
    tray->tray->SetToolTip(base::UTF8ToUTF16(tooltip ? tooltip : ""));
}

bool zebkit_tray_set_menu(zebkit_tray* tray, const char* json, zebkit_menu_func func, void* data, char** error) {
  if (error)
    *error = nullptr;
  if (!OnEngineThread() || !tray)
    return Fail(error, "a tray on the engine's thread");
  std::string problem;
  auto items = zebkit::ParseMenuItems(json ? json : "[]", &problem);
  if (!items)
    return Fail(error, problem.c_str());
  tray->tray->SetMenu(std::move(*items), base::BindRepeating(
                                             [](zebkit_menu_func func, void* data, const std::string& id) {
                                               if (func)
                                                 func(id.c_str(), data);
                                             },
                                             func, data));
  return true;
}

void zebkit_tray_on_click(zebkit_tray* tray, zebkit_app_func func, void* data) {
  if (!OnEngineThread() || !tray)
    return;
  tray->tray->SetClick(func ? base::BindRepeating([](zebkit_app_func func, void* data) { func(data); }, func, data)
                            : base::RepeatingClosure());
}

void zebkit_tray_close(zebkit_tray* tray) {
  if (OnEngineThread())
    delete tray;
}

}  // extern "C"
