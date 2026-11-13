/* zebkit's services (<zebkit/services.h>) without the engine running:
 * compression, cryptography, URLs, images. Known answers where the
 * formats have them. Prints one line per check; exit 0 when all pass. */

#include "harness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/services.h>


static void hex(const uint8_t *bytes, size_t size, char *out) {
    for (size_t i = 0; i < size; ++i)
        sprintf(out + 2 * i, "%02x", bytes[i]);
}

static void test_compression(void) {
    static const char *names[] = {"", "gzip", "zlib", "deflate-raw", "brotli", "zstd"};
    char text[4000];
    for (size_t i = 0; i < sizeof(text); ++i)
        text[i] = "abcabcabd"[i % 9];
    for (int format = ZEBKIT_COMPRESS_GZIP; format <= ZEBKIT_COMPRESS_ZSTD; ++format) {
        size_t packed_size = 0, unpacked_size = 0;
        char *error = NULL;
        uint8_t *packed = zebkit_compress(format, text, sizeof(text), -1, &packed_size, &error);
        uint8_t *unpacked = packed ? zebkit_decompress(format, packed, packed_size, 0, &unpacked_size, &error) : NULL;
        char what[64];
        snprintf(what, sizeof(what), "compress and decompress, %s", names[format]);
        check_text(unpacked && unpacked_size == sizeof(text) && memcmp(unpacked, text, sizeof(text)) == 0 &&
                  packed_size < 200,
              what, error);
        zebkit_free(packed);
        zebkit_free(unpacked);
        zebkit_free(error);
    }
    /* A limit on what data may expand to. */
    size_t size = 0;
    char *error = NULL;
    uint8_t *packed = zebkit_compress(ZEBKIT_COMPRESS_GZIP, text, sizeof(text), 9, &size, NULL);
    uint8_t *limited = zebkit_decompress(ZEBKIT_COMPRESS_GZIP, packed, size, 100, &size, &error);
    check_text(!limited && error, "decompress refuses more than the limit", error);
    zebkit_free(packed);
    zebkit_free(error);
    error = NULL;
    uint8_t *garbage = zebkit_decompress(ZEBKIT_COMPRESS_ZSTD, "not zstd", 8, 0, &size, &error);
    check_text(!garbage && error, "decompress refuses what is not the format", error);
    zebkit_free(error);
}

