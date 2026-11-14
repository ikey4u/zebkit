// zebkit.pdf: <zebkit/pdf.h> for pages granted ZEBKIT_ALLOW_PDF and for the
// application's script (pdf_script.h makes the object). Documents are
// worked on in one sequence of the thread pool, in the browser process; a
// page uses the documents it opened, and they close with its view.

#include <stdlib.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/containers/circular_deque.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "zebkit/engine/include/zebkit/pdf.h"
#include "zebkit/engine/page_api.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

struct Document {
  zebkit_pdf* pdf = nullptr;
  // The view whose page opened it, null for the application's script, and
  // the page's origin.
  const zebkit_view* owner = nullptr;
  std::string origin;
  // The page's document that opened it, or the script; nothing else uses it.
  content::WeakDocumentPtr document;
  const void* script = nullptr;
};

std::map<int, Document>& Documents() {
  static base::NoDestructor<std::map<int, Document>> documents;
  return *documents;
}

// The application's scripts that have called zebkit.pdf and not ended, each
// with a number of its own: an open() that completes after its script ended
// (ClosePdfsOfScript()) closes what it opened, also when a later script has
// the same address.
std::map<const void*, uint64_t>& LiveScripts() {
  static base::NoDestructor<std::map<const void*, uint64_t>> scripts;
  return *scripts;
}
uint64_t ScriptNumber(const void* script) {
  static uint64_t next = 1;
  auto [it, added] = LiveScripts().try_emplace(script, next);
  if (added)
    ++next;
  return it->second;
}

// PDFium does one thing at a time: one sequence for all of it, which also
// closes a document only after what was asked of it before.
scoped_refptr<base::SequencedTaskRunner> PdfSequence() {
  static base::NoDestructor<scoped_refptr<base::SequencedTaskRunner>> runner(
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE}));
  return *runner;
}

using page_api::RecordedOperations;
using page_api::ScriptHooks;
using Answer = page_api::Result;

void Reply(CallReply reply, Answer answer) {
  std::move(reply).Run(answer.ok, std::move(answer.value));
}

// Opens on the PDF sequence: bytes, or for the application's script a
// path. The document's id is made back on the UI thread.
struct Opened {
  zebkit_pdf* pdf = nullptr;
  std::string error;
};

Opened Open(bool create,
            std::optional<std::string> bytes,
            std::string path,
            std::string password) {
  char* error = nullptr;
  const char* pass = password.empty() ? nullptr : password.c_str();
  zebkit_pdf* pdf =
      create  ? zebkit_pdf_new(&error)
      : bytes ? zebkit_pdf_open(bytes->data(), bytes->size(), pass, &error)
              : zebkit_pdf_open_file(path.c_str(), pass, &error);
  Opened opened;
  opened.pdf = pdf;
  if (!pdf)
    opened.error = error ? error : "the document cannot be opened";
  free(error);
  return opened;
}

// Calls on documents wait here, by how soon they are wanted, and the PDF
// sequence takes the soonest each time: a page to show first, then the
// rest, then what reads a whole document (all its annotations, a search),
// so that a reader's first page is not drawn after its document's links.
enum Urgency { kNow, kSoon, kLater, kUrgencies };

Urgency UrgencyOf(const std::string& method) {
  static constexpr const char* kNowMethods[] = {
      "render", "renderAnnotation", "renderAnnotations", "charAt",
      "textRects", "page"};
  static constexpr const char* kLaterMethods[] = {
      "annotationsOfPages", "pages", "search", "formFields", "save",
      "attachments"};
  for (const char* name : kNowMethods) {
    if (method == name)
      return kNow;
  }
  for (const char* name : kLaterMethods) {
    if (method == name)
      return kLater;
  }
  return kSoon;
}

// Calls that only read a document; any other may change it.
bool OnlyReads(const std::string& method) {
  static constexpr const char* kReads[] = {
      "annotationAction", "annotationAt", "annotationFile", "annotations",
      "annotationsOfPages", "attachment", "attachments", "charAt",
      "exportAnnotations", "formFields", "imageData", "images", "info", "links",
      "outline", "page", "pageData", "pages", "render", "renderAnnotation",
      "renderAnnotations", "save", "search", "signatures", "text", "textLayout",
      "textLines", "textRects"};
  for (const char* name : kReads) {
    if (method == name)
      return true;
  }
  return false;
}

