#include "zebkit/engine/desktop_actions.h"

#include <string.h>

#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "zebkit/engine/include/zebkit/zebkit.h"

namespace {

// What a page could run or carry inline is not a program's to open.
bool CheckUrl(const char* url, GURL* parsed, std::string* error) {
  *parsed = GURL(url ? url : "");
  if (!parsed->is_valid() || parsed->SchemeIs("javascript") || parsed->SchemeIs("data") ||
      parsed->SchemeIs("blob") || parsed->SchemeIs("about") || parsed->spec().size() > 65536) {
    *error = "a valid URL to open, not javascript:, data:, blob: or about:";
    return false;
  }
  return true;
}

bool CheckPath(const char* path, base::FilePath* file, std::string* error) {
  *file = base::FilePath::FromUTF8Unsafe(path ? path : "");
  if (!file->IsAbsolute() || file->ReferencesParent()) {
    *error = "an absolute path";
    return false;
  }
  return true;
}

// The file manager is asked once the file is known to be there, which is
// read off the engine's thread.
void ShowIfThere(const base::FilePath& file, zebkit::DesktopDone done) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&base::PathExists, file),
      base::BindOnce(
          [](base::FilePath file, zebkit::DesktopDone done, bool exists) {
            if (!exists) {
              std::move(done).Run(false, "no such file");
              return;
            }
            zebkit::ShowInFileManager(file, std::move(done));
          },
          file, std::move(done)));
}

zebkit::DesktopDone Tell(zebkit_desktop_done_func func, void* data) {
  return base::BindOnce(
      [](zebkit_desktop_done_func func, void* data, bool done, std::string error) {
        if (func)
          func(done, done ? nullptr : error.c_str(), data);
      },
      func, data);
}

// The asynchronous form's answer to a call refused at once: after it returns.
void TellLater(zebkit_desktop_done_func func, void* data, std::string error) {
  content::GetUIThreadTaskRunner({})->PostTask(FROM_HERE,
                                               base::BindOnce(Tell(func, data), false, std::move(error)));
}

bool Fail(char** error, const std::string& message) {
  if (error)
    *error = strdup(message.c_str());
  return false;
}

}  // namespace

extern "C" {

bool zebkit_open_external(const char* url, char** error) {
  if (error)
    *error = nullptr;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return Fail(error, "the engine's thread");
  GURL parsed;
  std::string message;
  if (!CheckUrl(url, &parsed, &message))
    return Fail(error, message);
  zebkit::OpenWithSystem(parsed, base::DoNothing());
  return true;
}

void zebkit_open_external_async(const char* url, zebkit_desktop_done_func func, void* data) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) {
    if (func)
      func(false, "zebkit_open_external_async is for the engine's thread", data);
    return;
  }
  GURL parsed;
  std::string message;
  if (!CheckUrl(url, &parsed, &message)) {
    TellLater(func, data, message);
    return;
  }
  zebkit::OpenWithSystem(parsed, Tell(func, data));
}

bool zebkit_show_in_folder(const char* path, char** error) {
  if (error)
    *error = nullptr;
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
    return Fail(error, "the engine's thread");
  base::FilePath file;
  std::string message;
  if (!CheckPath(path, &file, &message))
    return Fail(error, message);
  ShowIfThere(file, base::DoNothing());
  return true;
}

void zebkit_show_in_folder_async(const char* path, zebkit_desktop_done_func func, void* data) {
  if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI)) {
    if (func)
      func(false, "zebkit_show_in_folder_async is for the engine's thread", data);
    return;
  }
  base::FilePath file;
  std::string message;
  if (!CheckPath(path, &file, &message)) {
    TellLater(func, data, message);
    return;
  }
  ShowIfThere(file, Tell(func, data));
}

}  // extern "C"
