// Off-screen views (<zebkit/offscreen.h>): a page drawn without a window,
// its frames handed to the application as shared surfaces, its input sent
// by the application.
//
// Frames come from viz's FrameSinkVideoCapturer on the view's frame sink,
// what Chrome's tab capture and Electron's off-screen rendering use: the
// GPU process copies each frame the page draws into a buffer of a pool
// (shared memory it reads back into, or a dmabuf), off the browser's main
// thread, and the browser gets the buffer's handle. Nothing is encoded. On
// macOS the frame read back is copied into an IOSurface of the engine's,
// only the part that changed since that surface last held a frame. The
// capturer works on damage: a page that does not change draws nothing and
// sends nothing.
//
// The view is a content shell window like any other, without a display
// (--headless): Linux's headless Ozone platform, or on macOS a view in no
// window. The headless screen's display has the views' scale.
//
// Input goes to the page's widgets as the window's events would
// (RenderWidgetHost::Forward*), an input method's composition through the
// view's DevTools session (Input.imeSetComposition, as Chrome's own
// remote input does). The cursor and popup widgets come from the hooks
// patches/1021 adds to content.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C structures and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/flat_map.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/read_only_shared_memory_region.h"
#include "base/memory/ref_counted.h"
#include "base/memory/shared_memory_mapping.h"
#include "base/memory/unsafe_shared_memory_region.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "build/build_config.h"
#include "components/input/native_web_keyboard_event.h"
#include "components/viz/common/surfaces/video_capture_target.h"
#include "components/viz/host/client_frame_sink_video_capturer.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/embedder_widget_hooks.h"
#include "content/public/browser/gpu_data_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_observer.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/shell/browser/shell.h"
#include "media/base/video_frame_metadata.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "ui/base/cursor/cursor.h"
#include "ui/display/display.h"
#include "ui/display/headless/headless_screen_manager.h"
#include "ui/display/headless/headless_screen_util.h"
#include "ui/display/screen.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "ui/events/keycodes/keyboard_code_conversion.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/gpu_memory_buffer_handle.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/offscreen.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/webauthn.h"

#if BUILDFLAG(IS_MAC)
#include <IOSurface/IOSurfaceRef.h>

#include <deque>

#include "components/viz/common/resources/shared_image_format.h"
#include "ui/gfx/mac/io_surface.h"

#include "zebkit/engine/offscreen_mac.h"
#else
#include <sys/stat.h>
#endif

namespace zebkit {

namespace {

class Offscreen;
class FrameStream;

int64_t Microseconds(base::TimeTicks ticks) {
  return (ticks - base::TimeTicks()).InMicroseconds();
}

zebkit_rect ToRect(const gfx::Rect& rect) {
  return {rect.x(), rect.y(), rect.width(), rect.height()};
}

// A buffer of the capturer's pool, mapped once in this process and kept
// while frames in it are out: its handle, its mapping, and the id the
// application knows it by.
class Buffer : public base::RefCountedThreadSafe<Buffer> {
 public:
  uint64_t id = 0;
  // Shared memory: the region (its file descriptor) and its mapping.
  base::ReadOnlySharedMemoryRegion read_only;
  base::ReadOnlySharedMemoryMapping read_only_mapping;
  base::UnsafeSharedMemoryRegion writable;
  base::WritableSharedMemoryMapping writable_mapping;
  int fd = -1;
  // A GPU buffer: an IOSurface, or a dmabuf's planes.
  gfx::GpuMemoryBufferHandle gpu;

  const uint8_t* memory() const {
    if (read_only_mapping.IsValid())
      return static_cast<const uint8_t*>(read_only_mapping.memory());
    if (writable_mapping.IsValid())
      return static_cast<const uint8_t*>(writable_mapping.memory());
    return nullptr;
  }
  size_t size() const {
    if (read_only.IsValid())
      return read_only.GetSize();
    if (writable.IsValid())
      return writable.GetSize();
    return 0;
  }

 private:
  friend class base::RefCountedThreadSafe<Buffer>;
  ~Buffer() = default;
};

uint64_t NextBufferId() {
  static uint64_t next = 0;
  return ++next;
}

// A frame the application holds: zebkit_frame first, so that the pointer
// it is given is this one.
struct Frame {
  zebkit_frame frame = {};
  std::vector<zebkit_rect> damage;
  gfx::Rect damage_union;
  scoped_refptr<Buffer> buffer;
  mojo::Remote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks> callbacks;
  // Whose frame it is; gone when the view or the popup is.
  base::WeakPtr<FrameStream> stream;
  // The capturer has its buffer back.
  bool done = false;
#if BUILDFLAG(IS_MAC)
  // The IOSurface of the stream's pool the frame was copied into.
  gfx::ScopedIOSurface surface;
#endif
};

#if BUILDFLAG(IS_MAC)
// An IOSurface of a stream's pool (FrameStream::CopyToSurface()).
struct Surface {
  gfx::ScopedIOSurface ref;
  uint64_t id = 0;
  gfx::Size size;
  bool in_use = false;
  // The frame it holds, 0 for none.
  uint64_t sequence = 0;
};
#endif

// The frames of one widget, the view's page or a popup: a capturer on its
// frame sink, and what the application holds of them.
class FrameStream : public viz::mojom::FrameSinkVideoConsumer {
 public:
  FrameStream(Offscreen* owner, zebkit_frame_layer layer)
      : owner_(owner), layer_(layer) {}
  ~FrameStream() override {
    if (capturer_)
      capturer_->StopAndResetConsumer();
    // Frames the application holds outlive the stream; a held one is let go.
    pending_.reset();
  }

  // Captures @view's frame sink, at @pixel_size pixels.
  void Start(content::RenderWidgetHostView* view,
             const gfx::Size& pixel_size,
             zebkit_frame_buffers buffers,
             double fps);
  void Stop();
  bool started() const { return !!capturer_; }

  // The target moved (a new widget for a new page) or changed size.
  void Retarget(content::RenderWidgetHostView* view);
  void SetPixelSize(const gfx::Size& pixel_size);
  void SetFrameRate(double fps);
  void SetMaxInFlight(int count) {
    max_in_flight_ = std::clamp(count, 1, 8);
    MaybeDeliverPending();
  }
  void RequestFrame() {
    force_next_ = true;
    if (capturer_)
      capturer_->RequestRefreshFrame();
  }

  // The application gave @frame back.
  void Release(Frame* frame);

  void set_bounds(const gfx::Rect& bounds_css) { bounds_css_ = bounds_css; }

  base::WeakPtr<FrameStream> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }

 private:
  // viz::mojom::FrameSinkVideoConsumer
  void OnFrameCaptured(
      media::mojom::VideoBufferHandlePtr data,
      media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks) override;
  void OnNewCaptureVersion(
      const media::CaptureVersion& capture_version) override {}
  void OnFrameWithEmptyRegionCapture() override {}
  void OnStopped() override;
  void OnLog(const std::string& message) override {
    // What the capturer says, for --enable-logging --v=1.
    VLOG(1) << "zebkit: off-screen frames: " << message;
  }

  scoped_refptr<Buffer> BufferFor(media::mojom::VideoBufferHandlePtr data);
  void Deliver(std::unique_ptr<Frame> frame);
#if BUILDFLAG(IS_MAC)
  // macOS: a frame read back into shared memory, copied into an IOSurface
  // of the stream's own (the part that changed since that surface last
  // held a frame), which the application gets in its place.
  void CopyToSurface(Frame* frame);
  bool copy_to_surface_ = false;
  std::vector<Surface> surfaces_;
  // The damage of the frames delivered lately, by sequence.
  std::deque<std::pair<uint64_t, gfx::Rect>> history_;
#endif
  void MaybeDeliverPending();

