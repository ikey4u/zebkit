// Drag and drop (<zebkit/page.h>).
//
// From the page: content gives a drag the page starts to the window's drag
// and drop, which an off-screen view does not have. patches/1024's hook
// gives it here instead: it is reported to the application, with what it
// carries and its picture, and either the application runs it (the
// system's drag and drop, say) or the engine does, inside the page, from
// the mouse events the application sends the off-screen view: they are the
// drag's from then on, over the page (dragenter, dragover, drop), until the
// button is released or Escape pressed, and the page's drag source is told
// how it ended (dragend), as a window's drag loop does.
//
// Into the page: what the application drags in (files from the system's
// file manager, text, links) goes to the page's widgets as the window's
// drop target would send it (RenderWidgetHost::DragTarget*): to the widget
// under the point, a frame's of another process included, at the point in
// it, a frame left when the drag moves on to another, as
// WebContentsViewAura does; files made readable by the page as they are
// dropped.
//
// CDP's own (Input.dispatchDragEvent, Input.setInterceptDrags, drags of
// Input.dispatchMouseEvent's events) are content's, and come first.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "content/public/browser/embedder_widget_hooks.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_observer.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/browser/zebkit_automation.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/drop_data.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/page/drag_operation.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/clipboard/clipboard_url_info.h"
#include "ui/base/clipboard/file_info.h"
#include "ui/base/dragdrop/mojom/drag_drop_types.mojom.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/vector2d.h"
#include "zebkit/engine/automation.h"
#include "zebkit/engine/page_internal.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

unsigned ToZebkitOperation(ui::mojom::DragOperation operation) {
  switch (operation) {
    case ui::mojom::DragOperation::kCopy:
      return ZEBKIT_DRAG_COPY;
    case ui::mojom::DragOperation::kLink:
      return ZEBKIT_DRAG_LINK;
    case ui::mojom::DragOperation::kMove:
      return ZEBKIT_DRAG_MOVE;
    default:
      return ZEBKIT_DRAG_NONE;
  }
}

ui::mojom::DragOperation ToOperation(unsigned operation) {
  if (operation & ZEBKIT_DRAG_MOVE)
    return ui::mojom::DragOperation::kMove;
  if (operation & ZEBKIT_DRAG_COPY)
    return ui::mojom::DragOperation::kCopy;
  if (operation & ZEBKIT_DRAG_LINK)
    return ui::mojom::DragOperation::kLink;
  return ui::mojom::DragOperation::kNone;
}

blink::DragOperationsMask ToMask(unsigned operations) {
  return static_cast<blink::DragOperationsMask>(operations & ZEBKIT_DRAG_ALL);
}


// What a drag carries, to and from JSON (<zebkit/page.h>).
std::string DropDataToJson(const content::DropData& data) {
  base::DictValue out;
  if (data.text)
    out.Set("text", base::UTF16ToUTF8(*data.text));
  if (data.html) {
    out.Set("html", base::UTF16ToUTF8(*data.html));
    if (data.html_base_url.is_valid())
      out.Set("htmlBaseUrl", data.html_base_url.spec());
  }
  if (!data.url_infos.empty()) {
    base::ListValue urls;
    for (const ui::ClipboardUrlInfo& info : data.url_infos) {
      base::DictValue url;
      url.Set("url", info.url.spec());
      url.Set("title", base::UTF16ToUTF8(info.title));
      urls.Append(std::move(url));
    }
    out.Set("urls", std::move(urls));
  }
  if (!data.filenames.empty()) {
    base::ListValue files;
    for (const ui::FileInfo& file : data.filenames)
      files.Append(file.path.AsUTF8Unsafe());
    out.Set("files", std::move(files));
  }
  if (!data.custom_data.empty()) {
    base::DictValue custom;
    for (const auto& [type, value] : data.custom_data) {
      // Chromium's own mark of the drag, not the page's data.
      if (type == u"chromium/x-drag-id")
        continue;
      custom.Set(base::UTF16ToUTF8(type), base::UTF16ToUTF8(value));
    }
    if (!custom.empty())
      out.Set("custom", std::move(custom));
  }
  if (!data.file_contents.empty()) {
    base::DictValue contents;
    std::string name = "image";
    if (std::optional<base::FilePath> safe =
            data.GetSafeFilenameForImageFileContents()) {
      name = safe->AsUTF8Unsafe();
    }
    contents.Set("name", name);
    if (data.file_contents_source_url.is_valid())
      contents.Set("url", data.file_contents_source_url.spec());
    contents.Set("base64", base::Base64Encode(data.file_contents));
    out.Set("fileContents", std::move(contents));
  }
  return base::WriteJson(out).value_or("{}");
}