static void test_crypto(void) {
    uint8_t digest[64];
    char text[129];
    zebkit_digest_compute(ZEBKIT_DIGEST_SHA256, "abc", 3, digest);
    hex(digest, 32, text);
    check_text(strcmp(text, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0, "SHA-256(\"abc\")",
          NULL);
    check_text(zebkit_digest_size(ZEBKIT_DIGEST_SHA384) == 48, "the size of SHA-384", NULL);
    /* RFC 4231, test case 2. */
    zebkit_hmac(ZEBKIT_DIGEST_SHA256, "Jefe", 4, "what do ya want for nothing?", 28, digest);
    hex(digest, 32, text);
    check_text(strcmp(text, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843") == 0,
          "HMAC-SHA-256, RFC 4231 case 2", NULL);
    uint8_t a[16] = {0}, b[16] = {0};
    check_text(zebkit_random_bytes(a, 16) && zebkit_random_bytes(b, 16) && memcmp(a, b, 16) != 0, "random bytes", NULL);

    uint8_t key[32], nonce[12];
    zebkit_random_bytes(key, sizeof(key));
    zebkit_random_bytes(nonce, sizeof(nonce));
    size_t sealed_size = 0, opened_size = 0;
    uint8_t *sealed = zebkit_aes_gcm_seal(key, 32, nonce, 12, "ad", 2, "secret", 6, &sealed_size, NULL);
    uint8_t *opened = sealed ? zebkit_aes_gcm_open(key, 32, nonce, 12, "ad", 2, sealed, sealed_size, &opened_size, NULL)
                             : NULL;
    check_text(sealed_size == 6 + 16 && opened && opened_size == 6 && memcmp(opened, "secret", 6) == 0,
          "AES-256-GCM: seal and open", NULL);
    zebkit_free(opened);
    sealed[0] ^= 1;
    char *error = NULL;
    opened = zebkit_aes_gcm_open(key, 32, nonce, 12, "ad", 2, sealed, sealed_size, &opened_size, &error);
    check_text(!opened && error, "AES-256-GCM: a changed byte is refused", error);
    zebkit_free(error);
    zebkit_free(sealed);
}

static void expect_url(const char *url, const char *base, zebkit_url_part part, const char *expected) {
    char *got = zebkit_url_get(url, base, part);
    char what[160];
    snprintf(what, sizeof(what), "url part %d of %s%s%s", part, url, base ? " against " : "", base ? base : "");
    check_text(got && strcmp(got, expected) == 0, what, got ? got : "(none)");
    zebkit_free(got);
}

static void test_urls(void) {
    expect_url("HTTPS://User:Pw@Example.COM:443/a/../b?q=1#f", NULL, ZEBKIT_URL_SPEC, "https://User:Pw@example.com/b?q=1#f");
    expect_url("../c?x", "https://example.com/a/b/", ZEBKIT_URL_SPEC, "https://example.com/a/c?x");
    expect_url("http://bücher.example/", NULL, ZEBKIT_URL_HOST, "xn--bcher-kva.example");
    expect_url("https://example.com:8443/p", NULL, ZEBKIT_URL_ORIGIN, "https://example.com:8443");
    expect_url("https://example.com/p?a=b#c", NULL, ZEBKIT_URL_QUERY, "a=b");
    char *invalid = zebkit_url_get("not a url", NULL, ZEBKIT_URL_SPEC);
    check_text(invalid == NULL, "an invalid URL", NULL);
    char *domain = zebkit_url_registrable_domain("a.b.example.co.uk");
    check_text(domain && strcmp(domain, "example.co.uk") == 0, "registrable domain of a.b.example.co.uk", domain);
    zebkit_free(domain);
    domain = zebkit_url_registrable_domain("https://user.github.io/x");
    check_text(domain && strcmp(domain, "user.github.io") == 0, "registrable domain of a private suffix", domain);
    zebkit_free(domain);
}

static void test_images(void) {
    static const char *names[] = {"", "PNG", "JPEG", "WebP lossless"};
    const int w = 16, h = 8;
    uint8_t pixels[16 * 8 * 4];
    for (int i = 0; i < w * h; ++i) {
        pixels[4 * i + 0] = (uint8_t)(i * 7);
        pixels[4 * i + 1] = (uint8_t)(255 - i);
        pixels[4 * i + 2] = 40;
        pixels[4 * i + 3] = 255;
    }
    for (int format = ZEBKIT_IMAGE_PNG; format <= ZEBKIT_IMAGE_WEBP; ++format) {
        size_t size = 0;
        char *error = NULL;
        uint8_t *encoded = zebkit_image_encode(format, pixels, w, h, format == ZEBKIT_IMAGE_WEBP ? 101 : 95, &size,
                                               &error);
        int dw = 0, dh = 0;
        uint8_t *decoded = encoded ? zebkit_image_decode(encoded, size, &dw, &dh, &error) : NULL;
        int same = decoded && dw == w && dh == h;
        /* PNG and lossless WebP give the pixels back exactly; JPEG nearly. */
        if (same && format != ZEBKIT_IMAGE_JPEG)
            same = memcmp(decoded, pixels, sizeof(pixels)) == 0;
        char what[64];
        snprintf(what, sizeof(what), "image: encode and decode %s", names[format]);
        check_text(same, what, error);
        zebkit_free(encoded);
        zebkit_free(decoded);
        zebkit_free(error);
    }
    char *error = NULL;
    int w2, h2;
    check_text(!zebkit_image_decode("GIF89a", 6, &w2, &h2, &error) && error, "image: an unsupported format is refused", error);
    zebkit_free(error);
}

int main(void) {
    test_compression();
    test_crypto();
    test_urls();
    test_images();
    return summary();
}
