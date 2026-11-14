/*
 * libzebkit without the engine at work (docs/MOBILE.md): each part once,
 * without zebkit_main(). scripts/mobile-test.sh runs it on an Android device
 * or emulator through adb, and here for the profile mobile-linux.
 *
 *     zebkit_mobile_test [icudtl.dat]
 *
 * Prints "ok <part>" per part and exits 0, or "FAIL <part>: why" and 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>
#include <zebkit/pdf.h>
#include <zebkit/services.h>
#include <zebkit/viewer.h>

static int failures;

static void check(int ok, const char *part, const char *why) {
    if (ok) {
        printf("ok %s\n", part);
    } else {
        printf("FAIL %s: %s\n", part, why ? why : "");
        failures++;
    }
}

/* A page with "Hello zebkit" in Helvetica. PDFium rebuilds the cross
 * reference table, which is left out. */
static const char kPdf[] =
    "%PDF-1.4\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100]"
    " /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >> endobj\n"
    "4 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "5 0 obj << /Length 43 >> stream\n"
    "BT /F1 18 Tf 20 40 Td (Hello zebkit) Tj ET\n"
    "endstream endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

static void test_pdf(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open(kPdf, sizeof(kPdf) - 1, NULL, &error);
    check(pdf != NULL, "pdf open", error);
    zebkit_free(error);
    if (!pdf)
        return;
    check(zebkit_pdf_page_count(pdf) == 1, "pdf pages", "not one page");

    error = NULL;
    char *text = zebkit_pdf_text(pdf, 0, &error);
    check(text && strstr(text, "Hello zebkit"), "pdf text", text ? text : error);
    zebkit_free(text);
    zebkit_free(error);

    int width = 0, height = 0;
    error = NULL;
    zebkit_pdf_render_options options = {2.0f, 0, 0, 0xffffffff, 0, 0, 0, 0};
    uint8_t *rgba = zebkit_pdf_render(pdf, 0, &options, &width, &height, &error);
    int dark = 0;
    if (rgba) {
        for (int i = 0; i < width * height; i++) {
            if (rgba[i * 4] < 128)
                dark++;
        }
    }
    check(rgba && width == 400 && height == 200 && dark > 100, "pdf render", error ? error : "no text drawn");

    /* The rendered page back through the image codecs. */
    if (rgba) {
        size_t png_size = 0;
        char *encode_error = NULL;
        uint8_t *png = zebkit_image_encode(ZEBKIT_IMAGE_PNG, rgba, width, height, -1, &png_size, &encode_error);
        int w = 0, h = 0;
        char *decode_error = NULL;
        uint8_t *back = png ? zebkit_image_decode(png, png_size, &w, &h, &decode_error) : NULL;
        check(back && w == width && h == height && memcmp(back, rgba, (size_t)w * h * 4) == 0, "image png",
              encode_error ? encode_error : decode_error);
        zebkit_free(back);
        zebkit_free(png);
        zebkit_free(encode_error);
        zebkit_free(decode_error);
    }
    zebkit_free(rgba);
    zebkit_free(error);

    size_t saved_size = 0;
    error = NULL;
    uint8_t *saved = zebkit_pdf_save(pdf, 0, &saved_size, &error);
    check(saved && saved_size > 100 && memcmp(saved, "%PDF", 4) == 0, "pdf save", error);
    zebkit_free(saved);
    zebkit_free(error);
    zebkit_pdf_close(pdf);
}

static void test_services(void) {
    static const char text[] = "zebkit without the engine, zebkit without the engine, zebkit without the engine";
    const zebkit_compression formats[] = {ZEBKIT_COMPRESS_GZIP, ZEBKIT_COMPRESS_BROTLI, ZEBKIT_COMPRESS_ZSTD};
    const char *names[] = {"compress gzip", "compress brotli", "compress zstd"};
    for (int i = 0; i < 3; i++) {
        size_t packed_size = 0, unpacked_size = 0;
        char *error = NULL;
        uint8_t *packed = zebkit_compress(formats[i], text, sizeof(text), -1, &packed_size, &error);
        uint8_t *unpacked =
            packed ? zebkit_decompress(formats[i], packed, packed_size, 0, &unpacked_size, &error) : NULL;
        check(unpacked && unpacked_size == sizeof(text) && memcmp(unpacked, text, sizeof(text)) == 0, names[i],
              error);
        zebkit_free(packed);
        zebkit_free(unpacked);
        zebkit_free(error);
    }

    uint8_t digest[32];
    static const uint8_t abc_sha256[4] = {0xba, 0x78, 0x16, 0xbf};
    check(zebkit_digest_compute(ZEBKIT_DIGEST_SHA256, "abc", 3, digest) && memcmp(digest, abc_sha256, 4) == 0,
          "digest sha256", "wrong digest");
}

static void test_icu(const char *data) {
    int loaded = data ? zebkit_icu_init_from_file(data) : zebkit_icu_init();
    check(loaded, "icu", "no icudtl.dat");
    if (!loaded)
        return;
    char *host = zebkit_url_get("https://bücher.example/a?b#c", NULL, ZEBKIT_URL_HOST);
    check(host && strcmp(host, "xn--bcher-kva.example") == 0, "url idn", host);
    zebkit_free(host);
}


/* The page's zebkit in a system web view (<zebkit/viewer.h>): messages as
 * the viewer's script posts them, answers as the JavaScript it would run. */
