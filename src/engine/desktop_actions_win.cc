// The system's handlers (desktop_actions.h), on Windows: ShellExecute, and
// the Explorer's selection.

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "zebkit/engine/desktop_actions.h"

namespace zebkit {

namespace {

// The shell's calls, which may load extensions and wait on them, on a COM
// thread of their own rather than the engine's, as Chrome's platform_util.
scoped_refptr<base::SingleThreadTaskRunner> ShellRunner() {
  static base::NoDestructor<scoped_refptr<base::SingleThreadTaskRunner>> runner(
      base::ThreadPool::CreateCOMSTATaskRunner({base::MayBlock(), base::TaskPriority::USER_BLOCKING}));
  return *runner;
}

void OnShellThread(base::OnceCallback<std::string()> work, DesktopDone done) {
  ShellRunner()->PostTaskAndReplyWithResult(
      FROM_HERE, std::move(work),
      base::BindOnce([](DesktopDone done, std::string error) { std::move(done).Run(error.empty(), error); },
                     std::move(done)));
}

}  // namespace

void OpenWithSystem(const GURL& url, DesktopDone done) {
  OnShellThread(base::BindOnce(
                    [](std::wstring target) -> std::string {
                      const auto result = reinterpret_cast<intptr_t>(
                          ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                      return result <= 32 ? "no program opens this URL" : std::string();
                    },
                    base::UTF8ToWide(url.spec())),
                std::move(done));
}

void ShowInFileManager(const base::FilePath& path, DesktopDone done) {
  OnShellThread(base::BindOnce(
                    [](base::FilePath path) -> std::string {
                      PIDLIST_ABSOLUTE item = ILCreateFromPathW(path.value().c_str());
                      if (!item)
                        return "no such file";
                      const HRESULT result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
                      ILFree(item);
                      return FAILED(result) ? "the Explorer did not show it" : std::string();
                    },
                    path),
                std::move(done));
}

}  // namespace zebkit
