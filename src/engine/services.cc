// zebkit's services (services.h): compression, cryptography, URLs, images,
// ICU's data and HTTP, over what Chromium has inside it.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/i18n/icu_util.h"
#include "base/memory/ref_counted.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/net_errors.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/simple_url_loader_stream_consumer.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/boringssl/src/include/openssl/aead.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/hmac.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#include "third_party/brotli/include/brotli/decode.h"
#include "third_party/brotli/include/brotli/encode.h"
#include "third_party/libwebp/src/src/webp/decode.h"
#include "third_party/libwebp/src/src/webp/encode.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "third_party/zlib/zlib.h"
#include "third_party/zstd/src/lib/zstd.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/size.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "zebkit/engine/engine.h"
#include "zebkit/engine/include/zebkit/services.h"

namespace {

constexpr size_t kDefaultMaxSize = 256u << 20;
std::atomic<bool> g_content_icu_initialized{false};

// A buffer the caller frees with zebkit_free().
uint8_t* Give(const uint8_t* data, size_t size, size_t* out_size) {
  uint8_t* copy = static_cast<uint8_t*>(malloc(size ? size : 1));
  if (!copy)
    return nullptr;
  if (size)
    memcpy(copy, data, size);
  if (out_size)
    *out_size = size;
  return copy;
}

uint8_t* Give(const std::vector<uint8_t>& data, size_t* out_size) {
  return Give(data.data(), data.size(), out_size);
}

char* GiveString(const std::string& text) {
  return strdup(text.c_str());
}

std::nullptr_t Fail(char** error, const std::string& message) {
  if (error)
    *error = strdup(message.c_str());
  return nullptr;
}

// ---- zlib's formats ----

int WindowBits(zebkit_compression format) {
  switch (format) {
    case ZEBKIT_COMPRESS_GZIP:
      return 15 + 16;
    case ZEBKIT_COMPRESS_ZLIB:
      return 15;
    default:
      return -15;
  }
}

uint8_t* ZlibCompress(zebkit_compression format,
                      const uint8_t* data,
                      size_t size,
                      int level,
                      size_t* out_size,
                      char** error) {
  z_stream stream = {};
  if (deflateInit2(&stream, level < 0 ? Z_DEFAULT_COMPRESSION : level,
                   Z_DEFLATED, WindowBits(format), 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return Fail(error, "bad compression level");
  }
  std::vector<uint8_t> out(deflateBound(&stream, size));
  stream.next_in = const_cast<uint8_t*>(data);
  stream.avail_in = static_cast<uInt>(size);
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  const int result = deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  if (result != Z_STREAM_END)
    return Fail(error, "compression failed");
  return Give(out, out_size);
}

uint8_t* ZlibDecompress(zebkit_compression format,
                        const uint8_t* data,
                        size_t size,
                        size_t max_size,
                        size_t* out_size,
                        char** error) {
  z_stream stream = {};
  if (inflateInit2(&stream, WindowBits(format)) != Z_OK)
    return Fail(error, "cannot start decompressing");
  std::vector<uint8_t> out;
  uint8_t chunk[64 * 1024];
  stream.next_in = const_cast<uint8_t*>(data);
  stream.avail_in = static_cast<uInt>(size);
  int result = Z_OK;
  while (result != Z_STREAM_END) {
    stream.next_out = chunk;
    stream.avail_out = sizeof(chunk);
    result = inflate(&stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_STREAM_END) {
      inflateEnd(&stream);
      return Fail(error, stream.msg ? stream.msg : "corrupt or truncated data");
    }
    out.insert(out.end(), chunk, chunk + (sizeof(chunk) - stream.avail_out));
    if (out.size() > max_size) {
      inflateEnd(&stream);
      return Fail(error, "the data expands beyond the limit");
    }
    if (result == Z_OK && stream.avail_in == 0 && stream.avail_out != 0) {
      inflateEnd(&stream);
      return Fail(error, "truncated data");
    }
  }
  inflateEnd(&stream);
  return Give(out, out_size);
}

const EVP_MD* Digest(zebkit_digest digest) {
  switch (digest) {
    case ZEBKIT_DIGEST_SHA1:
      return EVP_sha1();
    case ZEBKIT_DIGEST_SHA256:
      return EVP_sha256();
    case ZEBKIT_DIGEST_SHA384:
      return EVP_sha384();
    case ZEBKIT_DIGEST_SHA512:
      return EVP_sha512();
  }
  return nullptr;
}

const EVP_AEAD* AesGcm(size_t key_size) {
  if (key_size == 16)
    return EVP_aead_aes_128_gcm();
  if (key_size == 32)
    return EVP_aead_aes_256_gcm();
  return nullptr;
}

// ---- HTTP ----

class Fetch : public network::SimpleURLLoaderStreamConsumer {
 public:
  static void Start(content::BrowserContext* profile,
                    std::unique_ptr<network::ResourceRequest> request,
                    std::string body,
                    std::string content_type,
                    zebkit_response_func func,
                    void* data) {
    auto* fetch = new Fetch(func, data);
    constexpr net::NetworkTrafficAnnotationTag kAnnotation =
        net::DefineNetworkTrafficAnnotation("zebkit_fetch", R"(
          semantics {
            sender: "zebkit"
            description: "A request the embedding application makes."
            trigger: "The application calls zebkit_fetch() or zebkit_fetch_in()."
            data: "What the application sends."
            destination: OTHER
          }
          policy {
            cookies_allowed: YES
            setting: "Up to the application."
            policy_exception_justification: "Made by the application."
          })");
    fetch->loader_ =
        network::SimpleURLLoader::Create(std::move(request), kAnnotation);
    fetch->loader_->SetOnResponseStartedCallback(
        base::BindOnce(&Fetch::ResponseStarted, base::Unretained(fetch)));
    // What the application gave for one origin is not for another's, as the
    // fetch standard does for Authorization. The stack's own cookies are kept
    // to their own origins by the stack.
    fetch->loader_->SetOnRedirectCallback(base::BindRepeating(
        [](const GURL& from, const net::RedirectInfo& info,
           const network::mojom::URLResponseHead&,
           std::vector<std::string>* removed) {
          if (!url::Origin::Create(from).IsSameOriginWith(info.new_url)) {
            removed->push_back("Authorization");
            removed->push_back("Proxy-Authorization");
            removed->push_back("Cookie");
          }
        }));
    if (!body.empty() || !content_type.empty())
      fetch->loader_->AttachStringForUpload(std::move(body), content_type);
    fetch->loader_->SetAllowHttpErrorResults(true);
    // The profile's network: its cookies, cache and connections.
    scoped_refptr<network::SharedURLLoaderFactory> factory =
        profile->GetDefaultStoragePartition()->GetURLLoaderFactoryForBrowserProcess();
    // DownloadToString only accepts limits up to 5 MiB. The public C API
    // permits 256 MiB, so consume the stream while enforcing that bound.
    fetch->loader_->DownloadAsStream(factory.get(), fetch);
  }

 private:
  Fetch(zebkit_response_func func, void* data) : func_(func), data_(data) {}
  ~Fetch() override = default;

  void ResponseStarted(const GURL& url,
                       const network::mojom::URLResponseHead& head) {
    response_url_ = url.spec();
    response_headers_ = head.headers;
  }

  void OnDataReceived(std::string_view chunk, base::OnceClosure resume) override {
    if (chunk.size() > kDefaultMaxSize - body_.size()) {
      // The stream API permits deleting the loader from this callback.
      Done(std::nullopt, net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }
    body_.append(chunk);
    std::move(resume).Run();
  }

  void OnComplete(bool success) override {
    if (success)
      Done(std::move(body_));
    else
      Done(std::nullopt);
  }

  void OnRetry(base::OnceClosure start_retry) override {
    body_.clear();
    std::move(start_retry).Run();
  }

  void Done(std::optional<std::string> body, int error_override = net::OK) {
    zebkit_response response = {};
    std::string headers;
    // The loader's status accessors require completed requests. A size
    // rejection happens during streaming, so use the captured response.
    const bool early = error_override != net::OK;
    std::string url = early ? response_url_ : loader_->GetFinalURL().spec();
    std::string error;
    const network::mojom::URLResponseHead* head =
        early ? nullptr : loader_->ResponseInfo();
    const net::HttpResponseHeaders* received_headers =
        early ? response_headers_.get() : (head ? head->headers.get() : nullptr);
    if (received_headers) {
      response.status = received_headers->response_code();
      size_t iter = 0;
      std::string name, value;
      while (received_headers->EnumerateHeaderLines(&iter, &name, &value)) {
        if (!headers.empty())
          headers += "\r\n";
        headers += name + ": " + value;
      }
    }
    if (!body) {
      error = net::ErrorToString(error_override == net::OK
                                    ? loader_->NetError()
                                    : error_override);
      response.error = error.c_str();
    } else {
      response.body = reinterpret_cast<const uint8_t*>(body->data());
      response.body_size = body->size();
    }
    response.url = url.c_str();
    response.headers = headers.c_str();
    if (func_)
      func_(&response, data_);
    delete this;
  }

  zebkit_response_func func_;
  void* data_;
  std::unique_ptr<network::SimpleURLLoader> loader_;
  std::string body_;
  std::string response_url_;
  scoped_refptr<net::HttpResponseHeaders> response_headers_;
};

}  // namespace

namespace zebkit {

void MarkICUInitialized() {
  g_content_icu_initialized.store(true, std::memory_order_release);
}

}  // namespace zebkit

extern "C" {

uint8_t* zebkit_compress(zebkit_compression format,
                         const void* data,
                         size_t size,
                         int level,
                         size_t* out_size,
                         char** error) {
  const uint8_t* in = static_cast<const uint8_t*>(data);
  switch (format) {
    case ZEBKIT_COMPRESS_GZIP:
    case ZEBKIT_COMPRESS_ZLIB:
    case ZEBKIT_COMPRESS_DEFLATE_RAW:
      if (level > 9)
        return Fail(error, "zlib's levels are 0 to 9");
      return ZlibCompress(format, in, size, level, out_size, error);
    case ZEBKIT_COMPRESS_BROTLI: {
      if (level > BROTLI_MAX_QUALITY)
        return Fail(error, "Brotli's levels are 0 to 11");
      size_t encoded = BrotliEncoderMaxCompressedSize(size);
      std::vector<uint8_t> out(encoded ? encoded : 64);
      encoded = out.size();
      if (!BrotliEncoderCompress(level < 0 ? BROTLI_DEFAULT_QUALITY : level,
                                 BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC,
                                 size, in, &encoded, out.data())) {
        return Fail(error, "compression failed");
      }
      out.resize(encoded);
      return Give(out, out_size);
    }
    case ZEBKIT_COMPRESS_ZSTD: {
      if (level > ZSTD_maxCLevel())
        return Fail(error, "Zstandard's levels go to 22");
      std::vector<uint8_t> out(ZSTD_compressBound(size));
      const size_t written =
          ZSTD_compress(out.data(), out.size(), in, size,
                        level < 0 ? ZSTD_CLEVEL_DEFAULT : level);
      if (ZSTD_isError(written))
        return Fail(error, ZSTD_getErrorName(written));
      out.resize(written);
      return Give(out, out_size);
    }
  }
  return Fail(error, "unknown format");
}

uint8_t* zebkit_decompress(zebkit_compression format,
                           const void* data,
                           size_t size,
                           size_t max_size,
                           size_t* out_size,
                           char** error) {
  const uint8_t* in = static_cast<const uint8_t*>(data);
  if (!max_size)
    max_size = kDefaultMaxSize;
  switch (format) {
    case ZEBKIT_COMPRESS_GZIP:
    case ZEBKIT_COMPRESS_ZLIB:
    case ZEBKIT_COMPRESS_DEFLATE_RAW:
      return ZlibDecompress(format, in, size, max_size, out_size, error);
    case ZEBKIT_COMPRESS_BROTLI: {
      BrotliDecoderState* state =
          BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
      std::vector<uint8_t> out;
      uint8_t chunk[64 * 1024];
      size_t available_in = size;
      const uint8_t* next_in = in;
      BrotliDecoderResult result = BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT;
      while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        size_t available_out = sizeof(chunk);
        uint8_t* next_out = chunk;
        result = BrotliDecoderDecompressStream(state, &available_in, &next_in,
                                               &available_out, &next_out,
                                               nullptr);
        out.insert(out.end(), chunk, next_out);
        if (out.size() > max_size) {
          BrotliDecoderDestroyInstance(state);
          return Fail(error, "the data expands beyond the limit");
        }
      }
      BrotliDecoderDestroyInstance(state);
      if (result != BROTLI_DECODER_RESULT_SUCCESS)
        return Fail(error, "corrupt or truncated data");
      return Give(out, out_size);
    }
    case ZEBKIT_COMPRESS_ZSTD: {
      ZSTD_DStream* stream = ZSTD_createDStream();
      std::vector<uint8_t> out;
      std::vector<uint8_t> chunk(ZSTD_DStreamOutSize());
      ZSTD_inBuffer input = {in, size, 0};
      size_t pending = 1;
      while (input.pos < input.size || pending) {
        ZSTD_outBuffer output = {chunk.data(), chunk.size(), 0};
        const size_t before = input.pos;
        pending = ZSTD_decompressStream(stream, &output, &input);
        if (ZSTD_isError(pending)) {
          ZSTD_freeDStream(stream);
          return Fail(error, ZSTD_getErrorName(pending));
        }
        out.insert(out.end(), chunk.data(), chunk.data() + output.pos);
        if (out.size() > max_size) {
          ZSTD_freeDStream(stream);
          return Fail(error, "the data expands beyond the limit");
        }
        if (input.pos == before && output.pos == 0) {
          ZSTD_freeDStream(stream);
          return Fail(error, "truncated data");
        }
      }
      ZSTD_freeDStream(stream);
      return Give(out, out_size);
    }
  }
  return Fail(error, "unknown format");
}

size_t zebkit_digest_size(zebkit_digest digest) {
  const EVP_MD* md = Digest(digest);
  return md ? EVP_MD_size(md) : 0;
}

bool zebkit_digest_compute(zebkit_digest digest,
                           const void* data,
                           size_t size,
                           uint8_t* out) {
  const EVP_MD* md = Digest(digest);
  return md && EVP_Digest(data, size, out, nullptr, md, nullptr);
}

bool zebkit_hmac(zebkit_digest digest,
                 const void* key,
                 size_t key_size,
                 const void* data,
                 size_t size,
                 uint8_t* out) {
  const EVP_MD* md = Digest(digest);
  unsigned length = 0;
  return md && HMAC(md, key, key_size, static_cast<const uint8_t*>(data), size,
                    out, &length);
}

bool zebkit_random_bytes(void* out, size_t size) {
  return RAND_bytes(static_cast<uint8_t*>(out), size) == 1;
}

uint8_t* zebkit_aes_gcm_seal(const void* key,
                             size_t key_size,
                             const void* nonce,
                             size_t nonce_size,
                             const void* ad,
                             size_t ad_size,
                             const void* plaintext,
                             size_t size,
                             size_t* out_size,
                             char** error) {
  const EVP_AEAD* aead = AesGcm(key_size);
  if (!aead)
    return Fail(error, "the key must be 16 or 32 bytes");
  if (nonce_size != 12)
    return Fail(error, "the nonce must be 12 bytes");
  EVP_AEAD_CTX context;
  if (!EVP_AEAD_CTX_init(&context, aead, static_cast<const uint8_t*>(key),
                         key_size, EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
    return Fail(error, "bad key");
  }
  std::vector<uint8_t> out(size + EVP_AEAD_max_overhead(aead));
  size_t written = 0;
  const bool ok = EVP_AEAD_CTX_seal(
      &context, out.data(), &written, out.size(),
      static_cast<const uint8_t*>(nonce), nonce_size,
      static_cast<const uint8_t*>(plaintext), size,
      static_cast<const uint8_t*>(ad), ad ? ad_size : 0);
  EVP_AEAD_CTX_cleanup(&context);
  if (!ok)
    return Fail(error, "sealing failed");
  out.resize(written);
  return Give(out, out_size);
}

uint8_t* zebkit_aes_gcm_open(const void* key,
                             size_t key_size,
                             const void* nonce,
                             size_t nonce_size,
                             const void* ad,
                             size_t ad_size,
                             const void* sealed,
                             size_t size,
                             size_t* out_size,
                             char** error) {
  const EVP_AEAD* aead = AesGcm(key_size);
  if (!aead)
    return Fail(error, "the key must be 16 or 32 bytes");
  if (nonce_size != 12)
    return Fail(error, "the nonce must be 12 bytes");
  EVP_AEAD_CTX context;
  if (!EVP_AEAD_CTX_init(&context, aead, static_cast<const uint8_t*>(key),
                         key_size, EVP_AEAD_DEFAULT_TAG_LENGTH, nullptr)) {
    return Fail(error, "bad key");
  }
  std::vector<uint8_t> out(size);
  size_t written = 0;
  const bool ok = EVP_AEAD_CTX_open(
      &context, out.data(), &written, out.size(),
      static_cast<const uint8_t*>(nonce), nonce_size,
      static_cast<const uint8_t*>(sealed), size,
      static_cast<const uint8_t*>(ad), ad ? ad_size : 0);
  EVP_AEAD_CTX_cleanup(&context);
  if (!ok)
    return Fail(error, "the data or its tag does not match the key");
  out.resize(written);
  return Give(out, out_size);
}

char* zebkit_url_get(const char* url, const char* base, zebkit_url_part part) {
  if (!url)
    return nullptr;
  // International domain names are ICU's to convert.
  zebkit_icu_init();
  GURL parsed = base ? GURL(base).Resolve(url) : GURL(url);
  if (!parsed.is_valid())
    return nullptr;
  switch (part) {
    case ZEBKIT_URL_SPEC:
      return GiveString(parsed.spec());
    case ZEBKIT_URL_SCHEME:
      return GiveString(std::string(parsed.scheme()));
    case ZEBKIT_URL_USERNAME:
      return GiveString(std::string(parsed.username()));
    case ZEBKIT_URL_PASSWORD:
      return GiveString(std::string(parsed.password()));
    case ZEBKIT_URL_HOST:
      return GiveString(std::string(parsed.host()));
    case ZEBKIT_URL_PORT:
      return GiveString(std::string(parsed.port()));
    case ZEBKIT_URL_PATH:
      return GiveString(std::string(parsed.path()));
    case ZEBKIT_URL_QUERY:
      return GiveString(std::string(parsed.query()));
    case ZEBKIT_URL_FRAGMENT:
      return GiveString(std::string(parsed.ref()));
    case ZEBKIT_URL_ORIGIN:
      return GiveString(url::Origin::Create(parsed).Serialize());
  }
  return nullptr;
}

char* zebkit_url_registrable_domain(const char* host_or_url) {
  if (!host_or_url || !*host_or_url)
    return nullptr;
  zebkit_icu_init();
  GURL parsed(host_or_url);
  if (!parsed.is_valid() || !parsed.has_host())
    parsed = GURL(std::string("https://") + host_or_url + "/");
  if (!parsed.is_valid() || !parsed.has_host())
    return nullptr;
  return GiveString(net::registry_controlled_domains::GetDomainAndRegistry(
      parsed, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES));
}

uint8_t* zebkit_image_decode(const void* data,
                             size_t size,
                             int* width,
                             int* height,
                             char** error) {
  const uint8_t* in = static_cast<const uint8_t*>(data);
  int w = 0, h = 0;
  std::vector<uint8_t> pixels;
  if (size >= 8 && memcmp(in, "\x89PNG\r\n\x1a\n", 8) == 0) {
    std::optional<gfx::PNGCodec::DecodeOutput> decoded = gfx::PNGCodec::Decode(
        base::span<const uint8_t>(in, size), gfx::PNGCodec::FORMAT_RGBA);
    if (!decoded)
      return Fail(error, "not a valid PNG file");
    pixels = std::move(decoded->output);
    w = decoded->width;
    h = decoded->height;
  } else if (size >= 3 && in[0] == 0xFF && in[1] == 0xD8 && in[2] == 0xFF) {
    if (!gfx::JPEGCodec::Decode(in, size, kRGBA_8888_SkColorType, &pixels, &w,
                                &h)) {
      return Fail(error, "not a valid JPEG file");
    }
  } else if (size >= 12 && memcmp(in, "RIFF", 4) == 0 &&
             memcmp(in + 8, "WEBP", 4) == 0) {
    uint8_t* decoded = WebPDecodeRGBA(in, size, &w, &h);
    if (!decoded)
      return Fail(error, "not a valid WebP file");
    pixels.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
    WebPFree(decoded);
  } else {
    return Fail(error, "not a PNG, JPEG or WebP file");
  }
  if (width)
    *width = w;
  if (height)
    *height = h;
  return Give(pixels, nullptr);
}

uint8_t* zebkit_image_encode(zebkit_image_format format,
                             const uint8_t* rgba,
                             int width,
                             int height,
                             int quality,
                             size_t* out_size,
                             char** error) {
  if (!rgba || width <= 0 || height <= 0)
    return Fail(error, "no pixels");
  if (quality < 0)
    quality = 90;
  switch (format) {
    case ZEBKIT_IMAGE_PNG: {
      std::optional<std::vector<uint8_t>> encoded = gfx::PNGCodec::Encode(
          rgba, gfx::PNGCodec::FORMAT_RGBA, gfx::Size(width, height),
          width * 4, /*discard_transparency=*/false, {});
      if (!encoded)
        return Fail(error, "encoding failed");
      return Give(*encoded, out_size);
    }
    case ZEBKIT_IMAGE_JPEG: {
      const SkImageInfo info = SkImageInfo::Make(
          width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
      std::optional<std::vector<uint8_t>> encoded = gfx::JPEGCodec::Encode(
          SkPixmap(info, rgba, static_cast<size_t>(width) * 4),
          quality > 100 ? 100 : quality);
      if (!encoded)
        return Fail(error, "encoding failed");
      return Give(*encoded, out_size);
    }
    case ZEBKIT_IMAGE_WEBP: {
      uint8_t* encoded = nullptr;
      const size_t written =
          quality > 100
              ? WebPEncodeLosslessRGBA(rgba, width, height, width * 4, &encoded)
              : WebPEncodeRGBA(rgba, width, height, width * 4, quality,
                               &encoded);
      if (!written)
        return Fail(error, "encoding failed");
      uint8_t* out = Give(encoded, written, out_size);
      WebPFree(encoded);
      return out;
    }
  }
  return Fail(error, "unknown format");
}

bool zebkit_icu_init(void) {
  // Content has already initialized ICU before entering RunProcess().
  // Services used by an application's script share that initialization.
  if (g_content_icu_initialized.load(std::memory_order_acquire))
    return true;
  static const bool initialized = [] {
    // ICU's data is beside the engine, which is not beside the application.
    const base::FilePath library = zebkit::LibraryDirectory();
    if (!library.empty()) {
      base::PathService::OverrideAndCreateIfNeeded(
          base::DIR_MODULE, library, /*is_absolute=*/true, /*create=*/false);
#if BUILDFLAG(IS_MAC)
      // On macOS ICU's data is in the framework's resources, which base
      // finds through the bundle zebkit_main() would have set.
      zebkit::SetUpBundle(library);
#endif
    }
    return base::i18n::InitializeICU();
  }();
  return initialized;
}

void zebkit_fetch(const char* method,
                  const char* url,
                  const char* headers,
                  const void* body,
                  size_t body_size,
                  zebkit_response_func func,
                  void* data) {
  zebkit_fetch_in(nullptr, method, url, headers, body, body_size, func, data);
}

void zebkit_fetch_in(zebkit_context* context,
                     const char* method,
                     const char* url,
                     const char* headers,
                     const void* body,
                     size_t body_size,
                     zebkit_response_func func,
                     void* data) {
  if (!zebkit::CurrentlyOnEngineThread())
    return;
  auto refuse = [&](const char* why) {
    zebkit_response response = {};
    response.url = "";
    response.headers = "";
    response.error = why;
    if (func)
      func(&response, data);
  };
  content::BrowserContext* profile = zebkit::ProfileOf(context);
  if ((context && !context->profile) || !profile)
    return refuse("the context is closed");
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(url ? url : "");
  request->method = method && *method ? method : "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kInclude;
  request->site_for_cookies = net::SiteForCookies::FromUrl(request->url);
  std::string content_type;
  if (headers) {
    for (std::string_view line : base::SplitStringPieceUsingSubstr(
             headers, "\r\n", base::TRIM_WHITESPACE,
             base::SPLIT_WANT_NONEMPTY)) {
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos)
        continue;
      std::string name(base::TrimWhitespaceASCII(line.substr(0, colon),
                                                 base::TRIM_ALL));
      std::string value(base::TrimWhitespaceASCII(line.substr(colon + 1),
                                                  base::TRIM_ALL));
      if (base::EqualsCaseInsensitiveASCII(name, "content-type"))
        content_type = value;
      else
        request->headers.SetHeader(name, value);
    }
  }
  if (!request->url.is_valid())
    return refuse("not a valid URL");
  std::string upload;
  if (body && body_size)
    upload.assign(static_cast<const char*>(body), body_size);
  if (!upload.empty() && content_type.empty())
    content_type = "application/octet-stream";
  Fetch::Start(profile, std::move(request), std::move(upload), std::move(content_type),
               func, data);
}

}  // extern "C"
