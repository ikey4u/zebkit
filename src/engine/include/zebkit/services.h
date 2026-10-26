/*
 * zebkit's services: what Chromium has inside it besides the web page,
 * for applications, as plain C functions. Each works without a view; those
 * marked "engine" need zebkit_main() running and call back on its thread.
 *
 * Buffers and strings returned are the caller's, to free with
 * zebkit_free(). A function that fails returns NULL (or false) and, when
 * @error is not NULL, sets it to a message to free with zebkit_free().
 *
 * The libraries themselves, with their own APIs, are in
 * <zebkit/vendor/<library>.h>.
 */

#ifndef ZEBKIT_SERVICES_H
#define ZEBKIT_SERVICES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compression: zlib, Brotli, Zstandard ---------------------------- */

typedef enum {
    ZEBKIT_COMPRESS_GZIP = 1,         /* RFC 1952, as Content-Encoding: gzip */
    ZEBKIT_COMPRESS_ZLIB = 2,         /* RFC 1950, as Content-Encoding: deflate */
    ZEBKIT_COMPRESS_DEFLATE_RAW = 3,  /* RFC 1951, no header */
    ZEBKIT_COMPRESS_BROTLI = 4,       /* RFC 7932 */
    ZEBKIT_COMPRESS_ZSTD = 5,         /* RFC 8878 */
} zebkit_compression;

/*
 * Compresses @size bytes at @data. @level -1 is the format's default;
 * otherwise 0-9 for zlib's formats, 0-11 for Brotli, 1-22 for Zstandard.
 * Sets *@out_size.
 */
ZEBKIT_API uint8_t *zebkit_compress(zebkit_compression format, const void *data, size_t size, int level,
                                    size_t *out_size, char **error);

/*
 * Decompresses @size bytes at @data. Fails rather than produce more than
 * @max_size bytes (0: 256 MiB), against data that expands without end.
 */
ZEBKIT_API uint8_t *zebkit_decompress(zebkit_compression format, const void *data, size_t size, size_t max_size,
                                      size_t *out_size, char **error);

/* ---- Cryptography: BoringSSL ----------------------------------------- */

typedef enum {
    ZEBKIT_DIGEST_SHA1 = 1,
    ZEBKIT_DIGEST_SHA256 = 2,
    ZEBKIT_DIGEST_SHA384 = 3,
    ZEBKIT_DIGEST_SHA512 = 4,
} zebkit_digest;

/* The size of a digest in bytes: 20, 32, 48 or 64; 0 for an unknown one. */
ZEBKIT_API size_t zebkit_digest_size(zebkit_digest digest);

/* Writes zebkit_digest_size(@digest) bytes to @out. */
ZEBKIT_API bool zebkit_digest_compute(zebkit_digest digest, const void *data, size_t size, uint8_t *out);

/* HMAC with @digest; writes zebkit_digest_size(@digest) bytes to @out. */
ZEBKIT_API bool zebkit_hmac(zebkit_digest digest, const void *key, size_t key_size, const void *data, size_t size,
                            uint8_t *out);

/* Fills @out with @size bytes from the system's secure random source. */
ZEBKIT_API bool zebkit_random_bytes(void *out, size_t size);

/*
 * AES-GCM with a 16-byte tag: a 16- or 32-byte @key (AES-128 or AES-256)
 * and a 12-byte @nonce, never used twice with the same key. @ad is
 * authenticated, not encrypted, and may be NULL. Sealing returns the
 * ciphertext followed by the tag; opening takes that and returns the
 * plaintext, or fails when anything was changed.
 */
ZEBKIT_API uint8_t *zebkit_aes_gcm_seal(const void *key, size_t key_size, const void *nonce, size_t nonce_size,
                                        const void *ad, size_t ad_size, const void *plaintext, size_t size,
                                        size_t *out_size, char **error);
ZEBKIT_API uint8_t *zebkit_aes_gcm_open(const void *key, size_t key_size, const void *nonce, size_t nonce_size,
                                        const void *ad, size_t ad_size, const void *sealed, size_t size,
                                        size_t *out_size, char **error);