content::DropData DropDataFromJson(const std::string& json) {
  content::DropData data;
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!parsed || !parsed->is_dict())
    return data;
  const base::DictValue& in = parsed->GetDict();
  if (const std::string* text = in.FindString("text"))
    data.text = base::UTF8ToUTF16(*text);
  if (const std::string* html = in.FindString("html")) {
    data.html = base::UTF8ToUTF16(*html);
    if (const std::string* base = in.FindString("htmlBaseUrl"))
      data.html_base_url = GURL(*base);
  }
  if (const base::ListValue* urls = in.FindList("urls")) {
    for (const base::Value& entry : *urls) {
      ui::ClipboardUrlInfo info;
      if (entry.is_string()) {
        info.url = GURL(entry.GetString());
      } else if (entry.is_dict()) {
        if (const std::string* url = entry.GetDict().FindString("url"))
          info.url = GURL(*url);
        if (const std::string* title = entry.GetDict().FindString("title"))
          info.title = base::UTF8ToUTF16(*title);
      }
      if (info.url.is_valid())
        data.url_infos.push_back(std::move(info));
    }
  }
  if (const base::ListValue* files = in.FindList("files")) {
    for (const base::Value& entry : *files) {
      if (!entry.is_string() || entry.GetString().empty())
        continue;
      const base::FilePath path = base::FilePath::FromUTF8Unsafe(entry.GetString());
      data.filenames.emplace_back(path, path.BaseName());
    }
  }
  if (const base::DictValue* custom = in.FindDict("custom")) {
    for (const auto [type, value] : *custom) {
      if (value.is_string()) {
        data.custom_data[base::UTF8ToUTF16(type)] =
            base::UTF8ToUTF16(value.GetString());
      }
    }
  }
  if (const base::DictValue* contents = in.FindDict("fileContents")) {
    if (const std::string* encoded = contents->FindString("base64")) {
      if (std::optional<std::vector<uint8_t>> bytes =
              base::Base64Decode(*encoded)) {
        data.file_contents = std::move(*bytes);
      }
    }
    if (const std::string* url = contents->FindString("url"))
      data.file_contents_source_url = GURL(*url);
    if (const std::string* name = contents->FindString("name")) {
      const base::FilePath path = base::FilePath::FromUTF8Unsafe(*name);
      data.file_contents_filename_extension = path.Extension();
      data.file_contents_content_disposition =
          "attachment; filename=\"" + *name + "\"";
    }
  }
  return data;
}

// A drag over the page, the page's own or the application's, with the
// dragover answers still to come: a drop waits for them, since what it
// does is what the last chose.
struct Target {
  content::DropData data;
  blink::DragOperationsMask operations = blink::kDragOperationNone;
  bool entered = false;
  gfx::PointF point;
  // The widget the drag is over (process and routing id), and the point in it.
  int process = -1;
  int routing = -1;
  gfx::PointF local;
  unsigned operation = ZEBKIT_DRAG_NONE;
  int pending = 0;
  std::vector<base::OnceClosure> after;
};

