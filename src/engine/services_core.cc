// zebkit's services (services.h) that need no engine: compression,
// cryptography, URLs, images and ICU's data, over what Chromium has inside
// it. They are in libzebkit on every system, the engine's or not (docs/MOBILE.md);
// HTTP, which needs the engine's network service, is in services.cc.

#ifdef UNSAFE_BUFFERS_BUILD
// The C API takes and returns C strings and arrays.
#pragma allow_unsafe_buffers
#endif

#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/memory_mapped_file.h"
#include "base/functional/function_ref.h"
#include "base/i18n/icu_util.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "build/build_config.h"
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
#include "zebkit/engine/include/zebkit/services.h"
#include "zebkit/engine/services_internal.h"

namespace {

using zebkit::services::Fail;
using zebkit::services::Give;
using zebkit::services::GiveString;
using zebkit::services::kDefaultMaxSize;

std::atomic<bool> g_content_icu_initialized{false};
// ICU's data loaded by zebkit_icu_init() or zebkit_icu_init_from_file().
std::atomic<bool> g_icu_loaded{false};

// Loads ICU's data once, by the first call that succeeds; a failed one may
// be tried again (with another file, say).
bool InitICUOnce(base::FunctionRef<bool()> load) {
  // Content has already initialized ICU before entering RunProcess().
  // Services used by an application's script share that initialization.
  if (g_content_icu_initialized.load(std::memory_order_acquire) ||
      g_icu_loaded.load(std::memory_order_acquire)) {
    return true;
  }
  static base::NoDestructor<base::Lock> lock;
  base::AutoLock hold(*lock);
  if (g_icu_loaded.load(std::memory_order_relaxed))
    return true;
  if (!load())
    return false;
  g_icu_loaded.store(true, std::memory_order_release);
  return true;
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

}  // namespace

namespace zebkit {

void MarkICUInitialized() {
  g_content_icu_initialized.store(true, std::memory_order_release);
}

bool services::InitializeICUFromFile(const base::FilePath& path) {
#if ICU_UTIL_DATA_IMPL == ICU_UTIL_DATA_FILE
  // Mapped from the file itself, without base's search for it (which on
  // Android asks Java for the APK's assets).
  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid())
    return false;
  return base::i18n::InitializeICUWithFileDescriptor(
      file.TakePlatformFile(), base::MemoryMappedFile::Region::kWholeFile);
#else
  // ICU's data is linked in.
  return base::i18n::InitializeICU();
#endif
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
  return InitICUOnce([] { return zebkit::LoadICUData(); });
}

bool zebkit_icu_init_from_file(const char* path) {
  if (!path || !*path)
    return false;
  return InitICUOnce([path] {
    return zebkit::services::InitializeICUFromFile(base::FilePath::FromUTF8Unsafe(path));
  });
}

}  // extern "C"
