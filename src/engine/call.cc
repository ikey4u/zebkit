// zebkit.* calls from a page, over the engine's own scheme: the page's
// script POSTs a call to zebkit://call and reads the answer from the
// response, both as one frame of bytes (below). Bytes go through Mojo's
// data pipes as they are, not as text, and many calls are under way at
// once. The factory is the frame's, so the browser knows which frame (and
// which origin) a call comes from without asking the page.
//
// A frame, request and answer alike:
//   u32 little-endian: the length of the JSON that follows
//   JSON (UTF-8): the call or the answer, bytes in it as {"$part": [o, n]}
//   the bytes: each part n bytes at offset o from here

#ifdef UNSAFE_BUFFERS_BUILD
// Frames are read and written by offset.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>
#include <string.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/self_deleting.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "zebkit/engine/call.h"
#include "zebkit/engine/scheme.h"
#include "zebkit/engine/view.h"

namespace zebkit {

namespace {

constexpr char kHeaders[] =
    "Content-Type: application/octet-stream\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Cache-Control: no-store";

// {"$part": [offset, length]} in @value, from @parts, as binary values.
bool TakeParts(base::Value& value, std::string_view parts) {
  if (value.is_list()) {
    for (base::Value& item : value.GetList()) {
      if (!TakeParts(item, parts))
        return false;
    }
    return true;
  }
  if (!value.is_dict())
    return true;
  base::DictValue& dict = value.GetDict();
  if (dict.size() == 1) {
    if (const base::ListValue* part = dict.FindList("$part")) {
      if (part->size() != 2 || !(*part)[0].is_int() || !(*part)[1].is_int())
        return false;
      const int64_t offset = (*part)[0].GetInt();
      const int64_t length = (*part)[1].GetInt();
      if (offset < 0 || length < 0 ||
          offset + length > static_cast<int64_t>(parts.size())) {
        return false;
      }
      const auto* data = reinterpret_cast<const uint8_t*>(parts.data());
      value = base::Value(
          base::Value::BlobStorage(data + offset, data + offset + length));
      return true;
    }
  }
  for (auto [key, item] : dict) {
    if (!TakeParts(item, parts))
      return false;
  }
  return true;
}

// Binary values in @value to {"$part": [offset, length]}, their bytes
// appended to @parts.
base::Value GiveParts(base::Value value, std::string* parts) {
  if (value.is_blob()) {
    const base::Value::BlobStorage& blob = value.GetBlob();
    base::ListValue part;
    part.Append(static_cast<int>(parts->size()));
    part.Append(static_cast<int>(blob.size()));
    parts->append(reinterpret_cast<const char*>(blob.data()), blob.size());
    base::DictValue dict;
    dict.Set("$part", std::move(part));
    return base::Value(std::move(dict));
  }
  if (value.is_list()) {
    for (base::Value& item : value.GetList())
      item = GiveParts(std::move(item), parts);
    return value;
  }
  if (value.is_dict()) {
    for (auto [key, item] : value.GetDict())
      item = GiveParts(std::move(item), parts);
    return value;
  }
  return value;
}

std::string Frame(bool ok, base::Value value) {
  std::string parts;
  base::DictValue answer;
  answer.Set("ok", ok);
  answer.Set("value", GiveParts(std::move(value), &parts));
  const std::string json = base::WriteJson(answer).value_or("{}");
  const uint32_t length = static_cast<uint32_t>(json.size());
  std::string frame;
  frame.reserve(4 + json.size() + parts.size());
  for (int shift = 0; shift < 32; shift += 8)
    frame.push_back(static_cast<char>((length >> shift) & 0xff));
  frame += json;
  frame += parts;
  return frame;
}

class CallLoaderFactory : public network::SelfDeletingURLLoaderFactory {
 public:
  CallLoaderFactory(
      content::GlobalRenderFrameHostId frame,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
        frame_(frame) {}

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    auto fail = [&](const std::string& message) {
      RespondToLoader(std::move(loader), std::move(client), 200, kHeaders,
                      Frame(false, base::Value(message)));
    };
    if (request.method != "POST" || request.url.host() != "call") {
      RespondToLoader(std::move(loader), std::move(client), 404,
                      "Access-Control-Allow-Origin: *", std::string());
      return;
    }
    content::RenderFrameHost* frame = content::RenderFrameHost::FromID(frame_);
    zebkit_view* view =
        frame ? ViewOf(content::WebContents::FromRenderFrameHost(frame))
              : nullptr;
    if (!view) {
      fail("zebkit is not available in this frame");
      return;
    }
    std::string body;
    if (request.request_body) {
      for (const network::DataElement& element :
           *request.request_body->elements()) {
        if (element.type() != network::DataElement::Tag::kBytes) {
          fail("a call's body is bytes");
          return;
        }
        const auto bytes = element.As<network::DataElementBytes>().bytes();
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      }
    }
    if (body.size() < 4) {
      fail("not a call");
      return;
    }
    uint32_t length = 0;
    for (int i = 0; i < 4; ++i)
      length |= static_cast<uint32_t>(static_cast<uint8_t>(body[i])) << (8 * i);
    if (length > body.size() - 4) {
      fail("not a call");
      return;
    }
    const std::string_view json(body.data() + 4, length);
    const std::string_view parts(body.data() + 4 + length,
                                 body.size() - 4 - length);
    std::optional<base::Value> parsed =
        base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_dict()) {
      fail("not a call");
      return;
    }
    base::DictValue& call = parsed->GetDict();
    const std::string* module = call.FindString("module");
    const std::string* method = call.FindString("method");
    base::Value* args = call.Find("args");
    if (!module || !method || !args || !args->is_list() ||
        !TakeParts(*args, parts)) {
      fail("not a call");
      return;
    }
    PageCall page_call;
    page_call.view = view;
    page_call.module = *module;
    page_call.method = *method;
    page_call.args = std::move(args->GetList());
    page_call.reply = base::BindOnce(
        [](mojo::PendingReceiver<network::mojom::URLLoader> loader,
           mojo::PendingRemote<network::mojom::URLLoaderClient> client,
           bool ok, base::Value value) {
          RespondToLoader(std::move(loader), std::move(client), 200, kHeaders,
                          Frame(ok, std::move(value)));
        },
        std::move(loader), std::move(client));
    view->TakeCall(frame, std::move(page_call));
  }

 private:
  ~CallLoaderFactory() override = default;

  const content::GlobalRenderFrameHostId frame_;
};

}  // namespace

mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateCallLoaderFactory(
    int render_process_id,
    int render_frame_id) {
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  base::MakeSelfDeleting<CallLoaderFactory>(
      content::GlobalRenderFrameHostId(render_process_id, render_frame_id),
      remote.InitWithNewPipeAndPassReceiver());
  return remote;
}

}  // namespace zebkit