class DragState : public content::WebContentsUserData<DragState>,
                  public content::RenderWidgetHostObserver {
 public:
  ~DragState() override { StopObserving(); }

  zebkit_drag_func on_start = nullptr;
  void* on_start_data = nullptr;
  zebkit_drag_end_func on_end = nullptr;
  void* on_end_data = nullptr;
  std::optional<gfx::PointF> mouse;

  // The page's own drag, while it lasts.
  struct Source {
    raw_ptr<content::RenderWidgetHost> widget;
    bool engine_runs = false;
    gfx::PointF point;
  };
  std::optional<Source> source;
  // Automation drives the page's input (crDragDrop.ts's drags).
  bool automated = false;
  // The drag over the page: the engine's run of the page's drag, or the
  // application's.
  Target target;

  bool Start(content::RenderWidgetHost* widget,
             const content::DropData& data,
             int operations,
             const SkBitmap& image,
             float scale,
             const gfx::Vector2d& offset,
             const gfx::Point& location) {
    zebkit_view* view = ViewOf(&GetWebContents());
    const bool offscreen = view && zebkit_view_is_offscreen(view);
    // A window's: the system's drag and drop, as before; unless automation
    // drives the page, whose mouse events carry the drag as an off-screen
    // view's do.
    automated = AutomationDrivesInput(&GetWebContents());
    if (!offscreen && !on_start && !automated)
      return false;
    if (source)
      return true;  // One at a time; the page is still in the last.

    gfx::PointF start = mouse.value_or(gfx::PointF());
    if (!mouse || !offscreen) {
      gfx::Point origin;
      if (content::RenderWidgetHostView* rwhv = widget->GetView())
        origin = rwhv->GetViewBounds().origin();
      start = gfx::PointF(location.x() - origin.x(), location.y() - origin.y());
    }

    bool taken = false;
    if (view && on_start) {
      const std::string json = DropDataToJson(data);
      SkBitmap pixels;
      if (!image.drawsNothing()) {
        if (image.colorType() == kN32_SkColorType &&
            image.alphaType() == kPremul_SkAlphaType) {
          pixels = image;
        } else if (pixels.tryAllocPixels(image.info()
                                             .makeColorType(kN32_SkColorType)
                                             .makeAlphaType(kPremul_SkAlphaType))) {
          if (!image.readPixels(pixels.pixmap()))
            pixels.reset();
        }
      }
      zebkit_drag drag = {};
      drag.data_json = json.c_str();
      drag.operations = static_cast<unsigned>(operations) & ZEBKIT_DRAG_ALL;
      drag.x = start.x();
      drag.y = start.y();
      if (!pixels.drawsNothing()) {
        drag.image = static_cast<const uint8_t*>(pixels.getPixels());
        drag.image_width = pixels.width();
        drag.image_height = pixels.height();
        drag.image_stride = static_cast<int>(pixels.rowBytes());
        drag.image_scale = scale;
        drag.image_offset_x = offset.x();
        drag.image_offset_y = offset.y();
      }
      // The application is told before the drag is the engine's: a source
      // set up after would miss what it does in the call.
      source = Source{widget, false, start};
      Observe(widget);
      taken = on_start(view, &drag, on_start_data);
      if (!source)
        return true;  // Ended in the call.
    }
    if (!offscreen && !taken && !automated) {
      // A window's drag the application did not take: the system's.
      source.reset();
      StopObserving();
      return false;
    }
    if (!source) {
      source = Source{widget, false, start};
      Observe(widget);
    }
    source->engine_runs = !taken;
    if (source->engine_runs) {
      target = Target();
      target.data = data;
      target.operations = static_cast<blink::DragOperationsMask>(operations);
      target.point = start;
      if (automated) {
        // As CDP's intercepted drag has it: without the source document's
        // own marker.
        target.data.custom_data.erase(u"chromium/x-drag-id");
        // crDragDrop.ts: the drag enters where the mouse that started it is.
        if (mouse)
          Enter(*mouse, 0);
      }
    }
    return true;
  }

  // The page's drag ended with @operation: dragend at the source.
  void EndSource(gfx::PointF point, unsigned operation) {
    if (!source)
      return;
    Source ended = *source;
    source.reset();
    StopObserving();
    if (ended.widget) {
      // In the source's own coordinates: a frame's when it began in one.
      gfx::PointF local = point;
      if (content::RenderWidgetHostView* root =
              GetWebContents().GetRenderWidgetHostView();
          root && ended.widget->GetView() &&
          ended.widget->GetView() != root) {
        local = ended.widget->GetView()->TransformRootPointToViewCoordSpace(
            point);
      }
      ended.widget->DragSourceEndedAt(local, point, ToOperation(operation),
                                      base::DoNothing());
      ended.widget->DragSourceSystemDragEnded();
    }
    if (zebkit_view* view = ViewOf(&GetWebContents()); view && on_end)
      on_end(view, operation, on_end_data);
  }

  content::RenderWidgetHost* MainWidget() {
    content::RenderWidgetHostView* view =
        GetWebContents().GetRenderWidgetHostView();
    return view ? view->GetRenderWidgetHost() : nullptr;
  }

  // ---- The drag over the page -------------------------------------------

  // Runs @then once the answers to what was sent have come.
  void WhenSettled(base::OnceClosure then) {
    if (target.pending == 0) {
      std::move(then).Run();
      return;
    }
    target.after.push_back(std::move(then));
  }

  content::RenderWidgetHost::DragOperationCallback Answer(
      base::OnceCallback<void(unsigned)> also = {}) {
    ++target.pending;
    return base::BindOnce(
        [](base::WeakPtr<DragState> self,
           base::OnceCallback<void(unsigned)> also,
           ui::mojom::DragOperation operation, bool document_is_handling) {
          if (!self)
            return;
          Target& target = self->target;
          target.operation = ToZebkitOperation(operation);
          target.data.operation = operation;
          target.data.document_is_handling_drag = document_is_handling;
          if (also)
            std::move(also).Run(target.operation);
          self->Settle();
        },
        weak_factory_.GetWeakPtr(), std::move(also));
  }

  void Settle() {
    if (--target.pending > 0)
      return;
    std::vector<base::OnceClosure> after;
    after.swap(target.after);
    for (base::OnceClosure& closure : after)
      std::move(closure).Run();
  }

  content::RenderWidgetHost* Current() {
    return target.process < 0
               ? nullptr
               : content::RenderWidgetHost::FromID(target.process,
                                                   target.routing);
  }

  // The widget under @point, a frame's of another process included, and
  // the point in it, to @then; the drag left the widget it was over when it
  // is another (dragleave there, then dragenter in the new one). The hit
  // tests are answered in the order they are asked, and a drop waits for
  // them (pending).
  using Located = base::OnceCallback<void(content::RenderWidgetHost*)>;
  void Locate(gfx::PointF point, int modifiers, Located then) {
    ++target.pending;
    content::FindWidgetAtPoint(
        &GetWebContents(), point,
        base::BindOnce(
            [](base::WeakPtr<DragState> self, gfx::PointF point, int modifiers,
               Located then, content::RenderWidgetHost* widget,
               gfx::PointF local) {
              if (!self)
                return;
              if (!widget) {
                widget = self->MainWidget();
                local = point;
              }
              Target& target = self->target;
              content::RenderWidgetHost* current = self->Current();
              if (widget && target.entered && current != widget) {
                if (current)
                  current->DragTargetDragLeave(target.local, target.point);
                content::DropData filtered = target.data;
                widget->FilterDropData(&filtered);
                widget->DragTargetDragEnter(filtered, local, point,
                                            target.operations, modifiers,
                                            self->Answer());
              }
              if (widget) {
                target.process = widget->GetProcess()->GetDeprecatedID();
                target.routing = widget->GetRoutingID();
              }
              target.local = local;
              target.point = point;
              std::move(then).Run(target.entered ? widget : nullptr);
              self->Settle();
            },
            weak_factory_.GetWeakPtr(), point, modifiers, std::move(then)));
  }

  void Enter(gfx::PointF point, int modifiers) {
    if (!MainWidget())
      return;
    target.point = point;
    target.entered = true;
    // No widget yet: the one under the point gets dragenter.
    target.process = target.routing = -1;
    Locate(point, modifiers, base::DoNothing());
  }

  void Over(gfx::PointF point,
            int modifiers,
            base::OnceCallback<void(unsigned)> done = {}) {
    if (!MainWidget() || !target.entered) {
      if (done)
        std::move(done).Run(ZEBKIT_DRAG_NONE);
      return;
    }
    Locate(point, modifiers,
           base::BindOnce(
               [](base::WeakPtr<DragState> self, gfx::PointF point,
                  int modifiers, base::OnceCallback<void(unsigned)> done,
                  content::RenderWidgetHost* widget) {
                 if (!self || !widget) {
                   if (done)
                     std::move(done).Run(ZEBKIT_DRAG_NONE);
                   return;
                 }
                 Target& target = self->target;
                 widget->DragTargetDragOver(target.local, point,
                                            target.operations, modifiers,
                                            self->Answer(std::move(done)));
               },
               weak_factory_.GetWeakPtr(), point, modifiers,
               std::move(done)));
  }

  void Leave() {
    if (!target.entered)
      return;
    target.entered = false;
    target.operation = ZEBKIT_DRAG_NONE;
    if (content::RenderWidgetHost* widget = Current())
      widget->DragTargetDragLeave(target.local, target.point);
    target.process = target.routing = -1;
  }

  // Drops at @point once the dragovers are answered; @done gets what was
  // done. Nothing is dropped where the page took nothing.
  void Drop(gfx::PointF point,
            int modifiers,
            base::OnceCallback<void(unsigned)> done) {
    // Dropped elsewhere than the last dragover: the widget there.
    if (target.entered && point != target.point)
      Locate(point, modifiers, base::DoNothing());
    WhenSettled(base::BindOnce(
        [](base::WeakPtr<DragState> self, gfx::PointF point, int modifiers,
           base::OnceCallback<void(unsigned)> done) {
          if (!self)
            return;
          Target& target = self->target;
          // Where the last dragover was: the drop's point is the same.
          content::RenderWidgetHost* widget = self->Current();
          if (!widget || !target.entered ||
              target.operation == ZEBKIT_DRAG_NONE) {
            self->Leave();
            std::move(done).Run(ZEBKIT_DRAG_NONE);
            return;
          }
          const unsigned operation = target.operation;
          target.entered = false;
          target.process = target.routing = -1;
          content::DropData filtered = target.data;
          widget->FilterDropData(&filtered);
          widget->DragTargetDrop(
              filtered, target.local, point, modifiers,
              base::BindOnce(std::move(done), operation));
        },
        weak_factory_.GetWeakPtr(), point, modifiers, std::move(done)));
  }

  // ---- The engine's run of the page's drag -------------------------------

  bool TakeMouse(zebkit_mouse_type type, float x, float y, unsigned modifiers) {
    if (!source || !source->engine_runs)
      return false;
    const gfx::PointF point(x, y);
    const int blink = BlinkModifiers(modifiers);
    switch (type) {
      case ZEBKIT_MOUSE_MOVE:
        source->point = point;
        if (!target.entered)
          Enter(point, blink);
        Over(point, blink);
        return true;
      case ZEBKIT_MOUSE_LEAVE:
        Leave();
        return true;
      case ZEBKIT_MOUSE_UP:
        source->point = point;
        if (!target.entered) {
          EndSource(point, ZEBKIT_DRAG_NONE);
          return true;
        }
        Over(point, blink);
        Drop(point, blink,
             base::BindOnce(
                 [](base::WeakPtr<DragState> self, gfx::PointF point,
                    unsigned operation) {
                   if (self)
                     self->EndSource(point, operation);
                 },
                 weak_factory_.GetWeakPtr(), point));
        return true;
      default:
        return true;
    }
  }

  bool TakeKey(const zebkit_key_event& event) {
    if (!source || !source->engine_runs)
      return false;
    const bool escape = (event.key && std::string(event.key) == "Escape") ||
                        (event.code && std::string(event.code) == "Escape") ||
                        event.key_code == 0x1B;
    if (escape && event.type == ZEBKIT_KEY_DOWN) {
      // Input.cancelDragging's: the source told, no dragleave.
      if (!automated)
        Leave();
      EndSource(source->point, ZEBKIT_DRAG_NONE);
    }
    return true;
  }

  // content::RenderWidgetHostObserver
  void RenderWidgetHostDestroyed(content::RenderWidgetHost* widget) override {
    if (source && source->widget == widget) {
      source->widget = nullptr;
      EndSource(source->point, ZEBKIT_DRAG_NONE);
    }
    StopObserving();
  }

 private:
  friend class content::WebContentsUserData<DragState>;
  explicit DragState(content::WebContents* contents)
      : content::WebContentsUserData<DragState>(*contents) {}

  void Observe(content::RenderWidgetHost* widget) {
    StopObserving();
    observed_ = widget;
    widget->AddObserver(this);
  }
  void StopObserving() {
    if (observed_)
      observed_->RemoveObserver(this);
    observed_ = nullptr;
  }

  raw_ptr<content::RenderWidgetHost> observed_ = nullptr;
  base::WeakPtrFactory<DragState> weak_factory_{this};

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(DragState);

DragState* StateOf(content::WebContents* contents) {
  if (!contents)
    return nullptr;
  DragState::CreateForWebContents(contents);
  return DragState::FromWebContents(contents);
}

DragState* StateOf(zebkit_view* view) {
  return zebkit::LiveView(view) ? StateOf(view->web_contents()) : nullptr;
}

bool OnDragStarted(content::WebContents* contents,
                   content::RenderWidgetHost* widget,
                   const content::DropData& data,
                   int operations,
                   const SkBitmap& image,
                   float scale,
                   const gfx::Vector2d& cursor_offset,
                   const gfx::Point& location) {
  if (!ViewOf(contents))
    return false;
  return StateOf(contents)->Start(widget, data, operations, image, scale,
                                  cursor_offset, location);
}

}  // namespace

