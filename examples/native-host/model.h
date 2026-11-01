// Shared data for the native toolkit examples. No toolkit owns engine objects.
#ifndef ZEBKIT_EXAMPLES_NATIVE_HOST_MODEL_H
#define ZEBKIT_EXAMPLES_NATIVE_HOST_MODEL_H
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>
#include <zebkit/pdf.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <IOSurface/IOSurfaceRef.h>
#endif

namespace native_host {
struct Image {
  int width = 0, height = 0;
  float scale = 1;
  zebkit_rect bounds{};
  std::vector<uint8_t> bgra;
  void Take(const zebkit_frame& frame) {
    const uint8_t* pixels = frame.pixels;
    size_t stride = frame.stride;
#if defined(__APPLE__)
    // On macOS a frame is an IOSurface, the engine's until it is released.
    IOSurfaceRef surface = nullptr;
    if (frame.kind == ZEBKIT_FRAME_IOSURFACE && frame.iosurface) {
      surface = static_cast<IOSurfaceRef>(frame.iosurface);
      if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) != 0)
        return;
      pixels = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
      stride = IOSurfaceGetBytesPerRow(surface);
    } else
#endif
    if (frame.kind != ZEBKIT_FRAME_SHARED_MEMORY || !frame.pixels)
      return;
    width = frame.width; height = frame.height;
    scale = frame.scale; bounds = frame.bounds;
    bgra.resize(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
      auto* to = bgra.data() + static_cast<size_t>(y) * width * 4;
      std::memcpy(to, pixels + static_cast<size_t>(y) * stride, width * 4);
      if (frame.format == ZEBKIT_FORMAT_RGBA8)
        for (int x = 0; x < width; ++x) std::swap(to[x * 4], to[x * 4 + 2]);
    }
#if defined(__APPLE__)
    if (surface)
      IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
#endif
  }
};

inline std::string SamplePdf() {
  const std::string text = "BT /F1 24 Tf 50 700 Td (PDF in your application's window) Tj "
                           "0 -44 Td /F1 14 Tf (Rendered by zebkit / PDFium.) Tj ET";
  const std::vector<std::string> objects = {
    "<< /Type /Catalog /Pages 2 0 R >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    "<< /Length " + std::to_string(text.size()) + " >>\nstream\n" + text + "\nendstream"
  };
  std::ostringstream out;
  out << "%PDF-1.7\n";
  std::vector<std::streamoff> offsets{0};
  for (size_t i = 0; i < objects.size(); ++i) {
    offsets.push_back(out.tellp());
    out << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
  }
  const auto xref = out.tellp();
  out << "xref\n0 6\n0000000000 65535 f \n";
  for (size_t i = 1; i < offsets.size(); ++i)
    out << std::setw(10) << std::setfill('0') << offsets[i] << " 00000 n \n";
  out << "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" << xref << "\n%%EOF\n";
  return out.str();
}

// The PDF as one continuous scrolling document in a pane the application owns:
// every page's place is known from its size, and only pages in the pane are
// rendered, each once at the pane's width, the last few kept. The wheel moves a
// target and the position follows it over a few frames; a trackpad's precise
// deltas are followed at once. The toolkit calls Tick() every frame while
// Animating(), and draws Canvas().
struct PdfPane {
  static constexpr int kGap = 8, kMargin = 8;
  static constexpr size_t kCacheBytes = 128u << 20;  // of rendered pages
  static constexpr double kNotch = 120;              // a wheel's click, in pixels
  struct Page { double width = 612, height = 792, top = 0, pixels_high = 0; };
  struct Rendered { int page = 0, width = 0, height = 0; std::vector<uint8_t> bgra; uint64_t used = 0; };

  zebkit_pdf* pdf = nullptr;
  std::vector<Page> pages;
  std::vector<Rendered> rendered;
  Image canvas;
  std::function<void()> changed;
  int width = 0, height = 0;
  double total = 0, position = 0, target = 0;
  float scale = 1;
  uint64_t clock = 0;
  int renders = 0;
  bool dirty = true;
  bool report = std::getenv("NATIVE_HOST_PDF_REPORT") != nullptr;

