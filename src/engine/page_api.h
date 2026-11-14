// The calls of a page's global zebkit that need no engine (page_api.cc,
// page_api_pdf.cc): what the engine's bridge (bridge.cc, pdf_bridge.cc) and
// the viewer of the system's web view (viewer.cc, docs/VIEWER.md) both
// answer a page with, so that a page sees the same zebkit on every system.
// Each takes the call's arguments as the page sent them (bytes as blobs,
// FromPageValue()) and gives its answer the same way.

#ifndef ZEBKIT_ENGINE_PAGE_API_H_
#define ZEBKIT_ENGINE_PAGE_API_H_

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "zebkit/engine/include/zebkit/pdf.h"

namespace zebkit {

// Bytes in a call: a binary value from the application's script, {$bytes}
// from a page, or a string, taken as its UTF-8.
std::optional<std::string> GetBytes(const base::Value* value);

// Bytes as {"$bytes": base64}, the form a page's JSON has them in, and back.
base::Value ToPageValue(base::Value value);
base::Value FromPageValue(base::Value value);

namespace page_api {

// An answer: the result, or when not ok the message.
struct Result {
  bool ok = false;
  base::Value value;
};

Result Ok(base::Value value);
Result Error(const std::string& message);

// The capability (ZEBKIT_ALLOW_*) a module of zebkit needs; 0 for none of
// zebkit's.
unsigned Needs(const std::string& module);

// zebkit.fs: under @root, and nowhere else. @data is writeFile's.
Result Files(const base::FilePath& root,
             const std::string& method,
             const std::string& path,
             std::optional<std::string> data,
             bool recursive);

// zebkit.compress and decompress ("compress"), crypto, image, canvas, text.
Result Compute(const std::string& module,
               const std::string& method,
               base::ListValue args);

// zebkit.url.parse.
Result UrlParse(const std::string& method, const base::ListValue& args);

// zebkit.pdf's calls on an open document, @other a second one a call names
// (importPages) or null: page_api_pdf.cc. Not open, create or close, whose
// documents are the caller's to keep.
Result PdfWork(zebkit_pdf* pdf,
               zebkit_pdf* other,
               const std::string& method,
               base::ListValue args);

// What a script's layers recorded on each document, and the hooks a script
// registered on each: PdfWork's, which the engine's bridge forgets with the
// document.
struct Recorded {
  int hook = 0;
  std::vector<std::string> operations;
};
std::map<zebkit_pdf*, Recorded>& RecordedOperations();
std::map<zebkit_pdf*, std::set<int>>& ScriptHooks();

}  // namespace page_api

}  // namespace zebkit

#endif  // ZEBKIT_ENGINE_PAGE_API_H_