void InstallDragHook() {
  content::EmbedderWidgetHooks hooks = content::GetEmbedderWidgetHooks();
  if (hooks.drag_started == &OnDragStarted)
    return;
  hooks.drag_started = &OnDragStarted;
  content::SetEmbedderWidgetHooks(hooks);
}

bool DragTakesMouse(zebkit_view* view,
                    zebkit_mouse_type type,
                    float x,
                    float y,
                    unsigned modifiers) {
  DragState* state = view && view->web_contents()
                         ? DragState::FromWebContents(view->web_contents())
                         : nullptr;
  return state && state->TakeMouse(type, x, y, modifiers);
}

bool DragTakesKey(zebkit_view* view, const zebkit_key_event& event) {
  DragState* state = view && view->web_contents()
                         ? DragState::FromWebContents(view->web_contents())
                         : nullptr;
  return state && state->TakeKey(event);
}

void NoteMousePosition(zebkit_view* view, float x, float y) {
  if (DragState* state = StateOf(view))
    state->mouse = gfx::PointF(x, y);
}

// The application's answer for a drag over or a drop, called once: with the
// operation, or with none as the view closes, before on_close
// (zebkit_view::AddCancel()).
base::OnceCallback<void(unsigned)> DragAnswer(zebkit_view* view,
                                              zebkit_drag_operation_func func,
                                              void* data) {
  const int id = view->AddCancel(base::BindOnce(
      [](zebkit_view* view, zebkit_drag_operation_func func, void* data) {
        if (func)
          func(view, ZEBKIT_DRAG_NONE, data);
      },
      base::Unretained(view), func, data));
  return base::BindOnce(
      [](base::WeakPtr<zebkit_view> view, int id, zebkit_drag_operation_func func,
         void* data, unsigned operation) {
        if (view && view->TakeCancel(id) && func)
          func(view.get(), operation, data);
      },
      view->GetWeakPtr(), id, func, data);
}

}  // namespace zebkit