  raw_ptr<Offscreen> owner_;
  const zebkit_frame_layer layer_;
  std::unique_ptr<viz::ClientFrameSinkVideoCapturer> capturer_;
  viz::mojom::BufferFormatPreference preference_ =
      viz::mojom::BufferFormatPreference::kDefault;
  gfx::Size pixel_size_;
  gfx::Rect bounds_css_;
  uint64_t sequence_ = 0;
  int in_flight_ = 0;
  int max_in_flight_ = 2;
  bool force_next_ = true;
  bool warned_unusable_ = false;
  gfx::Size last_size_;
  // The newest frame, waiting while the application holds as many as it
  // may; the damage of those it replaced is in it.
  std::unique_ptr<Frame> pending_;
  // Buffers by their shared memory's GUID or their IOSurface's or dmabuf's
  // identity: mapped once.
  std::map<std::string, scoped_refptr<Buffer>> buffers_;
  base::WeakPtrFactory<FrameStream> weak_factory_{this};
};

// What an off-screen view has besides its zebkit_view.
class Offscreen : public content::WebContentsObserver,
                  public content::RenderWidgetHostObserver {
 public:
  Offscreen(zebkit_view* view, const gfx::Size& size, float scale);
  ~Offscreen() override;

  static Offscreen* Of(zebkit_view* view);
  static Offscreen* OfContents(content::WebContents* contents);
  static std::map<zebkit_view*, Offscreen*>& All();

  zebkit_view* view() const { return view_; }
  float scale() const;
  const gfx::Size& size() const { return size_; }

  void SetSize(const gfx::Size& size, float scale);
  void SetOnFrame(zebkit_frame_func func, void* data);
  void SetFrameRate(double fps);
  void SetMaxInFlight(int count);
  void SetBuffers(zebkit_frame_buffers buffers);
  void RequestFrame();
  void SetFocus(bool focused);

  void SendMouse(zebkit_mouse_type type,
                 float x,
                 float y,
                 zebkit_mouse_button button,
                 int click_count,
                 unsigned modifiers);
  void SendWheel(float x,
                 float y,
                 float dx,
                 float dy,
                 bool precise,
                 zebkit_wheel_phase phase,
                 unsigned modifiers);
  void SendKey(const zebkit_key_event& event);

  // A frame of @layer, to the application.
  void DeliverFrame(const zebkit_frame* frame) {
    if (on_frame_)
      on_frame_(view_, frame, on_frame_data_);
  }

  void PopupShown(content::RenderWidgetHost* popup, const gfx::Rect& screen);
  void CursorChanged(const ui::Cursor& cursor);
  bool Owns(content::RenderWidgetHost* widget);

  zebkit_frame_buffers buffers() const { return buffers_; }
  double fps() const { return fps_; }
  int max_in_flight() const { return max_in_flight_; }

  void SetOnCaret(zebkit_caret_func func, void* data);

  zebkit_cursor_func on_cursor_ = nullptr;
  void* on_cursor_data_ = nullptr;
  zebkit_popup_func on_popup_ = nullptr;
  void* on_popup_data_ = nullptr;

 private:
  // content::WebContentsObserver
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void RenderViewReady() override;
  void PrimaryMainFrameRenderProcessGone(
      base::TerminationStatus status) override;
  void WebContentsDestroyed() override;

  // content::RenderWidgetHostObserver
  void RenderWidgetHostDidUpdateVisualProperties(
      content::RenderWidgetHost* widget) override;
  void RenderWidgetHostDestroyed(content::RenderWidgetHost* widget) override;

  content::RenderWidgetHostView* MainView();
  content::RenderWidgetHost* MainWidget();
  // The widget the view's main frame draws in changed: frames, focus and
  // the caret follow it.
  void AttachMainWidget();
  void ClosePopup();
  void CaretChanged();
  void PlaceWindow();
  gfx::Size PixelSize(content::RenderWidgetHostView* view) const;
  // (@x, @y) in the view to the popup's coordinates when it is over the
  // popup.
  bool InPopup(float x, float y, gfx::PointF* local) const;
  content::RenderWidgetHost* KeyTarget();

  const raw_ptr<zebkit_view> view_;
  gfx::Size size_;
  bool focused_ = false;
  zebkit_frame_func on_frame_ = nullptr;
  void* on_frame_data_ = nullptr;
  zebkit_frame_buffers buffers_ = ZEBKIT_BUFFERS_AUTO;
  double fps_ = 0;
  int max_in_flight_ = 2;
  FrameStream main_{this, ZEBKIT_LAYER_VIEW};
  raw_ptr<content::RenderWidgetHost> main_widget_ = nullptr;
  // The popup widget shown, its place in the view in CSS pixels, and its
  // frames.
  raw_ptr<content::RenderWidgetHost> popup_ = nullptr;
  gfx::Rect popup_bounds_;
  std::unique_ptr<FrameStream> popup_frames_;
  zebkit_caret_func on_caret_ = nullptr;
  void* on_caret_data_ = nullptr;
  base::CallbackListSubscription caret_subscription_;
  void SubscribeCaret();
  std::optional<gfx::Rect> last_caret_;
  // A mouse button held, so that a drag that leaves the popup stays its.
  bool mouse_in_popup_ = false;
  // The primary display: the size of the largest view, their scale.
  void UpdateDisplay();
  base::WeakPtrFactory<Offscreen> weak_factory_{this};
};

// ---- FrameStream ---------------------------------------------------------

void FrameStream::Start(content::RenderWidgetHostView* view,
                        const gfx::Size& pixel_size,
                        zebkit_frame_buffers buffers,
                        double fps) {
  Stop();
  if (!view)
    return;
  capturer_ = view->CreateVideoCapturer();
  if (!capturer_)
    return;
  pixel_size_ = pixel_size;
  capturer_->SetFormat(media::PIXEL_FORMAT_ARGB);
  capturer_->SetAutoThrottlingEnabled(false);
  capturer_->SetAnimationFpsLockIn(false, 1.0f);
  capturer_->SetMinSizeChangePeriod(base::TimeDelta());
  if (!pixel_size_.IsEmpty())
    capturer_->SetResolutionConstraints(pixel_size_, pixel_size_, false);
  SetFrameRate(fps);
  // IOSurfaces on macOS, which the GPU draws into; on Linux shared memory
  // by default, which every system has: a dmabuf needs a GPU that makes
  // them, and a host that imports them.
  bool gpu = buffers == ZEBKIT_BUFFERS_GPU;
  // Without a GPU (SwiftShader, software compositing) there are no GPU
  // buffers to ask for: viz would stop on the request.
  if (!content::GpuDataManager::GetInstance()->HardwareAccelerationEnabled())
    gpu = false;
#if BUILDFLAG(IS_MAC)
  // IOSurfaces by default: the GPU's read back, copied into surfaces of
  // the engine's. viz's own IOSurfaces (ZEBKIT_BUFFERS_GPU) come only for
  // a page in a window on the screen for now.
  copy_to_surface_ = !gpu && buffers != ZEBKIT_BUFFERS_SHARED_MEMORY;
  surfaces_.clear();
  history_.clear();
#endif
  preference_ = gpu
                    ? viz::mojom::BufferFormatPreference::kPreferMappableSharedImage
                    : viz::mojom::BufferFormatPreference::kDefault;
  force_next_ = true;
  capturer_->Start(this, preference_);
}

void FrameStream::Stop() {
  if (!capturer_)
    return;
  capturer_->StopAndResetConsumer();
  capturer_.reset();
  pending_.reset();
  buffers_.clear();
}

void FrameStream::Retarget(content::RenderWidgetHostView* view) {
  if (!capturer_ || !view)
    return;
  capturer_->ChangeTarget(viz::VideoCaptureTarget(
      view->GetRenderWidgetHost()->GetFrameSinkId()));
  force_next_ = true;
}

void FrameStream::SetPixelSize(const gfx::Size& pixel_size) {
  if (pixel_size == pixel_size_ || pixel_size.IsEmpty())
    return;
  pixel_size_ = pixel_size;
  if (capturer_)
    capturer_->SetResolutionConstraints(pixel_size_, pixel_size_, false);
}

void FrameStream::SetFrameRate(double fps) {
  if (!capturer_)
    return;
  // No limit is the display's: 120 a second at most.
  const double rate = fps > 0 ? std::min(fps, 120.0) : 120.0;
  capturer_->SetMinCapturePeriod(base::Seconds(1.0 / rate));
}

void FrameStream::OnStopped() {
  // GPU buffers that could not be had (no GPU, or one without them): shared
  // memory then, which always works.
  if (capturer_ && preference_ != viz::mojom::BufferFormatPreference::kDefault) {
    LOG(WARNING) << "zebkit: no GPU buffers for off-screen frames; "
                    "shared memory instead";
    preference_ = viz::mojom::BufferFormatPreference::kDefault;
    capturer_->StopAndResetConsumer();
    force_next_ = true;
    capturer_->Start(this, preference_);
  }
}

scoped_refptr<Buffer> FrameStream::BufferFor(
    media::mojom::VideoBufferHandlePtr data) {
  std::string key;
  auto buffer = base::MakeRefCounted<Buffer>();
  if (data->is_read_only_shmem_region()) {
    base::ReadOnlySharedMemoryRegion& region =
        data->get_read_only_shmem_region();
    if (!region.IsValid())
      return nullptr;
    key = "shm:" + region.GetGUID().ToString();
    if (auto it = buffers_.find(key); it != buffers_.end())
      return it->second;
    buffer->read_only = std::move(region);
    buffer->read_only_mapping = buffer->read_only.Map();
    if (!buffer->read_only_mapping.IsValid())
      return nullptr;
#if !BUILDFLAG(IS_MAC)
    buffer->fd = buffer->read_only.GetPlatformHandle().fd;
#endif
  } else if (data->is_unsafe_shmem_region()) {
    base::UnsafeSharedMemoryRegion& region = data->get_unsafe_shmem_region();
    if (!region.IsValid())
      return nullptr;
    key = "shm:" + region.GetGUID().ToString();
    if (auto it = buffers_.find(key); it != buffers_.end())
      return it->second;
    buffer->writable = std::move(region);
    buffer->writable_mapping = buffer->writable.Map();
    if (!buffer->writable_mapping.IsValid())
      return nullptr;
#if !BUILDFLAG(IS_MAC)
    buffer->fd = buffer->writable.GetPlatformHandle().fd;
#endif
  } else if (data->is_gpu_memory_buffer_handle()) {
    gfx::GpuMemoryBufferHandle& handle = data->get_gpu_memory_buffer_handle();
    switch (handle.type) {
      case gfx::SHARED_MEMORY_BUFFER: {
        base::UnsafeSharedMemoryRegion region = std::move(handle).region();
        if (!region.IsValid())
          return nullptr;
        key = "shm:" + region.GetGUID().ToString();
        if (auto it = buffers_.find(key); it != buffers_.end())
          return it->second;
        buffer->writable = std::move(region);
        buffer->writable_mapping = buffer->writable.Map();
        if (!buffer->writable_mapping.IsValid())
          return nullptr;
#if !BUILDFLAG(IS_MAC)
        buffer->fd = buffer->writable.GetPlatformHandle().fd;
#endif
        break;
      }
#if BUILDFLAG(IS_MAC)
      case gfx::IO_SURFACE_BUFFER: {
        IOSurfaceRef surface = handle.io_surface().get();
        if (!surface)
          return nullptr;
        key = "iosurface:" + std::to_string(IOSurfaceGetID(surface));
        if (auto it = buffers_.find(key); it != buffers_.end())
          return it->second;
        buffer->gpu = std::move(handle);
        break;
      }
#elif BUILDFLAG(IS_OZONE)
      case gfx::NATIVE_PIXMAP: {
        const gfx::NativePixmapHandle& pixmap = handle.native_pixmap_handle();
        if (pixmap.planes.empty() || !pixmap.planes[0].fd.is_valid())
          return nullptr;
        struct stat info;
        if (fstat(pixmap.planes[0].fd.get(), &info) != 0)
          return nullptr;
        key = "dmabuf:" + std::to_string(info.st_dev) + ":" +
              std::to_string(info.st_ino);
        if (auto it = buffers_.find(key); it != buffers_.end())
          return it->second;
        buffer->gpu = std::move(handle);
        break;
      }
#endif
      default:
        return nullptr;
    }
  } else {
    return nullptr;
  }
  buffer->id = NextBufferId();
  // A pool is recreated when the size changes: buffers of an old one are
  // not seen again. Keep a pool's worth.
  if (buffers_.size() >= 16)
    buffers_.clear();
  buffers_[key] = buffer;
  return buffer;
}

void FrameStream::OnFrameCaptured(
    media::mojom::VideoBufferHandlePtr data,
    media::mojom::VideoFrameInfoPtr info,
    const gfx::Rect& content_rect,
    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        callbacks) {
  auto frame = std::make_unique<Frame>();
  frame->callbacks.Bind(std::move(callbacks));
  frame->stream = weak_factory_.GetWeakPtr();
  const int handle_kind = static_cast<int>(data->which());
  scoped_refptr<Buffer> buffer = BufferFor(std::move(data));
  if (!buffer) {
    LOG_IF(WARNING, !warned_unusable_)
        << "zebkit: an off-screen frame in a buffer of kind " << handle_kind
        << " that cannot be used; frames are dropped";
    warned_unusable_ = true;
    frame->callbacks->Done();
    return;
  }
  frame->buffer = buffer;

  const gfx::Rect visible = info->visible_rect;
  zebkit_frame& out = frame->frame;
  out.layer = layer_;
  out.width = visible.width();
  out.height = visible.height();
  out.scale = owner_->scale();
  out.bounds = ToRect(bounds_css_);
  out.buffer_id = buffer->id;
  out.format = ZEBKIT_FORMAT_BGRA8;
  out.fd = -1;
  out.drawn_us = info->metadata.reference_time
                     ? Microseconds(*info->metadata.reference_time)
                     : 0;
  out.ready_us = Microseconds(base::TimeTicks::Now());

  // What changed, in the frame's pixels; all of it the first time and when
  // the size changed.
  gfx::Rect damage(visible.size());
  if (!force_next_ && last_size_ == visible.size() &&
      info->metadata.capture_update_rect) {
    damage = *info->metadata.capture_update_rect;
    damage.Offset(-visible.x(), -visible.y());
    damage.Intersect(gfx::Rect(visible.size()));
  }
  // A frame again with nothing changed: the capturer's refresh. An idle page
  // sends nothing.
  if (damage.IsEmpty() && !force_next_) {
    frame->callbacks->Done();
    return;
  }
  force_next_ = false;
  last_size_ = visible.size();
  frame->damage_union = damage;

  const size_t stride =
      info->strides ? info->strides->stride_by_plane[0]
                    : static_cast<size_t>(info->coded_size.width()) * 4;
  if (const uint8_t* memory = buffer->memory()) {
    size_t base_offset = 0;
    if (buffer->gpu.type == gfx::SHARED_MEMORY_BUFFER)
      base_offset = buffer->gpu.offset;
    out.kind = ZEBKIT_FRAME_SHARED_MEMORY;
    out.stride = buffer->gpu.type == gfx::SHARED_MEMORY_BUFFER &&
                         buffer->gpu.stride
                     ? buffer->gpu.stride
                     : stride;
    out.offset = base_offset + visible.y() * out.stride + visible.x() * 4;
    out.size = buffer->size();
    out.pixels = memory + out.offset;
    out.fd = buffer->fd;
  }
#if BUILDFLAG(IS_MAC)
  else if (buffer->gpu.type == gfx::IO_SURFACE_BUFFER) {
    out.kind = ZEBKIT_FRAME_IOSURFACE;
    out.iosurface = buffer->gpu.io_surface().get();
  }
#elif BUILDFLAG(IS_OZONE)
  else if (buffer->gpu.type == gfx::NATIVE_PIXMAP) {
    const gfx::NativePixmapHandle& pixmap = buffer->gpu.native_pixmap_handle();
    out.kind = ZEBKIT_FRAME_DMABUF;
    out.plane_count = static_cast<int>(std::min<size_t>(pixmap.planes.size(), 4));
    for (int i = 0; i < out.plane_count; ++i) {
      out.fds[i] = pixmap.planes[i].fd.get();
      out.strides[i] = pixmap.planes[i].stride;
      out.offsets[i] = static_cast<uint32_t>(pixmap.planes[i].offset);
    }
    out.modifier = pixmap.modifier;
    out.fourcc = 0x34325241;  // DRM_FORMAT_ARGB8888: B, G, R, A in memory
  }
#endif

  if (in_flight_ >= max_in_flight_) {
    // The application holds as many as it may: this one waits, in place of
    // the one that waited, with its damage too.
    if (pending_) {
      if (pending_->frame.width == out.width &&
          pending_->frame.height == out.height) {
        frame->damage_union.Union(pending_->damage_union);
      } else {
        frame->damage_union = gfx::Rect(visible.size());
      }
      pending_->callbacks->Done();
    }
    pending_ = std::move(frame);
    return;
  }
  Deliver(std::move(frame));
}

void FrameStream::Deliver(std::unique_ptr<Frame> frame) {
  frame->damage = {ToRect(frame->damage_union)};
  frame->frame.damage = frame->damage.data();
  frame->frame.damage_count = frame->damage.size();
  frame->frame.sequence = ++sequence_;
#if BUILDFLAG(IS_MAC)
  if (copy_to_surface_ && frame->frame.kind == ZEBKIT_FRAME_SHARED_MEMORY)
    CopyToSurface(frame.get());
#endif
  ++in_flight_;
  // The application's until it releases it.
  Frame* given = frame.release();
  owner_->DeliverFrame(&given->frame);
}

void FrameStream::Release(Frame* frame) {
  CHECK_GT(in_flight_, 0);
  --in_flight_;
  if (!frame->done)
    frame->callbacks->Done();
#if BUILDFLAG(IS_MAC)
  for (Surface& surface : surfaces_) {
    if (frame->surface && surface.ref.get() == frame->surface.get())
      surface.in_use = false;
  }
#endif
  delete frame;
  MaybeDeliverPending();
}

#if BUILDFLAG(IS_MAC)
void FrameStream::CopyToSurface(Frame* frame) {
  zebkit_frame& out = frame->frame;
  const gfx::Size size(out.width, out.height);
  const uint64_t sequence = out.sequence;
  history_.emplace_back(sequence, frame->damage_union);
  while (history_.size() > 32)
    history_.pop_front();

  // A surface of the size that nobody holds, the one with the newest
  // picture; otherwise a new one. As many as the frames out, and one.
  Surface* surface = nullptr;
  for (Surface& candidate : surfaces_) {
    if (!candidate.in_use && candidate.size == size &&
        (!surface || candidate.sequence > surface->sequence)) {
      surface = &candidate;
    }
  }
  if (!surface) {
    std::erase_if(surfaces_, [&](const Surface& old) {
      return !old.in_use && old.size != size;
    });
    Surface fresh;
    fresh.ref = gfx::CreateIOSurface(size, viz::SinglePlaneFormat::kBGRA_8888,
                                     /*should_clear=*/false);
    if (!fresh.ref)
      return;  // Shared memory, then.
    fresh.id = NextBufferId();
    fresh.size = size;
    surfaces_.push_back(std::move(fresh));
    surface = &surfaces_.back();
  }

  // What changed since the surface last held a frame: all of it when that
  // is not known.
  gfx::Rect region;
  if (surface->sequence == 0 || history_.front().first > surface->sequence + 1) {
    region = gfx::Rect(size);
  } else {
    for (const auto& [seen, damage] : history_) {
      if (seen > surface->sequence)
        region.Union(damage);
    }
  }
  region.Intersect(gfx::Rect(size));

  IOSurfaceRef ref = surface->ref.get();
  IOSurfaceLock(ref, 0, nullptr);
  uint8_t* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(ref));
  const size_t row_bytes = IOSurfaceGetBytesPerRow(ref);
  for (int y = region.y(); y < region.bottom(); ++y) {
    memcpy(base + y * row_bytes + region.x() * 4,
           out.pixels + y * out.stride + region.x() * 4, region.width() * 4);
  }
  IOSurfaceUnlock(ref, 0, nullptr);
  surface->in_use = true;
  surface->sequence = sequence;