  void Open(zebkit_pdf* document) {
    pdf = document;
    const int count = zebkit_pdf_page_count(pdf);
    pages.resize(count);
    for (int i = 0; i < count; ++i) {
      char* error = nullptr;
      char* json = zebkit_pdf_page(pdf, i, &error);
      if (json) {
        const auto number = [&](const char* key, double& to) {
          if (const char* at = std::strstr(json, key)) to = std::strtod(at + std::strlen(key), nullptr);
        };
        number("\"width\":", pages[i].width);
        number("\"height\":", pages[i].height);
      }
      zebkit_free(json);
      zebkit_free(error);
    }
    Layout();
  }
  void Close() { rendered.clear(); pdf = nullptr; }
  void Resize(int new_width, int new_height, float new_scale = 1) {
    if (new_width == width && new_height == height && new_scale == scale) return;
    const double place = total > 0 ? position / total : 0;
    width = new_width; height = new_height; scale = new_scale;
    rendered.clear();
    Layout();
    position = target = Clamp(place * total);
    dirty = true;
  }
  // Pixels down the document, which are the toolkit's own for a precise device.
  void Wheel(double delta, bool precise) {
    target = Clamp(target + delta * scale);
    if (precise) position = target;
    Changed();
  }
  void WheelNotches(double notches) { Wheel(notches * kNotch, false); }
  void Pages(double count) { Wheel(count * height / scale * 0.9, false); }
  bool Animating() const { return position != target; }
  void Tick() {
    const double left = target - position;
    position = std::abs(left) < 1 ? target : position + left * 0.3;
    Changed();
  }
  const Image& Canvas() {
    if (dirty) Compose();
    return canvas;
  }
  int CurrentPage() const {
    int low = 0, high = static_cast<int>(pages.size()) - 1;
    const double middle = position + height / 2.;
    while (low < high) {
      const int mid = (low + high + 1) / 2;
      if (pages[mid].top <= middle) low = mid; else high = mid - 1;
    }
    return low;
  }

 private:
  double Clamp(double value) const { return std::max(0., std::min(value, std::max(0., total - height))); }
  void Changed() { dirty = true; if (changed) changed(); }
  double PixelsPerPoint(const Page& page) const {
    return std::max(1, width - 2 * static_cast<int>(kMargin * scale)) / page.width;
  }
  void Layout() {
    total = kGap * scale;
    for (Page& page : pages) {
      page.top = total;
      page.pixels_high = std::ceil(page.height * PixelsPerPoint(page));
      total += page.pixels_high + kGap * scale;
    }
  }
  Rendered* Render(int index) {
    for (Rendered& one : rendered)
      if (one.page == index) { one.used = ++clock; return &one; }
    zebkit_pdf_render_options options{};
    options.scale = static_cast<float>(PixelsPerPoint(pages[index]));
    options.flags = ZEBKIT_PDF_RENDER_ANNOTATIONS | ZEBKIT_PDF_RENDER_FORMS;
    options.background = 0xffffffff;
    int w = 0, h = 0;
    char* error = nullptr;
    uint8_t* rgba = zebkit_pdf_render(pdf, index, &options, &w, &h, &error);
    if (!rgba) { std::fprintf(stderr, "PDF: %s\n", error ? error : "cannot render"); zebkit_free(error); return nullptr; }
    ++renders;
    Rendered one;
    one.page = index; one.width = w; one.height = h; one.used = ++clock;
    one.bgra.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
    zebkit_free(rgba);
    for (size_t i = 0; i < one.bgra.size(); i += 4) std::swap(one.bgra[i], one.bgra[i + 2]);
    rendered.push_back(std::move(one));
    return &rendered.back();
  }
  void Evict(int first, int last) {
    size_t bytes = 0;
    for (const Rendered& one : rendered) bytes += one.bgra.size();
    while (bytes > kCacheBytes) {
      auto oldest = rendered.end();
      for (auto it = rendered.begin(); it != rendered.end(); ++it)
        if ((it->page < first || it->page > last) && (oldest == rendered.end() || it->used < oldest->used)) oldest = it;
      if (oldest == rendered.end()) return;
      bytes -= oldest->bgra.size();
      rendered.erase(oldest);
    }
  }
  void Compose() {
    dirty = false;
    if (width <= 0 || height <= 0) return;
    canvas.width = width; canvas.height = height; canvas.scale = scale;
    canvas.bgra.resize(static_cast<size_t>(width) * height * 4);
    uint32_t* out = reinterpret_cast<uint32_t*>(canvas.bgra.data());
    std::fill(out, out + static_cast<size_t>(width) * height, 0xffc8c8c8u);
    const double top = position, bottom = position + height;
    int first = -1, last = -1;
    for (int i = CurrentPage(); i >= 0 && pages[i].top + pages[i].pixels_high > top; --i) first = i;
    if (first < 0) first = CurrentPage();
    for (int i = first; i < static_cast<int>(pages.size()) && pages[i].top < bottom; ++i) {
      last = i;
      Rendered* page = Render(i);
      if (!page) continue;
      const int x = (width - page->width) / 2;
      const int y = static_cast<int>(std::lround(pages[i].top - position));
      for (int row = std::max(0, -y); row < page->height && y + row < height; ++row) {
        const int from = std::max(0, -x), to = std::min(page->width, width - x);
        if (to > from)
          std::memcpy(canvas.bgra.data() + (static_cast<size_t>(y + row) * width + x + from) * 4,
                      page->bgra.data() + (static_cast<size_t>(row) * page->width + from) * 4, (to - from) * 4u);
      }
    }
    Evict(first, last);
    if (report) {
      uint32_t sum = 0;
      for (size_t i = 0; i < canvas.bgra.size(); i += 64) sum = sum * 31 + canvas.bgra[i];
      size_t bytes = 0;
      for (const Rendered& one : rendered) bytes += one.bgra.size();
      std::printf("pdf-pane: position=%.0f total=%.0f page=%d pages=%d visible=%d-%d renders=%d kept=%zu bytes=%zu checksum=%08x\n",
                  position, total, CurrentPage() + 1, static_cast<int>(pages.size()), first + 1, last + 1, renders,
                  rendered.size(), bytes, sum);
      std::fflush(stdout);
    }
  }
};