// A call waiting, with the documents it uses: one that changes a document
// is never overtaken by a later call on it, nor overtakes an earlier one, so
// that apply(); render() renders the change and save(); apply() saves without.
struct PdfCall {
  uint64_t order = 0;
  std::vector<const zebkit_pdf*> documents;
  bool changes = false;
  base::OnceClosure work;
};

struct Waiting {
  base::Lock lock;
  uint64_t next = 0;
  std::array<base::circular_deque<PdfCall>, kUrgencies> calls;
};

Waiting& Calls() {
  static base::NoDestructor<Waiting> waiting;
  return *waiting;
}

// Whether @call has to wait for an earlier one on a document they share.
bool Held(const Waiting& waiting, const PdfCall& call) {
  for (const auto& calls : waiting.calls) {
    for (const PdfCall& other : calls) {
      if (other.order >= call.order || (!other.changes && !call.changes))
        continue;
      for (const zebkit_pdf* document : call.documents) {
        if (std::ranges::contains(other.documents, document))
          return true;
      }
    }
  }
  return false;
}

void RunSoonest() {
  base::OnceClosure work;
  {
    base::AutoLock lock(Calls().lock);
    // The soonest that nothing earlier holds back; the earliest of all never is.
    for (auto& calls : Calls().calls) {
      for (auto it = calls.begin(); it != calls.end(); ++it) {
        if (!Held(Calls(), *it)) {
          work = std::move(it->work);
          calls.erase(it);
          break;
        }
      }
      if (work)
        break;
    }
  }
  if (work)
    std::move(work).Run();
}

// One task on the sequence for each call waiting: each runs the soonest
// call there is when it runs. Calls of the same urgency keep their order.
void Enqueue(Urgency urgency,
             std::vector<const zebkit_pdf*> documents,
             bool changes,
             base::OnceClosure work) {
  {
    base::AutoLock lock(Calls().lock);
    Calls().calls[urgency].push_back(
        PdfCall{Calls().next++, std::move(documents), changes, std::move(work)});
  }
  PdfSequence()->PostTask(FROM_HERE, base::BindOnce(&RunSoonest));
}

// After every call already waiting for the document: last of all, and
// none can come for it after (it is no longer among Documents()).
void CloseOnSequence(zebkit_pdf* pdf) {
  Enqueue(kLater, {pdf}, /*changes=*/true, base::BindOnce(
                      [](zebkit_pdf* document) {
                        RecordedOperations().erase(document);
                        ScriptHooks().erase(document);
                        zebkit_pdf_close(document);
                      },
                      pdf));
}

}  // namespace