  frame->surface.reset(ref, base::scoped_policy::RETAIN);
  out.kind = ZEBKIT_FRAME_IOSURFACE;
  out.iosurface = ref;
  out.buffer_id = surface->id;
  out.pixels = nullptr;
  out.stride = 0;
  out.fd = -1;
  out.size = 0;
  out.offset = 0;
  // Copied: the capturer has its buffer back now.
  frame->callbacks->Done();
  frame->done = true;
  frame->buffer = nullptr;
}
#endif

void FrameStream::MaybeDeliverPending() {
  if (pending_ && in_flight_ < max_in_flight_)
    Deliver(std::move(pending_));
}

// ---- Offscreen -------------------------------------------------------------

void OnCursorChanged(content::RenderWidgetHost* widget,
                     const ui::Cursor& cursor);
void OnPopupShown(content::WebContents* contents,
                  content::RenderWidgetHost* popup,
                  const gfx::Rect& bounds);

void InstallHooks() {
  static bool installed = false;
  if (installed)
    return;
  installed = true;
  content::EmbedderWidgetHooks hooks = content::GetEmbedderWidgetHooks();
  hooks.cursor_changed = &OnCursorChanged;
  hooks.popup_shown = &OnPopupShown;
  content::SetEmbedderWidgetHooks(hooks);
}