using zebkit::StateOf;

extern "C" {

void zebkit_view_on_drag_start(zebkit_view* view,
                               zebkit_drag_func func,
                               void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::InstallDragHook();
  if (zebkit::DragState* state = StateOf(view)) {
    state->on_start = func;
    state->on_start_data = data;
  }
}

void zebkit_view_on_drag_end(zebkit_view* view,
                             zebkit_drag_end_func func,
                             void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::DragState* state = StateOf(view)) {
    state->on_end = func;
    state->on_end_data = data;
  }
}

bool zebkit_view_is_dragging(zebkit_view* view) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view, false);
  zebkit::DragState* state = StateOf(view);
  return state && state->source.has_value();
}

void zebkit_view_drag_enter(zebkit_view* view,
                            const char* data_json,
                            float x,
                            float y,
                            unsigned operations,
                            unsigned modifiers) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::DragState* state = StateOf(view);
  if (!state)
    return;
  state->Leave();
  state->target = zebkit::Target();
  state->target.data = zebkit::DropDataFromJson(data_json ? data_json : "");
  state->target.operations = zebkit::ToMask(operations);
  state->Enter(gfx::PointF(x, y), zebkit::BlinkModifiers(modifiers));
}

void zebkit_view_drag_over(zebkit_view* view,
                           float x,
                           float y,
                           unsigned operations,
                           unsigned modifiers,
                           zebkit_drag_operation_func func,
                           void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::DragState* state = StateOf(view);
  if (!state)
    return;
  state->target.operations = zebkit::ToMask(operations);
  state->Over(gfx::PointF(x, y), zebkit::BlinkModifiers(modifiers),
              zebkit::DragAnswer(view, func, data));
}

