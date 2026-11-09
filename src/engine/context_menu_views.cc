// A page's context menu with Chromium's views menu (context_menu.h), on Linux
// and Windows: the menu Chrome shows there.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/widget/widget.h"
#include "zebkit/engine/context_menu.h"
#include "zebkit/engine/view.h"
#include "zebkit/engine/window.h"

namespace zebkit {

namespace {

// Owns itself while its menu runs.
class ContextMenuRun : public ui::SimpleMenuModel::Delegate {
 public:
  ContextMenuRun(std::vector<ContextMenuEntry> entries, base::OnceCallback<void(std::string)> chosen)
      : entries_(std::move(entries)), chosen_(std::move(chosen)), model_(this) {
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].separator)
        model_.AddSeparator(ui::NORMAL_SEPARATOR);
      else
        model_.AddItem(static_cast<int>(i), base::UTF8ToUTF16(entries_[i].title));
    }
  }

  void Run(views::Widget* widget, const gfx::Point& screen_point) {
    runner_ = std::make_unique<views::MenuRunner>(
        &model_, views::MenuRunner::CONTEXT_MENU | views::MenuRunner::HAS_MNEMONICS,
        base::BindRepeating(&ContextMenuRun::Closed, base::Unretained(this)));
    runner_->RunMenuAt(widget, nullptr, gfx::Rect(screen_point, gfx::Size()),
                       views::MenuAnchorPosition::kTopLeft, ui::mojom::MenuSourceType::kMouse);
  }

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdEnabled(int command_id) const override {
    return command_id >= 0 && static_cast<size_t>(command_id) < entries_.size() &&
           entries_[command_id].enabled;
  }
  void ExecuteCommand(int command_id, int event_flags) override {
    if (IsCommandIdEnabled(command_id))
      picked_ = entries_[command_id].id;
  }

 private:
  // After the menu's own work: its command, if any, is recorded by then.
  void Closed() {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(
                       [](std::unique_ptr<ContextMenuRun> self) {
                         if (self->chosen_)
                           std::move(self->chosen_).Run(self->picked_);
                       },
                       base::WrapUnique(this)));
  }

  std::vector<ContextMenuEntry> entries_;
  base::OnceCallback<void(std::string)> chosen_;
  ui::SimpleMenuModel model_;
  std::unique_ptr<views::MenuRunner> runner_;
  std::string picked_;
};

}  // namespace

bool PresentNativeMenu(zebkit_view* view, content::RenderFrameHost* frame, const gfx::Point& point,
                       std::vector<ContextMenuEntry> entries, base::OnceCallback<void(std::string)> chosen) {
  Window* window = view->window();
  views::Widget* widget = window ? views::Widget::GetWidgetForNativeWindow(window->native_window()) : nullptr;
  content::RenderWidgetHostView* host = frame->GetView();
  if (!widget || !host || entries.empty())
    return false;
  // The point is in the frame's widget; the menu's place on the screen.
  const gfx::Point screen = host->GetViewBounds().origin() + point.OffsetFromOrigin();
  auto* run = new ContextMenuRun(std::move(entries), std::move(chosen));
  run->Run(widget, screen);
  return true;
}

}  // namespace zebkit
