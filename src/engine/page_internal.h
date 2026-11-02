// What a page asks of its window (<zebkit/page.h>): dialogs (dialog.cc),
// printing to PDF (print.cc, devtools.cc), drag and drop (drag.cc).

#ifndef ZEBKIT_ENGINE_PAGE_INTERNAL_H_
#define ZEBKIT_ENGINE_PAGE_INTERNAL_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted_memory.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "zebkit/engine/include/zebkit/offscreen.h"
#include "zebkit/engine/include/zebkit/page.h"

namespace content {
class DevToolsAgentHost;
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace zebkit {

class Window;

// The dialog manager of a window: the view's policy, the
// application's answers, CDP's (dialog.cc).
std::unique_ptr<content::JavaScriptDialogManager> CreateDialogManager(
    Window* window);

// window.print() in a page of @contents: a dialog of type print, reported
// (dialog.cc).
void ReportPrintDialog(content::WebContents* contents,
                       content::RenderFrameHost* frame);

// Automation's part in a page's dialogs (automation.cc): whether it holds
// them for its own answer (a Playwright dialog listener), and what it hears
// of them. One for the process.
class DialogObserver {
 public:
  virtual ~DialogObserver() = default;
  virtual bool HoldsDialogs(content::WebContents* contents) = 0;
  virtual void DialogOpened(content::WebContents* contents,
                            const zebkit_dialog& dialog) = 0;
  virtual void DialogClosed(content::WebContents* contents,
                            uint64_t id,
                            bool accepted,
                            const std::string& user_input) = 0;
};
void SetDialogObserver(DialogObserver* observer);

// Whether a DevTools client other than the engine's own sessions is
// attached to @contents' page (devtools.cc).
bool HasExternalDevToolsClient(content::WebContents* contents);

// The page of @contents printed to PDF with Page.printToPDF's parameters
// (print.cc): the bytes, or an empty pointer and the error, which is the
// caller's when @invalid_params (CDP's "invalid parameters").
using PdfCallback =
    base::OnceCallback<void(scoped_refptr<base::RefCountedMemory> pdf,
                            const std::string& error,
                            bool invalid_params)>;
void PrintToPdf(content::WebContents* contents,
                const std::string& options_json,
                PdfCallback callback);

// Drags (drag.cc): the hook that takes a page's drags, installed with the
// first view; an off-screen view's mouse and keys while the page drags,
// which return true when the drag took them.
void InstallDragHook();
bool DragTakesMouse(zebkit_view* view,
                    zebkit_mouse_type type,
                    float x,
                    float y,
                    unsigned modifiers);
bool DragTakesKey(zebkit_view* view, const zebkit_key_event& event);
// Where the mouse last was in an off-screen view, for where a drag starts.
void NoteMousePosition(zebkit_view* view, float x, float y);

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PAGE_INTERNAL_H_