void zebkit_view_drag_leave(zebkit_view* view) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  if (zebkit::DragState* state = StateOf(view))
    state->Leave();
}

void zebkit_view_drop(zebkit_view* view,
                      float x,
                      float y,
                      unsigned modifiers,
                      zebkit_drag_operation_func func,
                      void* data) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::DragState* state = StateOf(view);
  if (!state)
    return;
  auto done = zebkit::DragAnswer(view, func, data);
  const gfx::PointF point(x, y);
  const int blink = zebkit::BlinkModifiers(modifiers);
  const std::vector<ui::FileInfo>& files = state->target.data.filenames;
  if (files.empty()) {
    state->Drop(point, blink, std::move(done));
    return;
  }
  // zebkit.pathForFile() knows the files' paths before the page gets them,
  // as for a drop on a window (view.cc).
  std::vector<base::FilePath> paths;
  for (const ui::FileInfo& file : files)
    paths.push_back(file.path);
  zebkit::TellDroppedPaths(
      view, paths,
      base::BindOnce(
          [](base::WeakPtr<zebkit_view> view, gfx::PointF point, int blink,
             base::OnceCallback<void(unsigned)> done) {
            zebkit::DragState* state = view ? StateOf(view.get()) : nullptr;
            if (!state) {
              std::move(done).Run(ZEBKIT_DRAG_NONE);
              return;
            }
            state->Drop(point, blink, std::move(done));
          },
          view->GetWeakPtr(), point, blink, std::move(done)));
}

void zebkit_view_drag_source_end(zebkit_view* view,
                                 float x,
                                 float y,
                                 unsigned operation) {
  ZEBKIT_LIVE_VIEW_OR_RETURN(view);
  zebkit::DragState* state = StateOf(view);
  if (!state || !state->source)
    return;
  if (state->source->engine_runs)
    state->Leave();
  state->EndSource(gfx::PointF(x, y), operation & ZEBKIT_DRAG_ALL);
}

}  // extern "C"