// The scale of every off-screen view: they are all on the headless
// screen's primary display, which has their scale and the size of the
// largest. A display of its own for each, elsewhere than at the origin,
// could not have a scale of its own: the headless screen places a
// display's DIPs at its pixels' origin, where a window of another scale is
// not.
float g_scale = 1;

float Offscreen::scale() const {
  return g_scale;
}

Offscreen::Offscreen(zebkit_view* view, const gfx::Size& size, float scale)
    : content::WebContentsObserver(view->web_contents()),
      view_(view),
      size_(size) {
  All()[view] = this;
  view->authentication_ui_enabled_ = false;
  view->offscreen_view_ = true;
  InstallHooks();
  // A drag the page starts is run over the page from the application's
  // mouse events (drag.cc).
  InstallDragHook();
#if BUILDFLAG(IS_MAC)
  // A <select>'s list as a popup widget, which is drawn off-screen with the
  // page, rather than a menu of the system's.
  std::ignore = web_contents()->ForbidExternalPopupMenus().Release();
#endif
  const bool rescale = scale > 0 && scale != g_scale;
  if (scale > 0)
    g_scale = scale;
  UpdateDisplay();
  if (rescale) {
    for (auto& [other, offscreen] : All())
      offscreen->PlaceWindow();
  }
  PlaceWindow();
  AttachMainWidget();
}

Offscreen::~Offscreen() {
  ClosePopup();
  if (main_widget_)
    main_widget_->RemoveObserver(this);
  All().erase(view_);
}

std::map<zebkit_view*, Offscreen*>& Offscreen::All() {
  static base::NoDestructor<std::map<zebkit_view*, Offscreen*>> all;
  return *all;
}

Offscreen* Offscreen::Of(zebkit_view* view) {
  auto it = All().find(view);
  return it != All().end() ? it->second : nullptr;
}

Offscreen* Offscreen::OfContents(content::WebContents* contents) {
  for (auto& [view, offscreen] : All()) {
    if (offscreen->web_contents() == contents)
      return offscreen;
  }
  return nullptr;
}

content::RenderWidgetHostView* Offscreen::MainView() {
  return web_contents() ? web_contents()->GetRenderWidgetHostView() : nullptr;
}

content::RenderWidgetHost* Offscreen::MainWidget() {
  content::RenderWidgetHostView* view = MainView();
  return view ? view->GetRenderWidgetHost() : nullptr;
}