struct Model {
  zebkit_view* view = nullptr;
  zebkit_pdf* pdf = nullptr;
  Image web, popup;
  PdfPane pane;
  bool popup_shown = false;
  int frames = 0;
  std::function<void()> redraw;

  void Open(const std::string& url, const std::string& pdf_path) {
    char* error = nullptr;
    if (pdf_path.empty()) {
      const auto sample = SamplePdf();
      pdf = zebkit_pdf_open(sample.data(), sample.size(), nullptr, &error);
    } else {
      pdf = zebkit_pdf_open_file(pdf_path.c_str(), nullptr, &error);
    }
    if (!pdf) {
      std::string message = error ? error : "Cannot open PDF";
      zebkit_free(error);
      throw std::runtime_error(message);
    }
    pane.Open(pdf);
    pane.changed = [this] { if (redraw) redraw(); };
    view = zebkit_view_new_offscreen(nullptr, 600, 600, 1);
    if (!view) throw std::runtime_error("Cannot create the off-screen web view");
    zebkit_view_on_close(view, [](zebkit_view*, void* data) {
      auto& self = *static_cast<Model*>(data);
      self.view = nullptr;
      if (self.redraw) self.redraw();
    }, this);
    zebkit_view_set_frame_buffers(view, ZEBKIT_BUFFERS_SHARED_MEMORY);
    zebkit_view_on_frame(view, [](zebkit_view*, const zebkit_frame* frame, void* data) {
      auto& self = *static_cast<Model*>(data);
      (frame->layer == ZEBKIT_LAYER_POPUP ? self.popup : self.web).Take(*frame);
      ++self.frames;
      zebkit_frame_release(frame);
      if (self.redraw) self.redraw();
    }, this);
    zebkit_view_on_popup(view, [](zebkit_view*, bool shown, const zebkit_rect*, void* data) {
      auto& self = *static_cast<Model*>(data);
      self.popup_shown = shown;
      if (!shown) self.popup = Image();
      if (self.redraw) self.redraw();
    }, this);
    zebkit_view_load_url(view, url.empty() ?
      "data:text/html,<!doctype html><meta charset=utf-8><style>body{font:20px sans-serif;padding:24px}input,button,select{font:inherit;margin:8px}</style><h1>Your native window</h1><p>This page is a zebkit off-screen view.</p><input placeholder='Type here'><button onclick=\"this.textContent='Clicked'\">Click me</button><select><option>First</option><option>Second</option></select><p>Resize the window, scroll, and use your input method.</p>" : url.c_str());
  }
  void Close() {
    if (view) { zebkit_view_close(view); view = nullptr; }
    pane.Close();
    if (pdf) { zebkit_pdf_close(pdf); pdf = nullptr; }
  }
};
}  // namespace native_host
#endif
