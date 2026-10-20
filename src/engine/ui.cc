// zebkit.ui's native side: Chromium's views, the toolkit Chrome draws its
// own interface with, built and changed by the application's script.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include "zebkit/engine/ui.h"

#include <stdlib.h>

#include <optional>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted_memory.h"
#include "base/memory/weak_ptr.h"
#include "base/task/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/ime/text_input_client.h"
#include "ui/base/models/image_model.h"
#include "ui/events/base_event_utils.h"
#include "ui/events/event.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/text_constants.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/image_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/controls/textfield/textfield_controller.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/layout/box_layout.h"
#include "ui/snapshot/snapshot.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/views/widget/widget_observer.h"
#include "url/gurl.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/services.h"

namespace zebkit {

namespace {

// A view of a fixed width or height, where the props give one; its own
// size otherwise.
template <typename Base>
class Sized : public Base {
 public:
  using Base::Base;

  void SetFixedSize(std::optional<int> width, std::optional<int> height) {
    width_ = width;
    height_ = height;
    this->PreferredSizeChanged();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available) const override {
    gfx::Size size = Base::CalculatePreferredSize(available);
    if (width_)
      size.set_width(*width_);
    if (height_)
      size.set_height(*height_);
    return size;
  }

 private:
  std::optional<int> width_;
  std::optional<int> height_;
};

// A container: a flexbox of one direction, as views' BoxLayout lays it out.
class Box : public Sized<views::View> {
 public:
  Box() {
    layout_ = SetLayoutManager(
        std::make_unique<views::BoxLayout>(views::BoxLayout::Orientation::kVertical));
  }
  views::BoxLayout* layout() { return layout_; }