gfx::Size Offscreen::PixelSize(content::RenderWidgetHostView* view) const {
  if (!view)
    return gfx::ScaleToCeiledSize(size_, scale());
  return gfx::ScaleToCeiledSize(view->GetViewBounds().size(),
                                view->GetDeviceScaleFactor());
}

void Offscreen::UpdateDisplay() {
  gfx::Size largest;
  for (auto& [view, offscreen] : All())
    largest.SetToMax(offscreen->size());
  // Room around the views, as a screen has around a window: a popup that
  // does not fit below its <select> in the view still opens below it.
  largest.Enlarge(1024, 1024);
  display::Display display = display::Screen::Get()->GetPrimaryDisplay();
  headless::SetDisplayGeometry(
      display, gfx::Rect(gfx::ScaleToCeiledSize(largest, g_scale)),
      gfx::Insets(), g_scale);
  display::HeadlessScreenManager::Get()->UpdateDisplay(display);
}

void Offscreen::PlaceWindow() {
  // The page the view's size, whatever the window's frame takes; the
  // display gives it its scale.
  if (content::Shell* shell = view_->shell())
    shell->ResizeWebContentForTests(size_);
}

void Offscreen::SetSize(const gfx::Size& size, float scale) {
  if (!size.IsEmpty())
    size_ = size;
  // A new scale is every off-screen view's.
  const bool rescale = scale > 0 && scale != g_scale;
  if (scale > 0)
    g_scale = scale;
  UpdateDisplay();
  if (rescale) {
    for (auto& [view, offscreen] : All())
      offscreen->PlaceWindow();
  } else {
    PlaceWindow();
  }
  main_.SetPixelSize(gfx::ScaleToCeiledSize(size_, this->scale()));
}

void Offscreen::AttachMainWidget() {
  content::RenderWidgetHost* widget = MainWidget();
  if (widget == main_widget_)
    return;
  if (main_widget_)
    main_widget_->RemoveObserver(this);
  main_widget_ = widget;
  if (!widget)
    return;
  widget->AddObserver(this);
  main_.set_bounds(gfx::Rect(size_));
  if (main_.started())
    main_.Retarget(MainView());
  else if (on_frame_)
    main_.Start(MainView(), PixelSize(MainView()), buffers_, fps_);
  if (focused_) {
    widget->SetActive(true);
    widget->Focus();
  }
  SubscribeCaret();
}

void Offscreen::SubscribeCaret() {
  // There is something to subscribe to once the page has a widget.
  if (caret_subscription_ || !on_caret_ || !MainView())
    return;
  caret_subscription_ = web_contents()->RegisterFocusSelectionBoundsChanged(
      base::BindRepeating(
          [](base::WeakPtr<Offscreen> self, content::RenderWidgetHostView*) {
            if (self)
              self->CaretChanged();
          },
          weak_factory_.GetWeakPtr()));
}

void Offscreen::SetOnCaret(zebkit_caret_func func, void* data) {
  on_caret_ = func;
  on_caret_data_ = data;
  last_caret_.reset();
  SubscribeCaret();
}

void Offscreen::SetOnFrame(zebkit_frame_func func, void* data) {
  on_frame_ = func;
  on_frame_data_ = data;
  if (!func) {
    main_.Stop();
    if (popup_frames_)
      popup_frames_->Stop();
    return;
  }
  if (!main_.started())
    main_.Start(MainView(), PixelSize(MainView()), buffers_, fps_);
  if (popup_frames_ && !popup_frames_->started() && popup_ && popup_->GetView())
    popup_frames_->Start(popup_->GetView(), PixelSize(popup_->GetView()),
                         buffers_, fps_);
}

void Offscreen::SetFrameRate(double fps) {
  fps_ = fps;
  main_.SetFrameRate(fps);
  if (popup_frames_)
    popup_frames_->SetFrameRate(fps);
}

void Offscreen::SetMaxInFlight(int count) {
  max_in_flight_ = std::clamp(count, 1, 8);
  main_.SetMaxInFlight(max_in_flight_);
  if (popup_frames_)
    popup_frames_->SetMaxInFlight(max_in_flight_);
}

void Offscreen::SetBuffers(zebkit_frame_buffers buffers) {
  if (buffers == buffers_)
    return;
  buffers_ = buffers;
  if (main_.started())
    main_.Start(MainView(), PixelSize(MainView()), buffers_, fps_);
}

void Offscreen::RequestFrame() {
  main_.RequestFrame();
  if (popup_frames_)
    popup_frames_->RequestFrame();
}

void Offscreen::SetFocus(bool focused) {
  focused_ = focused;
  content::RenderWidgetHost* widget = MainWidget();
  if (!widget)
    return;
  // Active as the window it would be in is, which a <select> needs to open
  // its list; focused as the page.
  widget->SetActive(focused);
  if (focused) {
    widget->Focus();
  } else {
    ClosePopup();
    widget->Blur();
  }
}

void Offscreen::CaretChanged() {
  if (!on_caret_ || !web_contents())
    return;
  content::RenderFrameHost* frame = web_contents()->GetFocusedFrame();
  std::optional<gfx::Rect> caret;
  if (frame)
    caret = web_contents()->GetFocusSelectionBounds(frame);
  if (caret) {
    // In the screen's coordinates: the view's from there.
    if (content::RenderWidgetHostView* view = MainView())
      caret->Offset(-view->GetViewBounds().OffsetFromOrigin());
  }
  if (caret == last_caret_)
    return;
  last_caret_ = caret;
  if (caret) {
    const zebkit_rect rect = ToRect(*caret);
    on_caret_(view_, &rect, on_caret_data_);
  } else {
    on_caret_(view_, nullptr, on_caret_data_);
  }
}

bool Offscreen::Owns(content::RenderWidgetHost* widget) {
  if (!widget || !web_contents())
    return false;
  if (widget == popup_ || widget == MainWidget())
    return true;
  bool found = false;
  web_contents()->ForEachRenderFrameHost([&](content::RenderFrameHost* frame) {
    if (frame->GetRenderWidgetHost() == widget)
      found = true;
  });
  return found;
}

// ---- Popups ----------------------------------------------------------------

void Offscreen::PopupShown(content::RenderWidgetHost* popup,
                           const gfx::Rect& screen) {
  ClosePopup();
  content::RenderWidgetHostView* main = MainView();
  if (!popup || !main)
    return;
  popup_ = popup;
  popup_->AddObserver(this);
  popup_bounds_ = screen;
  popup_bounds_.Offset(-main->GetViewBounds().OffsetFromOrigin());
#if BUILDFLAG(IS_MAC)
  if (content::RenderWidgetHostView* view = popup->GetView())
    HideOffscreenPopupWindow(view->GetNativeView());
#endif
  popup_frames_ = std::make_unique<FrameStream>(this, ZEBKIT_LAYER_POPUP);
  popup_frames_->set_bounds(popup_bounds_);
  popup_frames_->SetMaxInFlight(max_in_flight_);
  if (on_frame_) {
    popup_frames_->Start(
        popup->GetView(),
        gfx::ScaleToCeiledSize(popup_bounds_.size(), scale()), buffers_, fps_);
  }
  if (on_popup_) {
    const zebkit_rect rect = ToRect(popup_bounds_);
    on_popup_(view_, true, &rect, on_popup_data_);
  }
}

void Offscreen::ClosePopup() {
  if (!popup_)
    return;
  popup_->RemoveObserver(this);
  popup_ = nullptr;
  popup_frames_.reset();
  mouse_in_popup_ = false;
  if (on_popup_)
    on_popup_(view_, false, nullptr, on_popup_data_);
}

void Offscreen::RenderWidgetHostDestroyed(content::RenderWidgetHost* widget) {
  if (widget == popup_) {
    ClosePopup();
  } else if (widget == main_widget_) {
    widget->RemoveObserver(this);
    main_widget_ = nullptr;
  }
}