/* ---- URLs: Chromium's parser ----------------------------------------- */

typedef enum {
    ZEBKIT_URL_SPEC = 0,      /* the whole URL, canonical */
    ZEBKIT_URL_SCHEME = 1,    /* "https" */
    ZEBKIT_URL_USERNAME = 2,
    ZEBKIT_URL_PASSWORD = 3,
    ZEBKIT_URL_HOST = 4,      /* "example.com", "[::1]"; punycode for IDN */
    ZEBKIT_URL_PORT = 5,      /* "" when it is the scheme's default */
    ZEBKIT_URL_PATH = 6,      /* "/a/b" */
    ZEBKIT_URL_QUERY = 7,     /* without the "?" */
    ZEBKIT_URL_FRAGMENT = 8,  /* without the "#" */
    ZEBKIT_URL_ORIGIN = 9,    /* "https://example.com", or "null" */
} zebkit_url_part;

/*
 * A part of @url, resolved against @base when @base is not NULL, as a
 * browser parses and canonicalizes it. NULL when it is not a valid URL.
 */
ZEBKIT_API char *zebkit_url_get(const char *url, const char *base, zebkit_url_part part);

/*
 * The registrable domain of a host or URL, by the public suffix list:
 * "example.co.uk" for "a.b.example.co.uk". "" for a host that has none (an
 * IP address, "localhost", a public suffix itself). NULL when invalid.
 */
ZEBKIT_API char *zebkit_url_registrable_domain(const char *host_or_url);

/* ---- Images: Chromium's codecs --------------------------------------- */

typedef enum {
    ZEBKIT_IMAGE_PNG = 1,
    ZEBKIT_IMAGE_JPEG = 2,
    ZEBKIT_IMAGE_WEBP = 3,
} zebkit_image_format;

/*
 * Decodes an image file (PNG, JPEG, WebP, and what else the engine's
 * Skia reads) to 8-bit RGBA, not premultiplied, @width * @height * 4 bytes,
 * rows top to bottom.
 */
ZEBKIT_API uint8_t *zebkit_image_decode(const void *data, size_t size, int *width, int *height, char **error);

/*
 * Encodes RGBA pixels, not premultiplied. @quality 0-100 is for JPEG and
 * lossy WebP; 101 asks WebP for lossless. PNG ignores it.
 */
ZEBKIT_API uint8_t *zebkit_image_encode(zebkit_image_format format, const uint8_t *rgba, int width, int height,
                                        int quality, size_t *out_size, char **error);

/* ---- Unicode: ICU ---------------------------------------------------- */

/*
 * Loads ICU's data (icudtl.dat, beside the engine) so that the ICU of
 * <zebkit/vendor/icu.h> can be used without zebkit_main(). Calling it
 * again, or with the engine running, does no harm.
 * If the process will also start the engine, use ICU-backed services
 * (including URL parsing) from its ready callback: Chromium registers
 * and locks URL schemes during startup, before any URLs are parsed.
 */
ZEBKIT_API bool zebkit_icu_init(void);

/* ---- HTTP: the engine's network stack (engine) ----------------------- */

typedef struct {
    int status;              /* HTTP status, or 0 when there was no response */
    const char *url;         /* after redirects */
    const char *headers;     /* "Name: value" lines, separated by "\r\n" */
    const uint8_t *body;
    size_t body_size;
    const char *error;       /* NULL, or why there was no response */
} zebkit_response;

typedef void (*zebkit_response_func)(const zebkit_response *response, void *data);

/*
 * Sends a request through the engine's network stack: HTTP/1.1, HTTP/2 and
 * HTTP/3, its cache, its cookies, its proxy settings, as the browser makes
 * a request of its own (no CORS, no page). @method NULL is GET; @headers
 * is "Name: value" lines separated by "\r\n", or NULL; @body may be NULL.
 * Redirects are followed. A body over 256 MiB fails. @func is called on
 * the engine's thread with what is valid for the call.
 */
ZEBKIT_API void zebkit_fetch(const char *method, const char *url, const char *headers, const void *body,
                             size_t body_size, zebkit_response_func func, void *data);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_SERVICES_H */