void CallPdf(PageCall call) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  if (call.method == "open" || call.method == "create") {
    const bool create = call.method == "create";
    const base::Value* source = call.args.empty() ? nullptr : &call.args[0];
    std::optional<std::string> bytes;
    std::string path;
    if (create) {
      // Nothing to read.
    } else if (source && source->is_string() && !call.view) {
      path = source->GetString();
    } else {
      // A page gives bytes; a string from a page is not a path.
      bytes = source && !source->is_string() ? GetBytes(source) : std::nullopt;
      if (!bytes) {
        std::move(call.reply).Run(false,
                                  base::Value("open() takes the PDF's bytes"));
        return;
      }
    }
    std::string password = call.args.size() > 1 && call.args[1].is_string()
                               ? call.args[1].GetString()
                               : std::string();
    base::WeakPtr<zebkit_view> view =
        call.view ? call.view->GetWeakPtr() : nullptr;
    const bool from_page = call.view != nullptr;
    const uint64_t script_number = call.script ? ScriptNumber(call.script) : 0;
    PdfSequence()->PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&Open, create, std::move(bytes), std::move(path),
                       std::move(password)),
        base::BindOnce(
            [](base::WeakPtr<zebkit_view> view, bool from_page,
               std::string origin, content::WeakDocumentPtr document,
               const void* script, uint64_t script_number, CallReply reply,
               Opened opened) {
              if (!opened.pdf) {
                std::move(reply).Run(false, base::Value(opened.error));
                return;
              }
              // The page's view, or its document, or the script, went while
              // it opened.
              auto live = script ? LiveScripts().find(script) : LiveScripts().end();
              if ((from_page && (!view || !document.AsRenderFrameHostIfValid())) ||
                  (script && (live == LiveScripts().end() || live->second != script_number))) {
                CloseOnSequence(opened.pdf);
                return;
              }
              static int next_id = 1;
              const int id = next_id++;
              Documents()[id] = {opened.pdf, view.get(), std::move(origin), document, script};
              std::move(reply).Run(true, base::Value(id));
            },
            std::move(view), from_page, call.origin, call.document, call.script,
            script_number, std::move(call.reply)));
    return;
  }

  // A page has the documents its view and origin opened, and no others.
  auto find = [&](const base::Value* id) -> zebkit_pdf* {
    auto document =
        id && id->is_int() ? Documents().find(id->GetInt()) : Documents().end();
    if (document == Documents().end() ||
        document->second.owner != call.view ||
        (call.view && document->second.origin != call.origin) ||
        document->second.script != call.script) {
      return nullptr;
    }
    if (call.view) {
      content::RenderFrameHost* opener = document->second.document.AsRenderFrameHostIfValid();
      if (!opener || opener != call.document.AsRenderFrameHostIfValid())
        return nullptr;
    }
    return document->second.pdf;
  };
  zebkit_pdf* pdf = find(call.args.empty() ? nullptr : &call.args[0]);
  if (!pdf) {
    std::move(call.reply).Run(false, base::Value("no such document"));
    return;
  }
  zebkit_pdf* other = nullptr;
  const bool names_another = call.method == "importPages" || call.method == "copyAnnotation" ||
                             call.method == "stampFromPage";
  if (names_another) {
    other = find(call.args.size() > 1 ? &call.args[1] : nullptr);
    if (!other) {
      std::move(call.reply).Run(false, base::Value("no such document"));
      return;
    }
  }
  if (call.method == "close") {
    Documents().erase(call.args[0].GetInt());
    CloseOnSequence(pdf);
    std::move(call.reply).Run(true, base::Value());
    return;
  }
  base::ListValue args;
  // A second argument that was the other document's id leaves a null.
  for (size_t i = other ? 2 : 1; i < call.args.size(); ++i)
    args.Append(call.args[i].Clone());
  if (other)
    args.Insert(args.begin(), base::Value());
  const Urgency urgency = UrgencyOf(call.method);
  std::vector<const zebkit_pdf*> documents = {pdf};
  if (other)
    documents.push_back(other);
  Enqueue(urgency, std::move(documents), !OnlyReads(call.method),
          base::BindOnce(
              [](zebkit_pdf* pdf, zebkit_pdf* other, std::string method,
                 base::ListValue args,
                 scoped_refptr<base::SequencedTaskRunner> caller,
                 CallReply reply) {
                caller->PostTask(
                    FROM_HERE,
                    base::BindOnce(&Reply, std::move(reply),
                                   page_api::PdfWork(pdf, other, method, std::move(args))));
              },
              pdf, other, call.method, std::move(args),
              base::SequencedTaskRunner::GetCurrentDefault(),
              std::move(call.reply)));
}

void ClosePdfsOfGoneDocuments(const zebkit_view* view,
                              const content::RenderFrameHost* frame) {
  for (auto it = Documents().begin(); it != Documents().end();) {
    // While RenderFrameDeleted(@frame) runs its document is still valid.
    content::RenderFrameHost* opener = it->second.document.AsRenderFrameHostIfValid();
    if (it->second.owner == view && (!opener || opener == frame)) {
      CloseOnSequence(it->second.pdf);
      it = Documents().erase(it);
    } else {
      ++it;
    }
  }
}

void ClosePdfsOfScript(const void* script) {
  LiveScripts().erase(script);
  for (auto it = Documents().begin(); it != Documents().end();) {
    if (!it->second.owner && it->second.script == script) {
      CloseOnSequence(it->second.pdf);
      it = Documents().erase(it);
    } else {
      ++it;
    }
  }
}

void ClosePdfsOf(const zebkit_view* view) {
  for (auto it = Documents().begin(); it != Documents().end();) {
    if (it->second.owner == view) {
      CloseOnSequence(it->second.pdf);
      it = Documents().erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace zebkit