void Offscreen::RenderWidgetHostDidUpdateVisualProperties(
    content::RenderWidgetHost* widget) {
  if (widget == popup_ && popup_frames_ && widget->GetView()) {
    popup_frames_->SetPixelSize(PixelSize(widget->GetView()));
  } else if (widget == main_widget_) {
    // A window keeps its pixels when its display's scale changes, and so
    // its page is another size in CSS pixels: the view's again.
    if (content::RenderWidgetHostView* view = MainView();
        view && view->GetViewBounds().size() != size_) {
      PlaceWindow();
    }
    main_.SetPixelSize(PixelSize(MainView()));
  }
}

bool Offscreen::InPopup(float x, float y, gfx::PointF* local) const {
  if (!popup_)
    return false;
  if (!gfx::RectF(popup_bounds_).Contains(x, y) && !mouse_in_popup_)
    return false;
  *local = gfx::PointF(x - popup_bounds_.x(), y - popup_bounds_.y());
  return true;
}

content::RenderWidgetHost* Offscreen::KeyTarget() {
  return popup_ ? popup_.get() : MainWidget();
}

void Offscreen::RenderFrameHostChanged(content::RenderFrameHost* old_host,
                                       content::RenderFrameHost* new_host) {
  if (new_host && new_host->IsInPrimaryMainFrame())
    AttachMainWidget();
}

void Offscreen::RenderViewReady() {
  AttachMainWidget();
}

void Offscreen::PrimaryMainFrameRenderProcessGone(
    base::TerminationStatus status) {
  ClosePopup();
}

void Offscreen::WebContentsDestroyed() {
  delete this;
}

// ---- Input -----------------------------------------------------------------

int BlinkModifiers(unsigned modifiers) {
  int out = 0;
  if (modifiers & ZEBKIT_MODIFIER_SHIFT)
    out |= blink::WebInputEvent::kShiftKey;
  if (modifiers & ZEBKIT_MODIFIER_CONTROL)
    out |= blink::WebInputEvent::kControlKey;
  if (modifiers & ZEBKIT_MODIFIER_ALT)
    out |= blink::WebInputEvent::kAltKey;
  if (modifiers & ZEBKIT_MODIFIER_META)
    out |= blink::WebInputEvent::kMetaKey;
  if (modifiers & ZEBKIT_MODIFIER_CAPS_LOCK)
    out |= blink::WebInputEvent::kCapsLockOn;
  if (modifiers & ZEBKIT_MODIFIER_NUM_LOCK)
    out |= blink::WebInputEvent::kNumLockOn;
  if (modifiers & ZEBKIT_MODIFIER_LEFT_BUTTON)
    out |= blink::WebInputEvent::kLeftButtonDown;
  if (modifiers & ZEBKIT_MODIFIER_MIDDLE_BUTTON)
    out |= blink::WebInputEvent::kMiddleButtonDown;
  if (modifiers & ZEBKIT_MODIFIER_RIGHT_BUTTON)
    out |= blink::WebInputEvent::kRightButtonDown;
  if (modifiers & ZEBKIT_MODIFIER_KEYPAD)
    out |= blink::WebInputEvent::kIsKeyPad;
  if (modifiers & ZEBKIT_MODIFIER_AUTO_REPEAT)
    out |= blink::WebInputEvent::kIsAutoRepeat;
  return out;
}

blink::WebPointerProperties::Button BlinkButton(zebkit_mouse_button button) {
  using Button = blink::WebPointerProperties::Button;
  switch (button) {
    case ZEBKIT_BUTTON_LEFT:
      return Button::kLeft;
    case ZEBKIT_BUTTON_MIDDLE:
      return Button::kMiddle;
    case ZEBKIT_BUTTON_RIGHT:
      return Button::kRight;
    case ZEBKIT_BUTTON_BACK:
      return Button::kBack;
    case ZEBKIT_BUTTON_FORWARD:
      return Button::kForward;
    default:
      return Button::kNoButton;
  }
}

void Offscreen::SendMouse(zebkit_mouse_type type,
                          float x,
                          float y,
                          zebkit_mouse_button button,
                          int click_count,
                          unsigned modifiers) {
  // While the page drags, its mouse events are the drag's (drag.cc).
  NoteMousePosition(view_, x, y);
  if (DragTakesMouse(view_, type, x, y, modifiers))
    return;
  blink::WebInputEvent::Type kind;
  switch (type) {
    case ZEBKIT_MOUSE_DOWN:
      kind = blink::WebInputEvent::Type::kMouseDown;
      break;
    case ZEBKIT_MOUSE_UP:
      kind = blink::WebInputEvent::Type::kMouseUp;
      break;
    case ZEBKIT_MOUSE_LEAVE:
      kind = blink::WebInputEvent::Type::kMouseLeave;
      break;
    default:
      kind = blink::WebInputEvent::Type::kMouseMove;
      break;
  }
  blink::WebMouseEvent event(kind, BlinkModifiers(modifiers),
                             base::TimeTicks::Now());
  event.button = BlinkButton(button);
  // A move with a button held is a drag of that button, as the platforms'
  // events say and as Blink's dragging (a slider's thumb, a text
  // selection, a drag and drop) requires.
  if (type == ZEBKIT_MOUSE_MOVE && button == ZEBKIT_BUTTON_NONE) {
    using Button = blink::WebPointerProperties::Button;
    event.button = (modifiers & ZEBKIT_MODIFIER_LEFT_BUTTON)     ? Button::kLeft
                   : (modifiers & ZEBKIT_MODIFIER_MIDDLE_BUTTON) ? Button::kMiddle
                   : (modifiers & ZEBKIT_MODIFIER_RIGHT_BUTTON)  ? Button::kRight
                                                                 : Button::kNoButton;
  } else if (type == ZEBKIT_MOUSE_LEAVE) {
    event.button = blink::WebPointerProperties::Button::kNoButton;
  }
  // The button pressed is down in the event's own modifiers, as the
  // platforms' events have it.
  event.UpdateEventModifiersToMatchButton();
  event.click_count =
      (type == ZEBKIT_MOUSE_DOWN || type == ZEBKIT_MOUSE_UP) ? click_count : 0;
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;

  content::RenderWidgetHost* target = MainWidget();
  gfx::PointF local;
  if (InPopup(x, y, &local)) {
    target = popup_;
    if (type == ZEBKIT_MOUSE_DOWN)
      mouse_in_popup_ = true;
  } else {
    local = gfx::PointF(x, y);
    // A press outside an open popup closes it, as a click outside a menu
    // does; the page gets the press.
    if (type == ZEBKIT_MOUSE_DOWN && popup_)
      ClosePopup();
  }
  if (type == ZEBKIT_MOUSE_UP)
    mouse_in_popup_ = false;
  if (!target)
    return;
  event.SetPositionInWidget(local.x(), local.y());
  content::RenderWidgetHostView* view = target->GetView();
  const gfx::Point origin =
      view ? view->GetViewBounds().origin() : gfx::Point();
  event.SetPositionInScreen(origin.x() + local.x(), origin.y() + local.y());
  target->ForwardMouseEvent(event);
}

