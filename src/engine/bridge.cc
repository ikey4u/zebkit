// What a page's zebkit.* calls do, once the browser has said which origin
// made them and what it may use: layer 2 of the API (services.h), and the
// application's own functions.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "zebkit/engine/path_utf8.h"
#include "zebkit/engine/include/zebkit/canvas.h"
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/include/zebkit/text.h"
#include "zebkit/engine/view.h"

namespace zebkit {

// Bytes: a binary value from the application's script, {$bytes} from a
// page, or a string, taken as its UTF-8.
std::optional<std::string> GetBytes(const base::Value* value) {
  if (!value)
    return std::nullopt;
  if (value->is_string())
    return value->GetString();
  if (value->is_blob())
    return std::string(value->GetBlob().begin(), value->GetBlob().end());
  if (value->is_dict()) {
    const std::string* encoded = value->GetDict().FindString("$bytes");
    std::string decoded;
    if (encoded && base::Base64Decode(*encoded, &decoded))
      return decoded;
  }
  return std::nullopt;
}


PageCall::PageCall() = default;
PageCall::PageCall(PageCall&&) = default;
PageCall& PageCall::operator=(PageCall&&) = default;
PageCall::~PageCall() = default;

namespace {

constexpr size_t kMaxFileSize = 256u << 20;

struct Result {
  bool ok = false;
  base::Value value;  // the result, or the message when not ok
};

Result Ok(base::Value value) {
  return {true, std::move(value)};
}

Result Error(const std::string& message) {
  return {false, base::Value(message)};
}

base::Value Bytes(const uint8_t* data, size_t size) {
  return base::Value(base::Value::BlobStorage(data, data + size));
}

base::Value Bytes(const std::string& data) {
  return Bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}


const base::Value* Arg(const PageCall& call, size_t index) {
  return index < call.args.size() ? &call.args[index] : nullptr;
}

std::string StringArg(const PageCall& call, size_t index) {
  const base::Value* value = Arg(call, index);
  return value && value->is_string() ? value->GetString() : std::string();
}

// Takes a string the engine returned (zebkit_free) into a std::string.
std::string Take(char* text) {
  std::string out = text ? text : "";
  free(text);
  return out;
}

void Reply(CallReply reply, Result result) {
  std::move(reply).Run(result.ok, std::move(result.value));
}

// ---- zebkit.fs: under the granted directory, and nowhere else ----

// @path inside @root, or nothing when it would leave it, by "..", by an
// absolute path, or through a symbolic link that points out.
std::optional<base::FilePath> Inside(const base::FilePath& root,
                                     const std::string& path) {
  if (root.empty())
    return std::nullopt;
#if BUILDFLAG(IS_WIN)
  // The whole file system (zebkit_script's), which has no one root here.
  if (root.value() == FILE_PATH_LITERAL("/")) {
    const base::FilePath absolute = PathFromUTF8(path);
    return absolute.IsAbsolute() && !absolute.ReferencesParent()
               ? std::optional<base::FilePath>(absolute)
               : std::nullopt;
  }
#endif
  std::string relative = path;
  while (!relative.empty() && relative[0] == '/')
    relative.erase(0, 1);
  base::FilePath inside = PathFromUTF8(relative);
  if (inside.ReferencesParent())
    return std::nullopt;
  const base::FilePath full = relative.empty() ? root : root.Append(inside);
  // A link could point anywhere, even when it points nowhere yet.
  if (base::IsLink(full))
    return std::nullopt;
  base::FilePath existing = full;
  while (!base::PathExists(existing) && existing != existing.DirName())
    existing = existing.DirName();
  const base::FilePath real = base::MakeAbsoluteFilePath(existing);
  if (real.empty() || (real != root && !root.IsParent(real)))
    return std::nullopt;
  return full;
}

// The grant's directory with its links resolved, here on the thread pool
// rather than on the UI thread when it is granted. Inside() compares real
// paths against it.
base::FilePath ResolvedRoot(const base::FilePath& given) {
  if (given.empty())
    return base::FilePath();
#if BUILDFLAG(IS_WIN)
  if (given.value() == FILE_PATH_LITERAL("/"))
    return given;
#endif
  return base::MakeAbsoluteFilePath(given);
}

Result Files(const base::FilePath& given_root,
             const std::string& method,
             const std::string& path,
             std::optional<std::string> data,
             bool recursive) {
  const base::FilePath root = ResolvedRoot(given_root);
  std::optional<base::FilePath> file = Inside(root, path);
  if (!file)
    return Error("not a path inside the directory zebkit.fs is given: " + path);
  if (method == "readFile" || method == "readText") {
    std::optional<int64_t> size = base::GetFileSize(*file);
    if (!size)
      return Error("cannot read " + path);
    if (static_cast<uint64_t>(*size) > kMaxFileSize)
      return Error(path + " is larger than 256 MiB");
    std::string contents;
    if (!base::ReadFileToString(*file, &contents))
      return Error("cannot read " + path);
    if (method == "readFile")
      return Ok(Bytes(contents));
    if (base::IsStringUTF8(contents))
      return Ok(base::Value(contents));
    // Invalid bytes become U+FFFD, as TextDecoder makes them.
    std::u16string wide;
    base::UTF8ToUTF16(contents.data(), contents.size(), &wide);
    return Ok(base::Value(base::UTF16ToUTF8(wide)));
  }
  if (method == "writeFile") {
    if (!data)
      return Error("writeFile needs a string or bytes");
    if (!base::WriteFile(*file, *data))
      return Error("cannot write " + path);
    return Ok(base::Value());
  }
  if (method == "readDir") {
    base::ListValue entries;
    base::FileEnumerator enumerator(
        *file, false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    for (base::FilePath name = enumerator.Next(); !name.empty();
         name = enumerator.Next()) {
      base::FileEnumerator::FileInfo info = enumerator.GetInfo();
      base::DictValue entry;
      entry.Set("name", PathToUTF8(name.BaseName()));
      entry.Set("directory", info.IsDirectory());
      entry.Set("size", static_cast<double>(info.GetSize()));
      entries.Append(std::move(entry));
    }
    return Ok(base::Value(std::move(entries)));
  }
  if (method == "stat") {
    base::File::Info info;
    if (!base::GetFileInfo(*file, &info))
      return Error("no such file or directory: " + path);
    base::DictValue stat;
    stat.Set("size", static_cast<double>(info.size));
    stat.Set("directory", info.is_directory);
    stat.Set("file", !info.is_directory && !info.is_symbolic_link);
    stat.Set("modified", info.last_modified.InMillisecondsFSinceUnixEpoch());
    return Ok(base::Value(std::move(stat)));
  }
  if (method == "mkdir") {
    if (!base::CreateDirectory(*file))
      return Error("cannot create " + path);
    return Ok(base::Value());
  }
  if (method == "remove") {
    if (*file == root)
      return Error("the directory zebkit.fs is given cannot be removed");
    const bool removed = recursive ? base::DeletePathRecursively(*file)
                                   : base::DeleteFile(*file);
    if (!removed)
      return Error("cannot remove " + path);
    return Ok(base::Value());
  }
  return Error("zebkit.fs has no " + method);
}

// ---- zebkit.compress, zebkit.crypto, zebkit.image ----

std::optional<zebkit_compression> Compression(const std::string& name) {
  if (name == "gzip")
    return ZEBKIT_COMPRESS_GZIP;
  if (name == "zlib" || name == "deflate")
    return ZEBKIT_COMPRESS_ZLIB;
  if (name == "deflate-raw")
    return ZEBKIT_COMPRESS_DEFLATE_RAW;
  if (name == "br" || name == "brotli")
    return ZEBKIT_COMPRESS_BROTLI;
  if (name == "zstd")
    return ZEBKIT_COMPRESS_ZSTD;
  return std::nullopt;
}

std::optional<zebkit_digest> DigestNamed(const std::string& name) {
  if (name == "SHA-1")
    return ZEBKIT_DIGEST_SHA1;
  if (name == "SHA-256")
    return ZEBKIT_DIGEST_SHA256;
  if (name == "SHA-384")
    return ZEBKIT_DIGEST_SHA384;
  if (name == "SHA-512")
    return ZEBKIT_DIGEST_SHA512;
  return std::nullopt;
}

Result Compute(const std::string& module,
               const std::string& method,
               base::ListValue args) {
  auto arg = [&](size_t i) -> const base::Value* {
    return i < args.size() ? &args[i] : nullptr;
  };
  auto text = [&](size_t i) {
    const base::Value* v = arg(i);
    return v && v->is_string() ? v->GetString() : std::string();
  };
  char* error = nullptr;
  auto failed = [&](const std::string& fallback) {
    std::string message = error ? error : fallback;
    free(error);
    return Error(message);
  };
  if (module == "compress") {
    std::optional<zebkit_compression> format = Compression(text(0));
    std::optional<std::string> data = GetBytes(arg(1));
    if (!format)
      return Error("unknown format \"" + text(0) +
                   "\": gzip, zlib, deflate-raw, br or zstd");
    if (!data)
      return Error("no bytes to " + method);
    size_t size = 0;
    uint8_t* out = nullptr;
    if (method == "compress") {
      const base::Value* level = arg(2);
      out = zebkit_compress(*format, data->data(), data->size(),
                            level && level->is_int() ? level->GetInt() : -1,
                            &size, &error);
    } else {
      out = zebkit_decompress(*format, data->data(), data->size(), 0, &size,
                              &error);
    }
    if (!out)
      return failed(method + " failed");
    base::Value bytes = Bytes(out, size);
    free(out);
    return Ok(std::move(bytes));
  }
  if (module == "crypto") {
    if (method == "randomBytes") {
      const base::Value* count = arg(0);
      if (!count || !count->is_int() || count->GetInt() < 0 ||
          count->GetInt() > 65536) {
        return Error("randomBytes takes a count from 0 to 65536");
      }
      std::string out(count->GetInt(), '\0');
      zebkit_random_bytes(out.data(), out.size());
      return Ok(Bytes(out));
    }
    // AES-GCM: (key, nonce, plaintext or sealed, additional data or null);
    // sealing gives the ciphertext and its tag, opening the plaintext.
    if (method == "aesGcmSeal" || method == "aesGcmOpen") {
      std::optional<std::string> key = GetBytes(arg(0));
      std::optional<std::string> nonce = GetBytes(arg(1));
      std::optional<std::string> data = GetBytes(arg(2));
      std::optional<std::string> ad = arg(3) && !arg(3)->is_none() ? GetBytes(arg(3)) : std::string();
      if (!key || !nonce || !data || !ad)
        return Error(method + " takes a key, a nonce, bytes and additional data or null");
      size_t size = 0;
      auto* fn = method == "aesGcmSeal" ? &zebkit_aes_gcm_seal : &zebkit_aes_gcm_open;
      uint8_t* out = fn(key->data(), key->size(), nonce->data(), nonce->size(), ad->data(), ad->size(),
                        data->data(), data->size(), &size, &error);
      if (!out)
        return failed(method == "aesGcmSeal" ? "sealing failed" : "the data does not open");
      base::Value bytes = Bytes(out, size);
      free(out);
      return Ok(std::move(bytes));
    }
    std::optional<zebkit_digest> digest = DigestNamed(text(0));
    if (!digest)
      return Error("unknown digest \"" + text(0) +
                   "\": SHA-1, SHA-256, SHA-384 or SHA-512");
    std::string out(zebkit_digest_size(*digest), '\0');
    uint8_t* into = reinterpret_cast<uint8_t*>(out.data());
    if (method == "digest") {
      std::optional<std::string> data = GetBytes(arg(1));
      if (!data || !zebkit_digest_compute(*digest, data->data(), data->size(),
                                          into)) {
        return Error("digest needs bytes or a string");
      }
      return Ok(Bytes(out));
    }
    if (method == "hmac") {
      std::optional<std::string> key = GetBytes(arg(1));
      std::optional<std::string> data = GetBytes(arg(2));
      if (!key || !data ||
          !zebkit_hmac(*digest, key->data(), key->size(), data->data(),
                       data->size(), into)) {
        return Error("hmac needs a key and data");
      }
      return Ok(Bytes(out));
    }
    return Error("zebkit.crypto has no " + method);
  }
  if (module == "image") {
    if (method == "decode") {
      std::optional<std::string> data = GetBytes(arg(0));
      if (!data)
        return Error("decode needs bytes");
      int width = 0, height = 0;
      uint8_t* pixels = zebkit_image_decode(data->data(), data->size(),
                                            &width, &height, &error);
      if (!pixels)
        return failed("decoding failed");
      base::DictValue image;
      image.Set("width", width);
      image.Set("height", height);
      image.Set("data",
                Bytes(pixels, static_cast<size_t>(width) * height * 4));
      free(pixels);
      return Ok(base::Value(std::move(image)));
    }
    if (method == "encode") {
      const std::string name = text(0);
      zebkit_image_format format;
      if (name == "png")
        format = ZEBKIT_IMAGE_PNG;
      else if (name == "jpeg")
        format = ZEBKIT_IMAGE_JPEG;
      else if (name == "webp")
        format = ZEBKIT_IMAGE_WEBP;
      else
        return Error("unknown format \"" + name + "\": png, jpeg or webp");
      const base::Value* image = arg(1);
      if (!image || !image->is_dict())
        return Error("encode needs {width, height, data}");
      std::optional<int> width = image->GetDict().FindInt("width");
      std::optional<int> height = image->GetDict().FindInt("height");
      std::optional<std::string> pixels =
          GetBytes(image->GetDict().Find("data"));
      if (!width || !height || !pixels ||
          pixels->size() != static_cast<size_t>(*width) * *height * 4) {
        return Error("encode needs width * height * 4 bytes of RGBA");
      }
      const base::Value* quality = arg(2);
      size_t size = 0;
      uint8_t* out = zebkit_image_encode(
          format, reinterpret_cast<const uint8_t*>(pixels->data()), *width,
          *height, quality && quality->is_int() ? quality->GetInt() : -1,
          &size, &error);
      if (!out)
        return failed("encoding failed");
      base::Value bytes = Bytes(out, size);
      free(out);
      return Ok(std::move(bytes));
    }
    return Error("zebkit.image has no " + method);
  }
  if (module == "canvas") {
    // render: (width, height, operations, "png" | "pixels", R, G, B, A pixels
    // to draw over or null); measure: (width, height, operations, text), the
    // advance of the text in the font the operations set.
    if (method != "render" && method != "measure")
      return Error("zebkit.canvas has no " + method);
    const base::Value* w = arg(0);
    const base::Value* h = arg(1);
    if (!w || !h || !w->is_int() || !h->is_int())
      return Error("a canvas has a width and a height");
    std::optional<std::string> base;
    if (method == "render" && arg(4) && !arg(4)->is_none()) {
      base = GetBytes(arg(4));
      if (!base || base->size() != static_cast<size_t>(w->GetInt()) * h->GetInt() * 4)
        return Error("the pixels to draw over are width * height * 4 bytes");
    }
    zebkit_canvas* canvas =
        base ? zebkit_canvas_wrap(reinterpret_cast<uint8_t*>(base->data()), w->GetInt(), h->GetInt(), 0,
                                  ZEBKIT_CANVAS_UNPREMULTIPLIED, &error)
             : zebkit_canvas_new(w->GetInt(), h->GetInt(), &error);
    if (!canvas)
      return failed("the canvas cannot be made");
    const base::Value* ops = arg(2);
    const std::string json = ops ? base::WriteJson(*ops).value_or("[]") : "[]";
    if (!zebkit_canvas_run(canvas, json.c_str(), &error)) {
      zebkit_canvas_close(canvas);
      return failed("the drawing failed");
    }
    base::Value answer;
    if (method == "measure") {
      answer = base::Value(zebkit_canvas_measure_text(canvas, text(3).c_str()));
    } else if (text(3) == "png") {
      size_t size = 0;
      uint8_t* out = zebkit_canvas_png(canvas, &size, &error);
      if (out) {
        answer = Bytes(out, size);
        free(out);
      }
    } else if (uint8_t* out = zebkit_canvas_pixels(canvas, &error)) {
      base::DictValue image;
      image.Set("width", w->GetInt());
      image.Set("height", h->GetInt());
      image.Set("data", Bytes(out, static_cast<size_t>(w->GetInt()) * h->GetInt() * 4));
      free(out);
      answer = base::Value(std::move(image));
    }
    zebkit_canvas_close(canvas);
    if (answer.is_none())
      return failed("the canvas cannot be read");
    return Ok(std::move(answer));
  }
  if (module == "text") {
    // (font bytes, size, text, {language, direction})
    if (method != "shape")
      return Error("zebkit.text has no " + method);
    std::optional<std::string> font = GetBytes(arg(0));
    const base::Value* size = arg(1);
    if (!font || !size || (!size->is_int() && !size->is_double()))
      return Error("shape takes a font's bytes, a size and text");
    const base::Value* options = arg(3);
    const std::string* language =
        options && options->is_dict() ? options->GetDict().FindString("language") : nullptr;
    const std::string* direction =
        options && options->is_dict() ? options->GetDict().FindString("direction") : nullptr;
    char* json = zebkit_text_shape(font->data(), font->size(), size->GetDouble(), text(2).c_str(),
                                   language ? language->c_str() : nullptr,
                                   direction ? direction->c_str() : nullptr, &error);
    if (!json)
      return failed("the text cannot be shaped");
    std::optional<base::Value> parsed = base::JSONReader::Read(json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
    free(json);
    if (!parsed)
      return Error("an answer that is not JSON");
    return Ok(std::move(*parsed));
  }
  return Error("zebkit has no " + module);
}

// ---- zebkit.url ----

Result Url(const PageCall& call) {
  if (call.method == "parse") {
    const std::string url = StringArg(call, 0);
    const base::Value* base_arg = Arg(call, 1);
    const char* base = base_arg && base_arg->is_string()
                           ? base_arg->GetString().c_str()
                           : nullptr;
    char* spec = zebkit_url_get(url.c_str(), base, ZEBKIT_URL_SPEC);
    if (!spec)
      return Ok(base::Value());
    free(spec);
    auto part = [&](zebkit_url_part p) {
      return Take(zebkit_url_get(url.c_str(), base, p));
    };
    base::DictValue parsed;
    parsed.Set("href", part(ZEBKIT_URL_SPEC));
    parsed.Set("protocol", part(ZEBKIT_URL_SCHEME) + ":");
    parsed.Set("username", part(ZEBKIT_URL_USERNAME));
    parsed.Set("password", part(ZEBKIT_URL_PASSWORD));
    parsed.Set("hostname", part(ZEBKIT_URL_HOST));
    parsed.Set("port", part(ZEBKIT_URL_PORT));
    parsed.Set("pathname", part(ZEBKIT_URL_PATH));
    const std::string query = part(ZEBKIT_URL_QUERY);
    const std::string fragment = part(ZEBKIT_URL_FRAGMENT);
    parsed.Set("search", query.empty() ? "" : "?" + query);
    parsed.Set("hash", fragment.empty() ? "" : "#" + fragment);
    parsed.Set("origin", part(ZEBKIT_URL_ORIGIN));
    return Ok(base::Value(std::move(parsed)));
  }
  if (call.method == "registrableDomain") {
    char* domain = zebkit_url_registrable_domain(StringArg(call, 0).c_str());
    if (!domain)
      return Ok(base::Value());
    return Ok(base::Value(Take(domain)));
  }
  return Error("zebkit.url has no " + call.method);
}

// ---- zebkit.fetch ----

struct FetchCall {
  CallReply reply;
};

void FetchDone(const zebkit_response* response, void* data) {
  std::unique_ptr<FetchCall> call(static_cast<FetchCall*>(data));
  if (response->error) {
    Reply(std::move(call->reply), Error(response->error));
    return;
  }
  base::DictValue headers;
  std::string_view lines(response->headers ? response->headers : "");
  while (!lines.empty()) {
    size_t end = lines.find("\r\n");
    std::string_view line = lines.substr(0, end);
    lines = end == std::string_view::npos ? std::string_view()
                                          : lines.substr(end + 2);
    const size_t colon = line.find(": ");
    if (colon == std::string_view::npos)
      continue;
    std::string name = base::ToLowerASCII(line.substr(0, colon));
    std::string value(line.substr(colon + 2));
    if (const std::string* before = headers.FindString(name))
      value = *before + ", " + value;
    headers.Set(name, value);
  }
  base::DictValue result;
  result.Set("status", response->status);
  result.Set("url", response->url ? response->url : "");
  result.Set("headers", std::move(headers));
  result.Set("body", Bytes(response->body, response->body_size));
  Reply(std::move(call->reply), Ok(base::Value(std::move(result))));
}

void Fetch(PageCall call) {
  const std::string url = StringArg(call, 0);
  std::string method = "GET";
  std::string headers;
  std::optional<std::string> body;
  if (const base::Value* options = Arg(call, 1); options && options->is_dict()) {
    if (const std::string* m = options->GetDict().FindString("method"))
      method = *m;
    if (const base::DictValue* h = options->GetDict().FindDict("headers")) {
      for (auto [name, value] : *h) {
        if (!value.is_string())
          continue;
        if (!headers.empty())
          headers += "\r\n";
        headers += name + ": " + value.GetString();
      }
    }
    body = GetBytes(options->GetDict().Find("body"));
  }
  auto* pending = new FetchCall{std::move(call.reply)};
  zebkit_fetch(method.c_str(), url.c_str(),
               headers.empty() ? nullptr : headers.c_str(),
               body ? body->data() : nullptr, body ? body->size() : 0,
               &FetchDone, pending);
}

unsigned Needs(const std::string& module) {
  if (module == "app")
    return ZEBKIT_ALLOW_APP;
  if (module == "fetch")
    return ZEBKIT_ALLOW_FETCH;
  if (module == "fs")
    return ZEBKIT_ALLOW_FILES;
  if (module == "compress")
    return ZEBKIT_ALLOW_COMPRESS;
  if (module == "crypto")
    return ZEBKIT_ALLOW_CRYPTO;
  if (module == "image")
    return ZEBKIT_ALLOW_IMAGE;
  if (module == "url")
    return ZEBKIT_ALLOW_URL;
  if (module == "pdf")
    return ZEBKIT_ALLOW_PDF;
  // Drawing and shaping reach nothing outside what they are given.
  if (module == "canvas" || module == "text")
    return ZEBKIT_ALLOW_IMAGE;
  return 0;
}

}  // namespace

base::Value ToPageValue(base::Value value) {
  if (value.is_blob()) {
    base::DictValue bytes;
    bytes.Set("$bytes", base::Base64Encode(value.GetBlob()));
    return base::Value(std::move(bytes));
  }
  if (value.is_list()) {
    base::ListValue list;
    for (base::Value& item : value.GetList())
      list.Append(ToPageValue(std::move(item)));
    return base::Value(std::move(list));
  }
  if (value.is_dict()) {
    base::DictValue dict;
    for (auto [key, item] : value.GetDict())
      dict.Set(key, ToPageValue(std::move(item)));
    return base::Value(std::move(dict));
  }
  return value;
}

base::Value FromPageValue(base::Value value) {
  if (value.is_list()) {
    for (base::Value& item : value.GetList())
      item = FromPageValue(std::move(item));
    return value;
  }
  if (!value.is_dict())
    return value;
  base::DictValue& dict = value.GetDict();
  if (dict.size() == 1) {
    const std::string* encoded = dict.FindString("$bytes");
    std::string decoded;
    if (encoded && base::Base64Decode(*encoded, &decoded)) {
      return base::Value(base::Value::BlobStorage(decoded.begin(),
                                                  decoded.end()));
    }
  }
  for (auto [key, item] : dict)
    item = FromPageValue(std::move(item));
  return value;
}

void HandlePageCall(PageCall call) {
  const unsigned needs = Needs(call.module);
  if (!needs) {
    Reply(std::move(call.reply), Error("zebkit has no " + call.module));
    return;
  }
  if (!(call.allow & needs)) {
    Reply(std::move(call.reply),
          Error("zebkit." + call.module + " is not allowed for " +
                call.origin));
    return;
  }
  if (call.module == "app") {
    if (!call.view) {
      Reply(std::move(call.reply), Error("zebkit.app is for pages"));
      return;
    }
    call.view->CallApp(std::move(call));
    return;
  }
  if (call.module == "fetch") {
    Fetch(std::move(call));
    return;
  }
  if (call.module == "url") {
    Result result = Url(call);
    Reply(std::move(call.reply), std::move(result));
    return;
  }
  if (call.module == "pdf") {
    CallPdf(std::move(call));
    return;
  }
  // The rest reads files or does work that takes time: off the UI thread.
  CallReply reply = std::move(call.reply);
  base::OnceCallback<Result()> work;
  if (call.module == "fs") {
    const base::Value* data = Arg(call, 1);
    const base::Value* options = Arg(call, 1);
    const bool recursive =
        call.method == "remove" && options && options->is_dict() &&
        options->GetDict().FindBool("recursive").value_or(false);
    work = base::BindOnce(&Files, call.files_root, call.method,
                          StringArg(call, 0),
                          call.method == "writeFile" ? GetBytes(data)
                                                     : std::nullopt,
                          recursive);
  } else {
    work = base::BindOnce(&Compute, call.module, call.method,
                          std::move(call.args));
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      std::move(work), base::BindOnce(&Reply, std::move(reply)));
}

}  // namespace zebkit