static char *call(zebkit_viewer *viewer, const char *origin, const char *message, char **app) {
    return zebkit_viewer_handle(viewer, origin, message, app);
}

static void test_viewer(void) {
    zebkit_viewer *viewer = zebkit_viewer_new();
    zebkit_viewer_grant(viewer, "app://reader", ZEBKIT_ALLOW_ALL, NULL);

    char *script = zebkit_viewer_script("(m) => window.webkit.messageHandlers.zebkit.postMessage(m)");
    check(script && strstr(script, "__zebkit_answer") && strstr(script, "makePdf") && strstr(script, "postMessage(m)"),
          "viewer script", "no page script");
    zebkit_free(script);

    char *app = NULL;
    /* "hello" as base64, through zstd and back: two calls. */
    char *answer = call(viewer, "app://reader",
                        "{\"zebkit\":1,\"id\":1,\"module\":\"crypto\",\"method\":\"digest\","
                        "\"args\":[\"SHA-256\",{\"$bytes\":\"YWJj\"}]}", &app);
    /* SHA-256("abc") starts ba7816bf: base64 "ungWv..." */
    check(answer && strstr(answer, "__zebkit_answer(1, true") && strstr(answer, "ungWv"), "viewer crypto",
          answer ? answer : "no answer");
    zebkit_free(answer);

    answer = call(viewer, "app://reader",
                  "{\"zebkit\":1,\"id\":2,\"module\":\"url\",\"method\":\"parse\",\"args\":[\"../b?q=1\",\"https://example.com/a/c\"]}",
                  &app);
    check(answer && strstr(answer, "https://example.com/b?q=1"), "viewer url", answer);
    zebkit_free(answer);

    /* zebkit.pdf: open the test page's bytes, read its text, close it. */
    size_t b64_size = 0;
    char *error = NULL;
    /* Base64 of kPdf, through the services' own encoder: none; make it here. */
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = sizeof(kPdf) - 1;
    char *b64 = malloc((n + 2) / 3 * 4 + 1);
    for (size_t i = 0, o = 0; i < n; i += 3) {
        unsigned v = (unsigned char)kPdf[i] << 16 | (i + 1 < n ? (unsigned char)kPdf[i + 1] << 8 : 0) |
                     (i + 2 < n ? (unsigned char)kPdf[i + 2] : 0);
        b64[o++] = table[v >> 18 & 63];
        b64[o++] = table[v >> 12 & 63];
        b64[o++] = i + 1 < n ? table[v >> 6 & 63] : '=';
        b64[o++] = i + 2 < n ? table[v & 63] : '=';
        b64[o] = 0;
        b64_size = o;
    }
    (void)b64_size;
    (void)error;
    char *open = malloc(strlen(b64) + 200);
    sprintf(open, "{\"zebkit\":1,\"id\":3,\"module\":\"pdf\",\"method\":\"open\",\"args\":[{\"$bytes\":\"%s\"},null]}", b64);
    answer = call(viewer, "app://reader", open, &app);
    check(answer && strstr(answer, "__zebkit_answer(3, true, \"1\")"), "viewer pdf open", answer);
    zebkit_free(answer);
    free(open);
    free(b64);
    answer = call(viewer, "app://reader",
                  "{\"zebkit\":1,\"id\":4,\"module\":\"pdf\",\"method\":\"text\",\"args\":[1,0]}", &app);
    check(answer && strstr(answer, "Hello zebkit"), "viewer pdf text", answer);
    zebkit_free(answer);
    answer = call(viewer, "app://reader",
                  "{\"zebkit\":1,\"id\":5,\"module\":\"pdf\",\"method\":\"close\",\"args\":[1]}", &app);
    check(answer && strstr(answer, "__zebkit_answer(5, true, null)"), "viewer pdf close", answer);
    zebkit_free(answer);

    /* zebkit.app is the application's: handed back, then answered. */
    answer = call(viewer, "app://reader",
                  "{\"zebkit\":1,\"id\":6,\"module\":\"app\",\"method\":\"save\",\"args\":[\"note\",{\"$bytes\":\"AQI=\"}]}",
                  &app);
    check(!answer && app && strstr(app, "\"method\":\"save\"") && strstr(app, "AQI="), "viewer app call", app);
    zebkit_free(app);
    app = NULL;
    answer = zebkit_viewer_answer(6, true, "{\"saved\":true}");
    check(answer && strstr(answer, "__zebkit_answer(6, true, \"{\\\"saved\\\":true}\")"), "viewer app answer", answer);
    zebkit_free(answer);

    /* Another origin gets nothing it was not given. */
    answer = call(viewer, "https://elsewhere.example",
                  "{\"zebkit\":1,\"id\":7,\"module\":\"crypto\",\"method\":\"randomBytes\",\"args\":[4]}", &app);
    check(answer && strstr(answer, "__zebkit_answer(7, false") && strstr(answer, "not allowed"), "viewer origin",
          answer);
    zebkit_free(answer);
    zebkit_viewer_free(viewer);
}

int main(int argc, char **argv) {
    printf("zebkit %s, %s\n", zebkit_version(), zebkit_engine_version());
    test_services();
    test_pdf();
    test_icu(argc > 1 ? argv[1] : NULL);
    test_viewer();
    return failures ? 1 : 0;
}