void Offscreen::SendWheel(float x,
                          float y,
                          float dx,
                          float dy,
                          bool precise,
                          zebkit_wheel_phase phase,
                          unsigned modifiers) {
  blink::WebMouseWheelEvent event(blink::WebInputEvent::Type::kMouseWheel,
                                  BlinkModifiers(modifiers),
                                  base::TimeTicks::Now());
  // Blink's deltas are the content's movement: the opposite of the DOM's.
  event.delta_x = -dx;
  event.delta_y = -dy;
  // A wheel's notch is 53 pixels in Chromium, Windows' and GTK's alike.
  event.wheel_ticks_x = precise ? 0 : -dx / 53.0f;
  event.wheel_ticks_y = precise ? 0 : -dy / 53.0f;
  event.delta_units = precise ? ui::ScrollGranularity::kScrollByPrecisePixel
                              : ui::ScrollGranularity::kScrollByPixel;
  switch (phase) {
    case ZEBKIT_WHEEL_PHASE_BEGAN:
      event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
      break;
    case ZEBKIT_WHEEL_PHASE_CHANGED:
      event.phase = blink::WebMouseWheelEvent::kPhaseChanged;
      break;
    case ZEBKIT_WHEEL_PHASE_ENDED:
      event.phase = blink::WebMouseWheelEvent::kPhaseEnded;
      break;
    case ZEBKIT_WHEEL_PHASE_MOMENTUM:
      event.momentum_phase = blink::WebMouseWheelEvent::kPhaseChanged;
      break;
    default:
      // A wheel's notch is a gesture of its own: Blink wants a phase on
      // every wheel event to make the gesture scroll of it.
      event.phase = blink::WebMouseWheelEvent::kPhaseNone;
      event.has_synthetic_phase = true;
      break;
  }
  event.dispatch_type = blink::WebInputEvent::DispatchType::kBlocking;
  event.event_action =
      blink::WebMouseWheelEvent::GetPlatformSpecificDefaultEventAction(event);
  event.pointer_type = blink::WebPointerProperties::PointerType::kMouse;

  content::RenderWidgetHost* target = MainWidget();
  gfx::PointF local;
  if (InPopup(x, y, &local))
    target = popup_;
  else
    local = gfx::PointF(x, y);
  if (!target)
    return;
  event.SetPositionInWidget(local.x(), local.y());
  content::RenderWidgetHostView* view = target->GetView();
  const gfx::Point origin =
      view ? view->GetViewBounds().origin() : gfx::Point();
  event.SetPositionInScreen(origin.x() + local.x(), origin.y() + local.y());
  if (phase == ZEBKIT_WHEEL_PHASE_NONE) {
    // A notch is a gesture of its own: began, then ended, as the platforms'
    // wheel events become in Chromium.
    event.phase = blink::WebMouseWheelEvent::kPhaseBegan;
    target->ForwardWheelEvent(event);
    blink::WebMouseWheelEvent end = event;
    end.phase = blink::WebMouseWheelEvent::kPhaseEnded;
    end.delta_x = end.delta_y = 0;
    end.wheel_ticks_x = end.wheel_ticks_y = 0;
    target->ForwardWheelEvent(end);
    return;
  }
  target->ForwardWheelEvent(event);
}

void CopyText(const char* utf8, char16_t* out, size_t cap) {
  memset(out, 0, cap * sizeof(char16_t));
  if (!utf8)
    return;
  const std::u16string text = base::UTF8ToUTF16(utf8);
  memcpy(out, text.data(), std::min(text.size(), cap - 1) * sizeof(char16_t));
}

void Offscreen::SendKey(const zebkit_key_event& in) {
  if (DragTakesKey(view_, in))
    return;
  content::RenderWidgetHost* target = KeyTarget();
  if (!target)
    return;
  blink::WebInputEvent::Type type;
  switch (in.type) {
    case ZEBKIT_KEY_UP:
      type = blink::WebInputEvent::Type::kKeyUp;
      break;
    case ZEBKIT_KEY_CHAR:
      type = blink::WebInputEvent::Type::kChar;
      break;
    default:
      type = blink::WebInputEvent::Type::kRawKeyDown;
      break;
  }
  input::NativeWebKeyboardEvent event(type, BlinkModifiers(in.modifiers),
                                      base::TimeTicks::Now());
  event.windows_key_code = in.key_code;
  event.native_key_code = in.native_key_code;

  ui::DomCode code = ui::DomCode::NONE;
  if (in.code && *in.code)
    code = ui::KeycodeConverter::CodeStringToDomCode(in.code);
  if (code == ui::DomCode::NONE && in.key_code)
    code = ui::UsLayoutKeyboardCodeToDomCode(
        static_cast<ui::KeyboardCode>(in.key_code));
  event.dom_code = static_cast<int>(code);

  ui::DomKey key = ui::DomKey::NONE;
  if (in.key && *in.key) {
    key = ui::KeycodeConverter::KeyStringToDomKey(in.key);
    // A printable key is its character.
    if (key == ui::DomKey::NONE) {
      const std::u16string text = base::UTF8ToUTF16(in.key);
      if (text.size() == 1)
        key = ui::DomKey::FromCharacter(text[0]);
    }
  }
  if (key == ui::DomKey::NONE && code != ui::DomCode::NONE) {
    ui::KeyboardCode unused;
    int flags = (in.modifiers & ZEBKIT_MODIFIER_SHIFT) ? ui::EF_SHIFT_DOWN : 0;
    if (!ui::DomCodeToUsLayoutDomKey(code, flags, &key, &unused))
      key = ui::DomKey::NONE;
  }
  event.dom_key = static_cast<uint32_t>(key);
  if (!event.windows_key_code && code != ui::DomCode::NONE) {
    ui::DomKey unused_key;
    ui::KeyboardCode key_code = ui::VKEY_UNKNOWN;
    if (ui::DomCodeToUsLayoutDomKey(code, 0, &unused_key, &key_code))
      event.windows_key_code = key_code;
  }

  if (in.type == ZEBKIT_KEY_CHAR) {
    CopyText(in.text, event.text.data(), event.text.size());
    CopyText(in.unmodified_text ? in.unmodified_text : in.text,
             event.unmodified_text.data(), event.unmodified_text.size());
  } else if (in.key_code == 0x0D || code == ui::DomCode::ENTER) {
    // Enter's text is a carriage return, which its keypress needs.
    event.text[0] = event.unmodified_text[0] = '\r';
  }
  target->ForwardKeyboardEvent(event);
}

// ---- Cursors ---------------------------------------------------------------

const char* CursorName(ui::mojom::CursorType type) {
  using T = ui::mojom::CursorType;
  switch (type) {
    case T::kPointer: return "default";
    case T::kCross: return "crosshair";
    case T::kHand: return "pointer";
    case T::kIBeam: return "text";
    case T::kWait: return "wait";
    case T::kHelp: return "help";
    case T::kEastResize: return "e-resize";
    case T::kNorthResize: return "n-resize";
    case T::kNorthEastResize: return "ne-resize";
    case T::kNorthWestResize: return "nw-resize";
    case T::kSouthResize: return "s-resize";
    case T::kSouthEastResize: return "se-resize";
    case T::kSouthWestResize: return "sw-resize";
    case T::kWestResize: return "w-resize";
    case T::kNorthSouthResize: return "ns-resize";
    case T::kEastWestResize: return "ew-resize";
    case T::kNorthEastSouthWestResize: return "nesw-resize";
    case T::kNorthWestSouthEastResize: return "nwse-resize";
    case T::kColumnResize: return "col-resize";
    case T::kRowResize: return "row-resize";
    case T::kMiddlePanning:
    case T::kMiddlePanningVertical:
    case T::kMiddlePanningHorizontal:
    case T::kMove: return "move";
    case T::kEastPanning: return "e-resize";
    case T::kNorthPanning: return "n-resize";
    case T::kNorthEastPanning: return "ne-resize";
    case T::kNorthWestPanning: return "nw-resize";
    case T::kSouthPanning: return "s-resize";
    case T::kSouthEastPanning: return "se-resize";
    case T::kSouthWestPanning: return "sw-resize";
    case T::kWestPanning: return "w-resize";
    case T::kVerticalText: return "vertical-text";
    case T::kCell: return "cell";
    case T::kContextMenu: return "context-menu";
    case T::kAlias: return "alias";
    case T::kProgress: return "progress";
    case T::kNoDrop: return "no-drop";
    case T::kCopy: return "copy";
    case T::kNone: return "none";
    case T::kNotAllowed: return "not-allowed";
    case T::kZoomIn: return "zoom-in";
    case T::kZoomOut: return "zoom-out";
    case T::kGrab: return "grab";
    case T::kGrabbing: return "grabbing";
    case T::kCustom: return "custom";
    default: return "default";
  }
}

