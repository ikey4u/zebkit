/* The libraries inside the engine (<zebkit/vendor/...>), used from C as an
 * application uses them: each through its own API, linked against
 * libzebkit and nothing else. Prints one line per check; exit 0 when all
 * pass. scripts/engine-test.sh runs it, and checks that the engine
 * exports none of the libraries' own names. */

#include <stdio.h>
#include <string.h>

#include <zebkit/services.h>
#include <zebkit/vendor/boringssl.h>
#include <zebkit/vendor/brotli.h>
#include <zebkit/vendor/dav1d.h>
#include <zebkit/vendor/ffmpeg.h>
#include <zebkit/vendor/harfbuzz.h>
#include <zebkit/vendor/icu.h>
#include <zebkit/vendor/libjpeg.h>
#include <zebkit/vendor/libvpx.h>
#include <zebkit/vendor/libwebp.h>
#include <zebkit/vendor/libxml.h>
#include <zebkit/vendor/opus.h>
#include <zebkit/vendor/pdfium.h>
#include <zebkit/vendor/sqlite.h>
#include <zebkit/vendor/zlib.h>
#include <zebkit/vendor/zstd.h>

static int failures, checks;

static void check(int ok, const char *what, const char *detail) {
    ++checks;
    if (!ok)
        ++failures;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

static const char text[] =
    "zebkit exposes the libraries inside the engine to applications. "
    "zebkit exposes the libraries inside the engine to applications.";

static void test_zlib(void) {
    unsigned char packed[512], unpacked[512];
    uLongf packed_size = sizeof(packed), unpacked_size = sizeof(unpacked);
    int ok = compress2(packed, &packed_size, (const Bytef *)text, sizeof(text), 9) == Z_OK &&
             uncompress(unpacked, &unpacked_size, packed, packed_size) == Z_OK &&
             unpacked_size == sizeof(text) && memcmp(unpacked, text, sizeof(text)) == 0;
    check(ok, "zlib: compress2 and uncompress", zlibVersion());
}

static void test_zstd(void) {
    char packed[512], unpacked[512];
    size_t packed_size = ZSTD_compress(packed, sizeof(packed), text, sizeof(text), 19);
    size_t unpacked_size = ZSTD_decompress(unpacked, sizeof(unpacked), packed, packed_size);
    check(!ZSTD_isError(packed_size) && unpacked_size == sizeof(text) && memcmp(unpacked, text, sizeof(text)) == 0,
          "zstd: ZSTD_compress and ZSTD_decompress", ZSTD_versionString());
}

static void test_brotli(void) {
    uint8_t packed[512], unpacked[512];
    size_t packed_size = sizeof(packed), unpacked_size = sizeof(unpacked);
    int ok = BrotliEncoderCompress(9, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_TEXT, sizeof(text), (const uint8_t *)text,
                                   &packed_size, packed) &&
             BrotliDecoderDecompress(packed_size, packed, &unpacked_size, unpacked) == BROTLI_DECODER_RESULT_SUCCESS &&
             unpacked_size == sizeof(text) && memcmp(unpacked, text, sizeof(text)) == 0;
    check(ok, "brotli: BrotliEncoderCompress and BrotliDecoderDecompress", NULL);
}

static void test_boringssl(void) {
    unsigned char digest[SHA512_DIGEST_LENGTH];
    SHA256((const uint8_t *)"abc", 3, digest);
    static const unsigned char expected[4] = {0xba, 0x78, 0x16, 0xbf};
    check(memcmp(digest, expected, 4) == 0, "boringssl: SHA256(\"abc\")", NULL);
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    unsigned length = 0;
    int ok = context && EVP_DigestInit_ex(context, EVP_sha512(), NULL) && EVP_DigestUpdate(context, text, 10) &&
             EVP_DigestFinal_ex(context, digest, &length) && length == 64;
    EVP_MD_CTX_free(context);
    check(ok, "boringssl: EVP_Digest* with SHA-512", NULL);
    SSL_CTX *tls = SSL_CTX_new(TLS_method());
    check(tls != NULL, "boringssl: SSL_CTX_new(TLS_method())", NULL);
    SSL_CTX_free(tls);
}

static int row_count;
static int count_rows(void *data, int columns, char **values, char **names) {
    (void)data, (void)columns, (void)values, (void)names;
    ++row_count;
    return 0;
}

static void test_sqlite(void) {
    sqlite3 *db = NULL;
    /* Chromium builds SQLite with SQLITE_OMIT_AUTOINIT. */
    int ok = sqlite3_initialize() == SQLITE_OK && sqlite3_open(":memory:", &db) == SQLITE_OK &&
             sqlite3_exec(db, "CREATE TABLE t (a INTEGER, b TEXT); INSERT INTO t VALUES (1, 'x'), (2, 'y');", NULL,
                          NULL, NULL) == SQLITE_OK &&
             sqlite3_exec(db, "SELECT * FROM t WHERE a > 0", count_rows, NULL, NULL) == SQLITE_OK && row_count == 2;
    sqlite3_stmt *statement = NULL;
    ok = ok && sqlite3_prepare_v2(db, "SELECT b FROM t WHERE a = ?", -1, &statement, NULL) == SQLITE_OK &&
         sqlite3_bind_int(statement, 1, 2) == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW &&
         strcmp((const char *)sqlite3_column_text(statement, 0), "y") == 0;
    sqlite3_finalize(statement);
    sqlite3_close(db);
    check(ok, "sqlite: exec, prepare, bind, step", sqlite3_libversion());
}

static void test_icu(void) {
    check(zebkit_icu_init(), "icu: zebkit_icu_init", NULL);
    UErrorCode status = U_ZERO_ERROR;
    UChar text16[64], upper[64];
    u_strFromUTF8(text16, 64, NULL, "stra\xc3\x9f" "e", -1, &status);
    int32_t length = u_strToUpper(upper, 64, text16, -1, "de", &status);
    char utf8[64];
    u_strToUTF8(utf8, sizeof(utf8), NULL, upper, length, &status);
    check(U_SUCCESS(status) && strcmp(utf8, "STRASSE") == 0, "icu: u_strToUpper(\"stra\xc3\x9f" "e\", de)", utf8);

    UChar words16[64];
    status = U_ZERO_ERROR;
    u_strFromUTF8(words16, 64, NULL, "Hello world, \xe4\xbd\xa0\xe5\xa5\xbd", -1, &status);
    UBreakIterator *iterator = ubrk_open(UBRK_WORD, "en", words16, -1, &status);
    int words = 0;
    if (U_SUCCESS(status)) {
        for (int32_t at = ubrk_first(iterator); at != UBRK_DONE; at = ubrk_next(iterator)) {
            if (ubrk_getRuleStatus(iterator) != UBRK_WORD_NONE)
                ++words;
        }
        ubrk_close(iterator);
    }
    char found[16];
    snprintf(found, sizeof(found), "%d words", words);
    check(U_SUCCESS(status) && words == 3, "icu: ubrk word breaks, English and Chinese", found);
}

static void test_libxml(void) {
    xmlDocPtr doc = xmlReadMemory("<a><b>1</b><b>2</b></a>", 23, "test.xml", NULL, 0);
    xmlXPathContextPtr context = doc ? xmlXPathNewContext(doc) : NULL;
    xmlXPathObjectPtr result = context ? xmlXPathEvalExpression((const xmlChar *)"//b", context) : NULL;
    int ok = result && result->nodesetval && result->nodesetval->nodeNr == 2;
    xmlXPathFreeObject(result);
    xmlXPathFreeContext(context);
    xmlFreeDoc(doc);
    check(ok, "libxml: xmlReadMemory and XPath", LIBXML_DOTTED_VERSION);
}

static void test_media(void) {
    check(avcodec_find_decoder(AV_CODEC_ID_H264) != NULL, "ffmpeg: avcodec_find_decoder(H264)", av_version_info());
    check(avcodec_find_decoder(AV_CODEC_ID_AAC) != NULL, "ffmpeg: avcodec_find_decoder(AAC)", NULL);
    check(strlen(dav1d_version()) > 0, "dav1d: dav1d_version", dav1d_version());
    check(strlen(opus_get_version_string()) > 0, "opus: opus_get_version_string", opus_get_version_string());
    int error = 0;
    OpusEncoder *encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
    check(encoder && error == OPUS_OK, "opus: opus_encoder_create", NULL);
    opus_encoder_destroy(encoder);
    check(strlen(vpx_codec_version_str()) > 0, "libvpx: vpx_codec_version_str", vpx_codec_version_str());
    check(vpx_codec_vp9_dx() != NULL && vpx_codec_vp8_cx() != NULL, "libvpx: VP9 decoder and VP8 encoder", NULL);
}

static void test_images(void) {
    char version[32];
    int v = WebPGetDecoderVersion();
    snprintf(version, sizeof(version), "%d.%d.%d", v >> 16, (v >> 8) & 255, v & 255);
    check(v > 0, "libwebp: WebPGetDecoderVersion", version);
    struct jpeg_compress_struct compress;
    struct jpeg_error_mgr errors;
    compress.err = jpeg_std_error(&errors);
    jpeg_create_compress(&compress);
    jpeg_destroy_compress(&compress);
    check(1, "libjpeg: jpeg_create_compress", NULL);
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, "zebkit", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    check(hb_buffer_get_length(buffer) == 6 && hb_buffer_get_direction(buffer) == HB_DIRECTION_LTR,
          "harfbuzz: a buffer of text", hb_version_string());
    hb_buffer_destroy(buffer);
}

/* A one-page PDF with "Hello zebkit" on it, its cross-reference table
 * left for PDFium to rebuild. */
static const char pdf[] =
    "%PDF-1.4\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
    "4 0 obj << /Length 44 >> stream\n"
    "BT /F1 18 Tf 20 40 Td (Hello zebkit) Tj ET\n"
    "endstream endobj\n"
    "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

static void test_pdfium(void) {
    FPDF_InitLibrary();
    FPDF_DOCUMENT document = FPDF_LoadMemDocument(pdf, (int)sizeof(pdf) - 1, NULL);
    check(document != NULL, "pdfium: FPDF_LoadMemDocument", NULL);
    if (!document) {
        FPDF_DestroyLibrary();
        return;
    }
    check(FPDF_GetPageCount(document) == 1, "pdfium: FPDF_GetPageCount", NULL);
    FPDF_PAGE page = FPDF_LoadPage(document, 0);
    check(page && FPDF_GetPageWidthF(page) == 200 && FPDF_GetPageHeightF(page) == 100,
          "pdfium: FPDF_LoadPage, 200 by 100 points", NULL);
    FPDF_TEXTPAGE text = FPDFText_LoadPage(page);
    unsigned short utf16[32] = {0};
    int count = FPDFText_GetText(text, 0, 12, utf16);
    char found[16] = {0};
    for (int i = 0; i < 12 && i < count; i++)
        found[i] = (char)utf16[i];
    check(strcmp(found, "Hello zebkit") == 0, "pdfium: FPDFText_GetText", found);
    FPDFText_ClosePage(text);
    FPDF_BITMAP bitmap = FPDFBitmap_Create(200, 100, 0);
    FPDFBitmap_FillRect(bitmap, 0, 0, 200, 100, 0xFFFFFFFF);
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, 200, 100, 0, FPDF_ANNOT);
    const unsigned char *pixels = FPDFBitmap_GetBuffer(bitmap);
    int stride = FPDFBitmap_GetStride(bitmap), dark = 0;
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 200; x++)
            if (pixels[y * stride + x * 4] < 128)
                dark++;
    char detail[32];
    snprintf(detail, sizeof(detail), "%d dark pixels", dark);
    check(dark > 100, "pdfium: FPDF_RenderPageBitmap draws the text", detail);
    FPDFBitmap_Destroy(bitmap);
    FPDF_ClosePage(page);
    FPDF_CloseDocument(document);
    FPDF_DestroyLibrary();
}

int main(void) {
    test_zlib();
    test_zstd();
    test_brotli();
    test_boringssl();
    test_sqlite();
    test_icu();
    test_libxml();
    test_media();
    test_images();
    test_pdfium();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
