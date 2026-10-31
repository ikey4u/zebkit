// Shared data for the native toolkit examples. No toolkit owns engine objects.
#ifndef ZEBKIT_EXAMPLES_NATIVE_HOST_MODEL_H
#define ZEBKIT_EXAMPLES_NATIVE_HOST_MODEL_H
#include <zebkit/zebkit.h>
#include <zebkit/offscreen.h>
#include <zebkit/pdf.h>
#include <algorithm>
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

struct Model {
  zebkit_view* view = nullptr;
  zebkit_pdf* pdf = nullptr;
  Image web, popup, document;
  bool popup_shown = false;
  int page = 0, frames = 0;
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
    RenderPdf();
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
  void RenderPdf() {
    char* error = nullptr;
    zebkit_pdf_render_options options{};
    options.scale = 1.5f;
    options.flags = ZEBKIT_PDF_RENDER_ANNOTATIONS | ZEBKIT_PDF_RENDER_FORMS;
    options.background = 0xffffffff;
    int width = 0, height = 0;
    uint8_t* rgba = zebkit_pdf_render(pdf, page, &options, &width, &height, &error);
    if (!rgba) {
      std::string message = error ? error : "Cannot render PDF";
      zebkit_free(error);
      throw std::runtime_error(message);
    }
    document.width = width; document.height = height;
    document.bgra.assign(rgba, rgba + static_cast<size_t>(width) * height * 4);
    zebkit_free(rgba);
    for (size_t i = 0; i < document.bgra.size(); i += 4)
      std::swap(document.bgra[i], document.bgra[i + 2]);
  }
  void Turn(int delta) {
    const int next = std::clamp(page + delta, 0, zebkit_pdf_page_count(pdf) - 1);
    if (next != page) {
      const int previous = page; page = next;
      try { RenderPdf(); if (redraw) redraw(); }
      catch (const std::exception& error) { page = previous; std::fprintf(stderr, "PDF: %s\n", error.what()); }
    }
  }
  void Close() {
    if (view) { zebkit_view_close(view); view = nullptr; }
    if (pdf) { zebkit_pdf_close(pdf); pdf = nullptr; }
  }
};
}  // namespace native_host
#endif