 private:
  raw_ptr<views::BoxLayout> layout_;
};

// A button whose label the script styles.
class Button : public Sized<views::MdTextButton> {
 public:
  using Sized::Sized;
  views::Label* text_label() { return label(); }
};

std::optional<SkColor> ParseColor(const std::string& text) {
  if (text.size() < 4 || text[0] != '#')
    return std::nullopt;
  std::string hex = text.substr(1);
  if (hex.size() == 3) {
    hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
  }
  uint32_t value = 0;
  if ((hex.size() != 6 && hex.size() != 8) ||
      !base::HexStringToUInt(hex, &value)) {
    return std::nullopt;
  }
  if (hex.size() == 6)
    return SkColorSetRGB(value >> 16, (value >> 8) & 255, value & 255);
  return SkColorSetARGB(value & 255, value >> 24, (value >> 16) & 255,
                        (value >> 8) & 255);
}

// 8 for all sides, or [vertical, horizontal], or [top, right, bottom, left].
std::optional<gfx::Insets> ParseInsets(const base::Value* value) {
  if (!value)
    return std::nullopt;
  if (value->is_int() || value->is_double())
    return gfx::Insets(static_cast<int>(value->GetIfDouble().value_or(0)));
  if (!value->is_list())
    return std::nullopt;
  std::vector<int> sides;
  for (const base::Value& side : value->GetList())
    sides.push_back(static_cast<int>(side.GetIfDouble().value_or(0)));
  if (sides.size() == 2)
    return gfx::Insets::VH(sides[0], sides[1]);
  if (sides.size() == 4)
    return gfx::Insets::TLBR(sides[0], sides[3], sides[2], sides[1]);
  return std::nullopt;
}

std::optional<views::LayoutAlignment> ParseAlignment(const std::string& name) {
  if (name == "start")
    return views::LayoutAlignment::kStart;
  if (name == "center")
    return views::LayoutAlignment::kCenter;
  if (name == "end")
    return views::LayoutAlignment::kEnd;
  if (name == "stretch")
    return views::LayoutAlignment::kStretch;
  return std::nullopt;
}

}  // namespace

// A node of the tree: what the script knows by an id.
struct UiTree::Node : public views::TextfieldController,
                      public views::WidgetObserver {
  int id = 0;
  std::string type;
  // What its parent lays out, and where its children go (a window's root,
  // a scroll view's contents).
  raw_ptr<views::View> view = nullptr;
  raw_ptr<Box> container = nullptr;
  // The view while it is in no parent.
  std::unique_ptr<views::View> owned;
  int parent = 0;
  std::vector<int> children;
  // A window's widget and its delegate: the node owns them, and frees them
  // after the widget is destroyed, not during.
  // (Declared in this order so that the widget is destroyed first.)
  raw_ptr<views::Widget> widget = nullptr;
  std::unique_ptr<views::WidgetDelegate> delegate_owned;
  std::unique_ptr<views::Widget> widget_owned;
  raw_ptr<UiTree> tree = nullptr;

  // Decoration: rebuilt when any of it changes.
  std::optional<SkColor> background;
  std::optional<SkColor> border_color;
  int border_width = 0;
  float radius = 0;
  gfx::Insets padding;
  int flex = 0;

  // views::TextfieldController
  void ContentsChanged(views::Textfield* sender,
                       const std::u16string& contents) override {
    tree->events_.Run(id, "input", base::Value(base::UTF16ToUTF8(contents)));
  }
  bool HandleKeyEvent(views::Textfield* sender,
                      const ui::KeyEvent& event) override {
    if (event.type() == ui::EventType::kKeyPressed &&
        event.key_code() == ui::VKEY_RETURN) {
      tree->events_.Run(id, "submit",
                        base::Value(base::UTF16ToUTF8(sender->GetText())));
      return true;
    }
    return false;
  }

  // views::WidgetObserver
  void OnWidgetDestroying(views::Widget* destroying) override {
    destroying->RemoveObserver(this);
    widget = nullptr;
    // Its views go with it.
    view = nullptr;
    container = nullptr;
    tree->WindowClosed(id);
  }
};

UiTree::UiTree(EventSink events, base::RepeatingClosure windows_changed)
    : events_(std::move(events)),
      windows_changed_(std::move(windows_changed)) {}

UiTree::~UiTree() {
  // Windows close; their views go with them, and with them the views of
  // the nodes inside.
  for (auto& [id, node] : nodes_) {
    if (node->widget) {
      node->widget->RemoveObserver(node.get());
      node->widget->CloseNow();
      node->widget = nullptr;
    }
  }
}

UiTree::Node* UiTree::Find(int id) {
  auto it = nodes_.find(id);
  return it == nodes_.end() ? nullptr : it->second.get();
}

size_t UiTree::open_windows() const {
  size_t count = 0;
  for (const auto& [id, node] : nodes_)
    count += node->widget ? 1 : 0;
  return count;
}

std::string UiTree::Create(int id, const std::string& type) {
  if (id <= 0 || nodes_.contains(id))
    return "a node needs a new id";
  auto node = std::make_unique<Node>();
  node->id = id;
  node->type = type;
  node->tree = this;
  if (type == "window") {
    auto root = std::make_unique<Box>();
    // What a window holds fills it.
    root->layout()->SetDefaultFlex(1);
    node->container = root.get();
    auto delegate = std::make_unique<views::WidgetDelegate>();
    delegate->SetContentsView(std::move(root));
    delegate->SetHasWindowSizeControls(true);
    auto owned_widget = std::make_unique<views::Widget>();
    views::Widget* widget = owned_widget.get();
    views::Widget::InitParams params(
        views::Widget::InitParams::CLIENT_OWNS_WIDGET,
        views::Widget::InitParams::TYPE_WINDOW);
    params.bounds = gfx::Rect(800, 600);
    params.delegate = delegate.get();
    node->delegate_owned = std::move(delegate);
    node->widget_owned = std::move(owned_widget);
#if BUILDFLAG(IS_LINUX)
    params.wm_class_class = "zebkit";
    params.wm_class_name = "zebkit";
#endif
    widget->Init(std::move(params));
    widget->AddObserver(node.get());
    node->widget = widget;
    nodes_[id] = std::move(node);
    windows_changed_.Run();
    return "";
  }
  std::unique_ptr<views::View> view;
  if (type == "view") {
    auto box = std::make_unique<Box>();
    node->container = box.get();
    view = std::move(box);
  } else if (type == "text") {
    auto label = std::make_unique<Sized<views::Label>>();
    label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    view = std::move(label);
  } else if (type == "button") {
    view = std::make_unique<Button>(
        base::BindRepeating(
            [](base::WeakPtr<UiTree> tree, int id) {
              if (tree)
                tree->events_.Run(id, "click", base::Value());
            },
            weak_factory_.GetWeakPtr(), id),
        std::u16string());
  } else if (type == "input") {
    auto field = std::make_unique<Sized<views::Textfield>>();
    field->set_controller(node.get());
    view = std::move(field);
  } else if (type == "image") {
    view = std::make_unique<Sized<views::ImageView>>();
  } else if (type == "scroll") {
    auto scroll = std::make_unique<Sized<views::ScrollView>>();
    node->container = scroll->SetContents(std::make_unique<Box>());
    view = std::move(scroll);
  } else if (type == "web") {
    view = std::make_unique<Sized<views::WebView>>(GetBrowserContext());
  } else {
    return "unknown type \"" + type + "\"";
  }
  node->view = view.get();
  node->owned = std::move(view);
  nodes_[id] = std::move(node);
  return "";
}

std::string UiTree::Set(int id, const base::DictValue& props) {
  Node* node = Find(id);
  if (!node)
    return "no node " + base::NumberToString(id);
  views::View* view = node->widget ? node->container.get() : node->view.get();
  if (!view)
    return "";  // A closed window's.
  bool decoration = false;
  for (auto [key, value] : props) {
    const std::string* text = value.GetIfString();
    const std::optional<double> number = value.GetIfDouble();
    if (key == "text") {
      const std::u16string label = base::UTF8ToUTF16(text ? *text : "");
      if (node->type == "text")
        static_cast<views::Label*>(view)->SetText(label);
      else if (node->type == "button")
        static_cast<views::MdTextButton*>(view)->SetText(label);
    } else if (key == "value" && node->type == "input") {
      static_cast<views::Textfield*>(view)->SetText(
          base::UTF8ToUTF16(text ? *text : ""));
    } else if (key == "placeholder" && node->type == "input") {
      static_cast<views::Textfield*>(view)->SetPlaceholderText(
          base::UTF8ToUTF16(text ? *text : ""));
    } else if (key == "password" && node->type == "input") {
      static_cast<views::Textfield*>(view)->SetTextInputType(
          value.GetIfBool().value_or(false) ? ui::TEXT_INPUT_TYPE_PASSWORD
                                            : ui::TEXT_INPUT_TYPE_TEXT);
    } else if (key == "title" && node->widget) {
      node->widget->widget_delegate()->SetTitle(
          base::UTF8ToUTF16(text ? *text : ""));
    } else if ((key == "width" || key == "height") && node->widget) {
      gfx::Rect bounds = node->widget->GetWindowBoundsInScreen();
      if (key == "width")
        bounds.set_width(number.value_or(bounds.width()));
      else
        bounds.set_height(number.value_or(bounds.height()));
      node->widget->SetBounds(bounds);
    } else if (key == "size") {
      // [width, height], either null for the view's own.
      std::optional<int> width, height;
      if (value.is_list() && value.GetList().size() == 2) {
        if (auto w = value.GetList()[0].GetIfDouble())
          width = static_cast<int>(*w);
        if (auto h = value.GetList()[1].GetIfDouble())
          height = static_cast<int>(*h);
      }
      if (node->type == "view")
        static_cast<Box*>(view)->SetFixedSize(width, height);
      else if (node->type == "text")
        static_cast<Sized<views::Label>*>(view)->SetFixedSize(width, height);
      else if (node->type == "button")
        static_cast<Button*>(view)->SetFixedSize(width, height);
      else if (node->type == "input")
        static_cast<Sized<views::Textfield>*>(view)->SetFixedSize(width,
                                                                  height);
      else if (node->type == "image")
        static_cast<Sized<views::ImageView>*>(view)->SetFixedSize(width,
                                                                  height);
      else if (node->type == "scroll")
        static_cast<Sized<views::ScrollView>*>(view)->SetFixedSize(width,
                                                                   height);
      else if (node->type == "web")
        static_cast<Sized<views::WebView>*>(view)->SetFixedSize(width, height);
    } else if (key == "direction" && node->container) {
      node->container->layout()->SetOrientation(
          text && *text == "row" ? views::BoxLayout::Orientation::kHorizontal
                                 : views::BoxLayout::Orientation::kVertical);
    } else if (key == "gap" && node->container) {
      node->container->layout()->set_between_child_spacing(number.value_or(0));
    } else if (key == "align" && node->container && text) {
      if (auto alignment = ParseAlignment(*text))
        node->container->layout()->set_cross_axis_alignment(*alignment);
    } else if (key == "justify" && node->container && text) {
      if (auto alignment = ParseAlignment(*text))
        node->container->layout()->set_main_axis_alignment(*alignment);
    } else if (key == "padding") {
      if (std::optional<gfx::Insets> insets = ParseInsets(&value)) {
        if (node->container)
          node->container->layout()->set_inside_border_insets(*insets);
        else
          node->padding = *insets;
        decoration = true;
      }
    } else if (key == "grow") {
      node->flex = static_cast<int>(number.value_or(0));
      Node* parent = Find(node->parent);
      if (parent && parent->container)
        parent->container->layout()->SetFlexForView(node->view, node->flex);
    } else if (key == "background") {
      node->background = text ? ParseColor(*text) : std::nullopt;
      decoration = true;
    } else if (key == "radius") {
      node->radius = number.value_or(0);
      decoration = true;
    } else if (key == "border") {
      // {width, color}
      node->border_width = 0;
      node->border_color.reset();
      if (const base::DictValue* border = value.GetIfDict()) {
        node->border_width = border->FindDouble("width").value_or(1);
        if (const std::string* color = border->FindString("color"))
          node->border_color = ParseColor(*color);
      }
      decoration = true;
    } else if (key == "color" && text) {
      if (std::optional<SkColor> color = ParseColor(*text)) {
        if (node->type == "text")
          static_cast<views::Label*>(view)->SetEnabledColor(*color);
        else if (node->type == "button")
          static_cast<views::MdTextButton*>(view)->SetEnabledTextColors(*color);
        else if (node->type == "input")
          static_cast<views::Textfield*>(view)->SetColor(*color);
      }
    } else if (key == "fontSize" || key == "fontWeight") {
      views::Label* label = node->type == "text"
                                ? static_cast<views::Label*>(view)
                                : node->type == "button"
                                      ? static_cast<Button*>(view)->text_label()
                                      : nullptr;
      if (label) {
        gfx::FontList fonts = label->font_list();
        if (key == "fontSize" && number)
          fonts = fonts.DeriveWithSizeDelta(static_cast<int>(*number) -
                                            fonts.GetFontSize());
        if (key == "fontWeight") {
          int weight = number ? static_cast<int>(*number)
                              : (text && *text == "bold" ? 700 : 400);
          fonts = fonts.DeriveWithWeight(static_cast<gfx::Font::Weight>(weight));
        }
        label->SetFontList(fonts);
      }
    } else if (key == "textAlign" && node->type == "text" && text) {
      static_cast<views::Label*>(view)->SetHorizontalAlignment(
          *text == "center"  ? gfx::ALIGN_CENTER
          : *text == "right" ? gfx::ALIGN_RIGHT
                             : gfx::ALIGN_LEFT);
    } else if (key == "wrap" && node->type == "text") {
      static_cast<views::Label*>(view)->SetMultiLine(
          value.GetIfBool().value_or(false));
    } else if (key == "enabled") {
      view->SetEnabled(value.GetIfBool().value_or(true));
    } else if (key == "visible") {
      view->SetVisible(value.GetIfBool().value_or(true));
    } else if (key == "tooltip" && node->type == "button") {
      static_cast<views::Button*>(view)->SetTooltipText(
          base::UTF8ToUTF16(text ? *text : ""));
    } else if (key == "src" && node->type == "image") {
      // An image file's bytes: PNG, JPEG or WebP.
      if (value.is_blob()) {
        int width = 0, height = 0;
        uint8_t* rgba =
            zebkit_image_decode(value.GetBlob().data(), value.GetBlob().size(),
                                &width, &height, nullptr);
        if (rgba) {
          SkBitmap bitmap;
          bitmap.allocPixels(SkImageInfo::Make(width, height,
                                               kRGBA_8888_SkColorType,
                                               kUnpremul_SkAlphaType));
          memcpy(bitmap.getPixels(), rgba, static_cast<size_t>(width) * height * 4);
          free(rgba);
          bitmap.setImmutable();
          static_cast<views::ImageView*>(view)->SetImage(
              ui::ImageModel::FromImageSkia(
                  gfx::ImageSkia::CreateFrom1xBitmap(bitmap)));
        }
      }
    } else if (key == "url" && node->type == "web" && text) {
      views::WebView* web = static_cast<views::WebView*>(view);
      content::NavigationController::LoadURLParams params((GURL(*text)));
      web->GetWebContents()->GetController().LoadURLWithParams(params);
    } else if (key == "on") {
      // Events are always sent; the script decides which to listen to.
    } else {
      return "unknown property \"" + key + "\" of a " + node->type;
    }
  }
  if (decoration) {
    view->SetBackground(
        node->background
            ? (node->radius > 0
                   ? views::CreateRoundedRectBackground(*node->background,
                                                        node->radius,
                                                        node->border_width)
                   : views::CreateSolidBackground(*node->background))
            : nullptr);
    std::unique_ptr<views::Border> border;
    if (node->border_width > 0 && node->border_color) {
      border = views::CreateRoundedRectBorder(node->border_width, node->radius,
                                              *node->border_color);
    }
    if (!node->padding.IsEmpty()) {
      border = border ? views::CreatePaddedBorder(std::move(border),
                                                  node->padding)
                      : views::CreateEmptyBorder(node->padding);
    }
    view->SetBorder(std::move(border));
  }
  return "";
}

std::string UiTree::Append(int parent_id, int child_id, int before_id) {
  Node* parent = Find(parent_id);
  Node* child = Find(child_id);
  if (!parent || !child)
    return "no node " + base::NumberToString(parent ? child_id : parent_id);
  if (!parent->container)
    return "a " + parent->type + " has no children";
  if (child->widget)
    return "a window is in no parent";
  if (child->parent) {
    if (std::string error = Remove(child_id); !error.empty())
      return error;
  }
  if (!child->owned)
    return "";  // Its window has closed.
  auto& siblings = parent->children;
  size_t index = siblings.size();
  if (before_id) {
    auto it = std::find(siblings.begin(), siblings.end(), before_id);
    if (it == siblings.end())
      return "no child " + base::NumberToString(before_id) + " to insert before";
    index = it - siblings.begin();
  }
  parent->container->AddChildViewAt(std::move(child->owned), index);
  siblings.insert(siblings.begin() + index, child_id);
  child->parent = parent_id;
  if (child->flex)
    parent->container->layout()->SetFlexForView(child->view, child->flex);
  return "";
}

std::string UiTree::Remove(int id) {
  // What a reactive region removes may be gone already.
  Node* node = Find(id);
  if (!node)
    return "";
  Node* parent = Find(node->parent);
  if (!parent)
    return "";
  std::erase(parent->children, id);
  node->parent = 0;
  if (parent->container && node->view)
    node->owned = parent->container->RemoveChildViewT(node->view.get());
  return "";
}

void UiTree::Forget(int id) {
  Node* node = Find(id);
  if (!node)
    return;
  for (int child : std::vector<int>(node->children))
    Forget(child);
  if (node->type == "input" && node->view && !node->widget)
    static_cast<views::Textfield*>(node->view.get())->set_controller(nullptr);
  nodes_.erase(id);
}

std::string UiTree::Destroy(int id) {
  Node* node = Find(id);
  if (!node)
    return "";
  if (node->widget) {
    views::Widget* widget = node->widget;
    widget->RemoveObserver(node);
    node->widget = nullptr;
    widget->CloseNow();
    Forget(id);
    windows_changed_.Run();
    return "";
  }
  Remove(id);
  // Its view, and the views of what is inside it, go with what it owns.
  std::unique_ptr<views::View> owned = std::move(node->owned);
  Forget(id);
  return "";
}

void UiTree::WindowClosed(int id) {
  Node* node = Find(id);
  if (!node)
    return;
  // The window's views are gone with it, and so are the nodes inside.
  for (int child : std::vector<int>(node->children))
    Forget(child);
  node->children.clear();
  events_.Run(id, "close", base::Value());
  // The widget is in the middle of its destruction: freed after it.
  auto it = nodes_.find(id);
  if (it != nodes_.end()) {
    std::unique_ptr<Node> closed = std::move(it->second);
    nodes_.erase(it);
    base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(
        FROM_HERE, std::move(closed));
  }
  windows_changed_.Run();
}

std::string UiTree::Commit(const base::ListValue& operations) {
  for (const base::Value& operation : operations) {
    const base::ListValue* op = operation.GetIfList();
    if (!op || op->empty() || !(*op)[0].is_string())
      return "an operation is [name, ...]";
    const std::string& name = (*op)[0].GetString();
    auto arg = [&](size_t i) -> int {
      return i < op->size() ? static_cast<int>((*op)[i].GetIfDouble().value_or(0))
                            : 0;
    };
    std::string error;
    if (name == "create" && op->size() == 3 && (*op)[2].is_string()) {
      error = Create(arg(1), (*op)[2].GetString());
    } else if (name == "set" && op->size() == 3 && (*op)[2].is_dict()) {
      error = Set(arg(1), (*op)[2].GetDict());
    } else if (name == "append") {
      error = Append(arg(1), arg(2), arg(3));
    } else if (name == "remove") {
      error = Remove(arg(1));
    } else if (name == "destroy") {
      error = Destroy(arg(1));
    } else if (name == "show") {
      Node* node = Find(arg(1));
      if (!node || !node->widget)
        error = "no window " + base::NumberToString(arg(1));
      else
        node->widget->Show();
    } else if (name == "focus") {
      Node* node = Find(arg(1));
      if (node && node->view)
        node->view->RequestFocus();
    } else {
      error = "unknown operation \"" + name + "\"";
    }
    if (!error.empty())
      return error;
  }
  return "";
}

base::Value UiTree::Dump(int id) {
  Node* node = Find(id);
  if (!node)
    return base::Value();
  if (node->widget)
    node->widget->LayoutRootViewIfNecessary();
  views::View* view = node->widget ? node->container.get() : node->view.get();
  base::DictValue dump;
  dump.Set("id", id);
  dump.Set("type", node->type);
  if (view) {
    // In the window's content area (its root container), without the
    // frame views draws around it.
    gfx::Rect bounds = view->GetLocalBounds();
    Node* window = node;
    while (window->parent && Find(window->parent))
      window = Find(window->parent);
    if (window->widget && window->container && view->GetWidget())
      bounds = views::View::ConvertRectToTarget(view, window->container, bounds);
    base::ListValue rect;
    rect.Append(bounds.x());
    rect.Append(bounds.y());
    rect.Append(bounds.width());
    rect.Append(bounds.height());
    dump.Set("bounds", std::move(rect));
    dump.Set("visible", view->GetVisible());
    if (node->type == "text")
      dump.Set("text", base::UTF16ToUTF8(static_cast<views::Label*>(view)->GetText()));
    else if (node->type == "button")
      dump.Set("text", base::UTF16ToUTF8(
                           static_cast<views::LabelButton*>(view)->GetText()));
    else if (node->type == "input")
      dump.Set("value", base::UTF16ToUTF8(
                            static_cast<views::Textfield*>(view)->GetText()));
  }
  if (node->widget) {
    dump.Set("title",
             base::UTF16ToUTF8(node->widget->widget_delegate()->GetWindowTitle()));
  }
  base::ListValue children;
  for (int child : node->children)
    children.Append(Dump(child));
  dump.Set("children", std::move(children));
  return base::Value(std::move(dump));
}

void UiTree::Capture(
    int id,
    base::OnceCallback<void(std::vector<uint8_t> png)> done) {
  Node* node = Find(id);
  views::View* view =
      !node ? nullptr : node->widget ? node->container.get() : node->view.get();
  if (!view || !view->GetWidget() || view->size().IsEmpty()) {
    std::move(done).Run({});
    return;
  }
  view->GetWidget()->LayoutRootViewIfNecessary();
  // What the compositor shows: views paints some controls, buttons among
  // them, into layers of their own, which painting the tree would miss.
  ui::GrabWindowSnapshotAsPNG(
      view->GetWidget()->GetNativeWindow(),
      view->ConvertRectToWidget(view->GetLocalBounds()),
      base::BindOnce(
          [](base::OnceCallback<void(std::vector<uint8_t>)> done,
             scoped_refptr<base::RefCountedMemory> png) {
            std::vector<uint8_t> bytes;
            if (png)
              bytes.assign(png->data(), png->data() + png->size());
            std::move(done).Run(std::move(bytes));
          },
          std::move(done)));
}

std::string UiTree::Act(int id, const std::string& action,
                        const base::Value& value) {
  Node* node = Find(id);
  if (!node || !node->view)
    return "no node " + base::NumberToString(id);
  if (action == "click" && node->type == "button") {
    // As a mouse does it, so that what a disabled or hidden button does is
    // what the user would get.
    views::View* button = node->view;
    if (!button->GetEnabled() || !button->IsDrawn())
      return "the button cannot be clicked";
    const gfx::Point center = button->GetLocalBounds().CenterPoint();
    ui::MouseEvent press(ui::EventType::kMousePressed, center, center,
                         ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON,
                         ui::EF_LEFT_MOUSE_BUTTON);
    ui::MouseEvent release(ui::EventType::kMouseReleased, center, center,
                           ui::EventTimeForNow(), ui::EF_LEFT_MOUSE_BUTTON,
                           ui::EF_LEFT_MOUSE_BUTTON);
    button->OnMousePressed(press);
    button->OnMouseReleased(release);
    return "";
  }
  if (action == "type" && node->type == "input") {
    auto* field = static_cast<views::Textfield*>(node->view.get());
    const std::string* text = value.GetIfString();
    static_cast<ui::TextInputClient*>(field)->InsertText(
        base::UTF8ToUTF16(text ? *text : ""),
        ui::TextInputClient::InsertTextCursorBehavior::kMoveCursorAfterText);
    return "";
  }
  if (action == "submit" && node->type == "input") {
    auto* field = static_cast<views::Textfield*>(node->view.get());
    ui::KeyEvent enter(ui::EventType::kKeyPressed, ui::VKEY_RETURN,
                       ui::EF_NONE);
    // Through View, where it is public; the Textfield handles it.
    static_cast<views::View*>(field)->OnKeyPressed(enter);
    return "";
  }
  return "a " + node->type + " cannot " + action;
}

}  // namespace zebkit