void Offscreen::CursorChanged(const ui::Cursor& cursor) {
  if (!on_cursor_)
    return;
  const ui::mojom::CursorType type = cursor.type();
  if (type == ui::mojom::CursorType::kCustom) {
    const SkBitmap& bitmap = cursor.custom_bitmap();
    zebkit_cursor_image image = {
        static_cast<const uint8_t*>(bitmap.getPixels()),
        bitmap.width(),
        bitmap.height(),
        cursor.custom_hotspot().x(),
        cursor.custom_hotspot().y(),
        cursor.image_scale_factor(),
    };
    on_cursor_(view_, "custom", image.pixels ? &image : nullptr,
               on_cursor_data_);
    return;
  }
  on_cursor_(view_, CursorName(type), nullptr, on_cursor_data_);
}

void OnCursorChanged(content::RenderWidgetHost* widget,
                     const ui::Cursor& cursor) {
  for (auto& [view, offscreen] : Offscreen::All()) {
    if (offscreen->Owns(widget)) {
      offscreen->CursorChanged(cursor);
      return;
    }
  }
}

void OnPopupShown(content::WebContents* contents,
                  content::RenderWidgetHost* popup,
                  const gfx::Rect& bounds) {
  if (Offscreen* offscreen = Offscreen::OfContents(contents))
    offscreen->PopupShown(popup, bounds);
}

}  // namespace

void MakeOffscreenLike(zebkit_view* opener, zebkit_view* popup) {
  Offscreen* offscreen = Offscreen::Of(opener);
  if (!offscreen || !popup->web_contents())
    return;
  // The size the page asked for (window.open's features), or the opener's.
  gfx::Size size = popup->web_contents()->GetContainerBounds().size();
  if (size.width() < 2 || size.height() < 2)
    size = offscreen->size();
  // Deleted with the page (Offscreen::WebContentsDestroyed).
  new Offscreen(popup, size, offscreen->scale());
  popup->authentication_ui_enabled_ = opener->authentication_ui_enabled_;
}

}  // namespace zebkit

using zebkit::Offscreen;

extern "C" {

zebkit_view* zebkit_view_new_offscreen(const char* url,
                                       int width,
                                       int height,
                                       float scale) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (!zebkit_is_headless()) {
    LOG(ERROR) << "zebkit: off-screen views are for a process without "
                  "windows (--headless)";
    return nullptr;
  }
  const gfx::Size size(width > 0 ? width : 800, height > 0 ? height : 600);
  zebkit_view* view = zebkit_view_new(url, size.width(), size.height());
  if (!view)
    return nullptr;
  // Deleted with the page (Offscreen::WebContentsDestroyed).
  new Offscreen(view, size, scale);
  return view;
}

bool zebkit_view_is_offscreen(zebkit_view* view) {
  return Offscreen::Of(view) != nullptr;
}

void zebkit_view_set_size(zebkit_view* view,
                          int width,
                          int height,
                          float scale) {
  if (Offscreen* offscreen = Offscreen::Of(view)) {
    offscreen->SetSize(gfx::Size(std::max(width, 0), std::max(height, 0)),
                       scale);
  } else if (width > 0 && height > 0) {
    zebkit_view_resize(view, width, height);
  }
}

void zebkit_view_on_frame(zebkit_view* view, zebkit_frame_func func, void* data) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->SetOnFrame(func, data);
}

void zebkit_frame_release(const zebkit_frame* frame) {
  if (!frame)
    return;
  auto* held = reinterpret_cast<zebkit::Frame*>(const_cast<zebkit_frame*>(frame));
  auto release = [](zebkit::Frame* held) {
    if (zebkit::FrameStream* stream = held->stream.get())
      stream->Release(held);
    else
      delete held;
  };
  if (content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    release(held);
  else
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(release, base::Unretained(held)));
}

void zebkit_view_set_frame_rate(zebkit_view* view, double fps) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->SetFrameRate(fps);
}

void zebkit_view_set_max_frames_in_flight(zebkit_view* view, int count) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->SetMaxInFlight(count);
}

void zebkit_view_set_frame_buffers(zebkit_view* view,
                                   zebkit_frame_buffers buffers) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->SetBuffers(buffers);
}

void zebkit_view_request_frame(zebkit_view* view) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->RequestFrame();
}

uint32_t zebkit_frame_iosurface_mach_port(const zebkit_frame* frame) {
#if BUILDFLAG(IS_MAC)
  if (frame && frame->kind == ZEBKIT_FRAME_IOSURFACE && frame->iosurface)
    return IOSurfaceCreateMachPort(static_cast<IOSurfaceRef>(frame->iosurface));
#endif
  return 0;
}

void zebkit_view_send_mouse(zebkit_view* view,
                            zebkit_mouse_type type,
                            float x,
                            float y,
                            zebkit_mouse_button button,
                            int click_count,
                            unsigned modifiers) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->SendMouse(type, x, y, button, click_count, modifiers);
}

void zebkit_view_send_wheel(zebkit_view* view,
                            float x,
                            float y,
                            float delta_x,
                            float delta_y,
                            bool precise,
                            zebkit_wheel_phase phase,
                            unsigned modifiers) {
  if (Offscreen* offscreen = Offscreen::Of(view))
    offscreen->SendWheel(x, y, delta_x, delta_y, precise, phase, modifiers);
}

void zebkit_view_send_key(zebkit_view* view, const zebkit_key_event* event) {
  if (Offscreen* offscreen = Offscreen::Of(view); offscreen && event)
    offscreen->SendKey(*event);
}

void zebkit_view_ime_set_composition(zebkit_view* view,
                                     const char* text,
                                     int selection_start,
                                     int selection_end) {
  const std::u16string composition = base::UTF8ToUTF16(text ? text : "");
  const int length = static_cast<int>(composition.size());
  base::DictValue params;
  params.Set("text", text ? text : "");
  params.Set("selectionStart",
             selection_start < 0 ? length : std::min(selection_start, length));
  params.Set("selectionEnd",
             selection_end < 0 ? length : std::min(selection_end, length));
  view->Send("Input.imeSetComposition", std::move(params));
}

void zebkit_view_ime_commit(zebkit_view* view, const char* text) {
  base::DictValue params;
  params.Set("text", text ? text : "");
  view->Send("Input.insertText", std::move(params));
}

void zebkit_view_ime_cancel(zebkit_view* view) {
  // An empty composition takes the composition away.
  base::DictValue params;
  params.Set("text", "");
  params.Set("selectionStart", 0);
  params.Set("selectionEnd", 0);
  view->Send("Input.imeSetComposition", std::move(params));
}

void zebkit_view_on_caret(zebkit_view* view, zebkit_caret_func func, void* data) {
  if (Offscreen* offscreen = Offscreen::Of(view)) {
    offscreen->SetOnCaret(func, data);
  }
}

void zebkit_view_set_focus(zebkit_view* view, bool focused) {
  if (Offscreen* offscreen = Offscreen::Of(view)) {
    view->offscreen_focused_ = focused;
    offscreen->SetFocus(focused);
  }
}

bool zebkit_view_on_authentication(zebkit_view* view,
                                   zebkit_authentication_func func,
                                   void* data) {
  if (!view)
    return false;
  zebkit::AuthenticationHandlerChanged(view);
  view->on_authentication_ = func;
  view->on_authentication_data_ = data;
  return true;
}

bool zebkit_view_authentication_action(zebkit_view* view, const char* json) {
  return view && json && zebkit::AuthenticationAction(view, json);
}

bool zebkit_view_set_authentication_ui(zebkit_view* view, bool enabled) {
  if (!view)
    return false;
  view->authentication_ui_enabled_ = enabled;
  if (!enabled)
    zebkit::AuthenticationPolicyDisabled(view);
  return true;
}

void zebkit_view_on_cursor(zebkit_view* view, zebkit_cursor_func func, void* data) {
  if (Offscreen* offscreen = Offscreen::Of(view)) {
    offscreen->on_cursor_ = func;
    offscreen->on_cursor_data_ = data;
  }
}

void zebkit_view_on_popup(zebkit_view* view, zebkit_popup_func func, void* data) {
  if (Offscreen* offscreen = Offscreen::Of(view)) {
    offscreen->on_popup_ = func;
    offscreen->on_popup_data_ = data;
  }
}

}  // extern "C"
