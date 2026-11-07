/* The libraries inside the engine (<zebkit/vendor/...>), used from C as an
 * application uses them: each through its own API, linked against
 * libzebkit and nothing else. Prints one line per check; exit 0 when all
 * pass. scripts/engine-test.sh runs it, and checks that the engine
 * exports none of the libraries' own names. */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <zebkit/services.h>
#include <zebkit/vendor/boringssl.h>
#include <zebkit/vendor/brotli.h>
#include <zebkit/vendor/dav1d.h>
#include <zebkit/vendor/ffmpeg.h>
#include <zebkit/vendor/freetype.h>
#include <zebkit/vendor/leveldb.h>
#include FT_OUTLINE_H
#include FT_MODULE_H
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
    /* zebkit's FFmpeg: what applications need besides playing. */
    check(avcodec_find_decoder(AV_CODEC_ID_VP9) != NULL && avcodec_find_decoder(AV_CODEC_ID_VP8) != NULL,
          "ffmpeg: VP8 and VP9 decoders", NULL);
    check(av_guess_format("mp4", NULL, NULL) != NULL && av_guess_format("webm", NULL, NULL) != NULL &&
              av_guess_format("matroska", NULL, NULL) != NULL,
          "ffmpeg: MP4, WebM and Matroska muxers", NULL);
    AVFormatContext *output = NULL;
    int made = avformat_alloc_output_context2(&output, NULL, "mp4", "out.mp4");
    check(made >= 0 && output, "ffmpeg: avformat_alloc_output_context2(mp4)", NULL);
    avformat_free_context(output);
    AVIOContext *io = NULL;
#ifdef _WIN32
    const char *null_device = "NUL";
#else
    const char *null_device = "/dev/null";
#endif
    check(avio_open(&io, null_device, AVIO_FLAG_WRITE) >= 0, "ffmpeg: the file protocol", NULL);
    avio_closep(&io);
    /* 48 kHz stereo to 16 kHz mono, as speech recognition wants it. */
    SwrContext *resampler = NULL;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO, mono = AV_CHANNEL_LAYOUT_MONO;
    int set = swr_alloc_set_opts2(&resampler, &mono, AV_SAMPLE_FMT_FLT, 16000, &stereo, AV_SAMPLE_FMT_FLT, 48000, 0,
                                  NULL);
    float in[960 * 2], out[400];
    for (int i = 0; i < 960 * 2; i++)
        in[i] = 0.5f;
    const uint8_t *in_planes[] = {(const uint8_t *)in};
    uint8_t *out_planes[] = {(uint8_t *)out};
    int converted = set >= 0 && swr_init(resampler) >= 0 ? swr_convert(resampler, out_planes, 400, in_planes, 960) : -1;
    char detail[48];
    snprintf(detail, sizeof(detail), "%d samples", converted);
    check(converted > 250 && converted <= 320, "libswresample: 48 kHz stereo to 16 kHz mono", detail);
    swr_free(&resampler);
    /* A 64 by 32 YUV picture to RGB, half the size. */
    struct SwsContext *scaler =
        sws_getContext(64, 32, AV_PIX_FMT_YUV420P, 32, 16, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t y[64 * 32], u[32 * 16], v[32 * 16], rgb[32 * 16 * 3];
    memset(y, 200, sizeof(y));
    memset(u, 128, sizeof(u));
    memset(v, 128, sizeof(v));
    const uint8_t *src[] = {y, u, v};
    const int src_stride[] = {64, 32, 32};
    uint8_t *dst[] = {rgb};
    const int dst_stride[] = {32 * 3};
    int rows = scaler ? sws_scale(scaler, src, src_stride, 0, 32, dst, dst_stride) : -1;
    snprintf(detail, sizeof(detail), "%d rows, %d,%d,%d", rows, rgb[0], rgb[1], rgb[2]);
    check(rows == 16 && rgb[0] > 200 && rgb[1] > 200 && rgb[2] > 200, "libswscale: YUV to RGB, scaled", detail);
    sws_freeContext(scaler);
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

/* The application's own functions, called by the libraries: allocators,
 * destroy callbacks, font and unicode functions, a stream. With CFI on
 * indirect calls (profiles/slim-match.gn), a check on such a call would
 * fail, as these are not in the engine (patches/1078). */
static int app_calls;

static voidpf app_zalloc(voidpf opaque, uInt items, uInt size) {
    (void)opaque;
    ++app_calls;
    return calloc(items, size);
}

static void app_zfree(voidpf opaque, voidpf address) {
    (void)opaque;
    ++app_calls;
    free(address);
}

static void app_destroy(void *data) {
    ++*(int *)data;
}

static hb_bool_t app_nominal_glyph(hb_font_t *font, void *data, hb_codepoint_t unicode,
                                   hb_codepoint_t *glyph, void *user_data) {
    (void)font, (void)data, (void)user_data;
    ++app_calls;
    *glyph = unicode;
    return 1;
}

static hb_position_t app_advance(hb_font_t *font, void *data, hb_codepoint_t glyph, void *user_data) {
    (void)font, (void)data, (void)glyph, (void)user_data;
    ++app_calls;
    return 1000;
}

static hb_unicode_general_category_t app_category(hb_unicode_funcs_t *funcs, hb_codepoint_t unicode,
                                                  void *user_data) {
    (void)funcs, (void)unicode, (void)user_data;
    ++app_calls;
    return HB_UNICODE_GENERAL_CATEGORY_LOWERCASE_LETTER;
}

static void *app_ft_alloc(FT_Memory memory, long size) {
    (void)memory;
    ++app_calls;
    return malloc((size_t)size);
}

static void app_ft_free(FT_Memory memory, void *block) {
    (void)memory;
    free(block);
}

static void *app_ft_realloc(FT_Memory memory, long cur_size, long new_size, void *block) {
    (void)memory, (void)cur_size;
    return realloc(block, (size_t)new_size);
}

static void test_callbacks(void) {
    unsigned char packed[512], unpacked[512];
    z_stream deflater = {0}, inflater = {0};
    deflater.zalloc = inflater.zalloc = app_zalloc;
    deflater.zfree = inflater.zfree = app_zfree;
    app_calls = 0;
    int ok = deflateInit(&deflater, 9) == Z_OK;
    deflater.next_in = (Bytef *)text, deflater.avail_in = sizeof(text);
    deflater.next_out = packed, deflater.avail_out = sizeof(packed);
    ok = ok && deflate(&deflater, Z_FINISH) == Z_STREAM_END && deflateEnd(&deflater) == Z_OK;
    ok = ok && inflateInit(&inflater) == Z_OK;
    inflater.next_in = packed, inflater.avail_in = sizeof(packed) - deflater.avail_out;
    inflater.next_out = unpacked, inflater.avail_out = sizeof(unpacked);
    ok = ok && inflate(&inflater, Z_FINISH) == Z_STREAM_END && inflateEnd(&inflater) == Z_OK &&
         memcmp(unpacked, text, sizeof(text)) == 0;
    check(ok && app_calls >= 4, "zlib: the application's zalloc and zfree", NULL);

    int destroyed = 0;
    hb_blob_t *blob = hb_blob_create(text, sizeof(text), HB_MEMORY_MODE_READONLY, &destroyed, app_destroy);
    hb_blob_destroy(blob);
    check(destroyed == 1, "harfbuzz: a blob's destroy callback", NULL);

    app_calls = 0;
    destroyed = 0;
    hb_font_funcs_t *funcs = hb_font_funcs_create();
    hb_font_funcs_set_nominal_glyph_func(funcs, app_nominal_glyph, &destroyed, app_destroy);
    hb_font_funcs_set_glyph_h_advance_func(funcs, app_advance, NULL, NULL);
    // A face of no font at all: the empty face's font could not be changed.
    hb_blob_t *nothing = hb_blob_create(text, sizeof(text), HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *face = hb_face_create(nothing, 0);
    hb_font_t *font = hb_font_create(face);
    hb_font_set_funcs(font, funcs, &destroyed, app_destroy);
    hb_unicode_funcs_t *unicode = hb_unicode_funcs_create(hb_unicode_funcs_get_default());
    hb_unicode_funcs_set_general_category_func(unicode, app_category, &destroyed, app_destroy);
    hb_buffer_t *buffer = hb_buffer_create();
    hb_buffer_set_unicode_funcs(buffer, unicode);
    hb_buffer_add_utf8(buffer, "zebkit", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, NULL, 0);
    unsigned int count = 0;
    hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &count);
    char detail[64];
    snprintf(detail, sizeof(detail), "%u glyphs, advance %d, %d calls", count, count ? positions[0].x_advance : 0,
             app_calls);
    ok = count == 6 && positions[0].x_advance == 1000 && app_calls >= 12;
    hb_buffer_destroy(buffer);
    hb_unicode_funcs_destroy(unicode);
    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(nothing);
    hb_font_funcs_destroy(funcs);
    check(ok && destroyed == 3, "harfbuzz: the application's font, unicode and destroy functions", detail);

    app_calls = 0;
    struct FT_MemoryRec_ memory = {NULL, app_ft_alloc, app_ft_free, app_ft_realloc};
    FT_Library library = NULL;
    ok = FT_New_Library(&memory, &library) == 0;
    if (ok) {
        FT_Add_Default_Modules(library);
        FT_Done_Library(library);
    }
    check(ok && app_calls > 0, "freetype: the application's memory functions", NULL);
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

static void test_freetype(const char *font_path) {
    FT_Library library = NULL;
    int ok = FT_Init_FreeType(&library) == 0;
    check(ok, "freetype: independent library", NULL);
    if (!ok) return;
    FT_Int major = 0, minor = 0, patch = 0;
    FT_Library_Version(library, &major, &minor, &patch);
    check(major == FREETYPE_MAJOR && minor == FREETYPE_MINOR && patch == FREETYPE_PATCH,
          "freetype: headers match the linked library", NULL);
    FT_Outline outline = {0};
    ok = FT_Outline_New(library, 4, 1, &outline) == 0;
    if (ok) {
        FT_Vector square[] = {{128,128}, {896,128}, {896,896}, {128,896}};
        memcpy(outline.points, square, sizeof(square));
        memset(outline.tags, FT_CURVE_TAG_ON, 4);
        outline.contours[0] = 3;
        unsigned char pixels[16 * 16] = {0};
        FT_Bitmap bitmap = {0};
        bitmap.rows = bitmap.width = bitmap.pitch = 16;
        bitmap.buffer = pixels;
        bitmap.num_grays = 256;
        bitmap.pixel_mode = FT_PIXEL_MODE_GRAY;
        FT_BBox bounds = {0};
        FT_Outline_Get_CBox(&outline, &bounds);
        ok = FT_Outline_Get_Bitmap(library, &outline, &bitmap) == 0 &&
             pixels[8 * 16 + 8] > 250 && pixels[0] == 0 && bounds.xMin == 128 && bounds.xMax == 896;
        FT_Outline_Done(library, &outline);
    }
    check(ok, "freetype: outline rasterization and caller-owned bitmap", NULL);
    if (font_path) {
        FT_Face face = NULL;
        ok = FT_New_Face(library, font_path, 0, &face) == 0;
        if (ok) {
            ok = FT_Set_Pixel_Sizes(face, 0, 24) == 0 &&
                 FT_Get_Char_Index(face, 'A') != 0 && FT_Load_Char(face, 'A', FT_LOAD_RENDER) == 0 &&
                 face->glyph->bitmap.width > 0 && face->glyph->bitmap.rows > 0;
            FT_Done_Face(face);
        }
        check(ok, "freetype: TrueType font loaded and glyph rendered", NULL);
    }
    FT_Done_FreeType(library);
}

// With no engine started: no thread pool for its compactions, the C locale, and
// a name that is not ASCII.
static void test_leveldb(void) {
#ifdef _WIN32
    char parent[MAX_PATH + 32];
    snprintf(parent, sizeof(parent), "%s\\zebkit-leveldb-XXXXXX", getenv("TEMP"));
#else
    char parent[] = "/tmp/zebkit-leveldb-XXXXXX";
#endif
    check(mkdtemp(parent) != NULL, "leveldb: temporary directory", NULL);
    char path[128];
    snprintf(path, sizeof(path), "%s/\xE6\x95\xB0\xE6\x8D\xAE", parent);

    leveldb_options_t *options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 1);
    leveldb_options_set_write_buffer_size(options, 64 * 1024);
    leveldb_writeoptions_t *sync = leveldb_writeoptions_create();
    leveldb_writeoptions_set_sync(sync, 1);
    leveldb_writeoptions_t *async = leveldb_writeoptions_create();
    leveldb_readoptions_t *read = leveldb_readoptions_create();

    char *error = NULL;
    leveldb_t *db = leveldb_open(options, path, &error);
    check(db && !error, "leveldb: opens a non-ASCII path under the C locale", error);
    if (!db) return;

    leveldb_t *second = leveldb_open(options, path, &error);
    check(!second && error, "leveldb: a second open is refused by the lock", NULL);
    if (second) leveldb_close(second);
    leveldb_free(error);
    error = NULL;

    // More than the write buffer many times over: flushes and compactions, on
    // their own thread here.
    char key[32];
    char value[1024];
    memset(value, 'v', sizeof(value));
    int written = 1;
    for (int i = 0; i < 2000 && written; i++) {
        snprintf(key, sizeof(key), "key%06d", i);
        leveldb_put(db, async, key, strlen(key), value, sizeof(value), &error);
        written = !error;
    }
    check(written, "leveldb: writes through compactions", error);
    leveldb_free(error);
    error = NULL;

    const char binary[] = {'a', 0, 'b'};
    leveldb_put(db, sync, binary, sizeof(binary), binary, sizeof(binary), &error);
    size_t size = 0;
    char *got = leveldb_get(db, read, binary, sizeof(binary), &size, &error);
    check(got && size == sizeof(binary) && !memcmp(got, binary, size), "leveldb: binary keys and values", error);
    leveldb_free(got);

    const leveldb_snapshot_t *snapshot = leveldb_create_snapshot(db);
    leveldb_writebatch_t *batch = leveldb_writebatch_create();
    leveldb_writebatch_put(batch, "batch", 5, "1", 1);
    leveldb_writebatch_delete(batch, "key000000", 9);
    leveldb_write(db, sync, batch, &error);
    leveldb_writebatch_destroy(batch);
    check(!error, "leveldb: a batch", error);

    got = leveldb_get(db, read, "key000000", 9, &size, &error);
    check(!got, "leveldb: the batch deleted it", NULL);
    leveldb_free(got);
    leveldb_readoptions_t *old = leveldb_readoptions_create();
    leveldb_readoptions_set_snapshot(old, snapshot);
    got = leveldb_get(db, old, "key000000", 9, &size, &error);
    check(got != NULL, "leveldb: the snapshot still has it", NULL);
    leveldb_free(got);
    leveldb_readoptions_destroy(old);
    leveldb_release_snapshot(db, snapshot);

    int count = 0;
    leveldb_iterator_t *iterator = leveldb_create_iterator(db, read);
    for (leveldb_iter_seek(iterator, "key", 3); leveldb_iter_valid(iterator); leveldb_iter_next(iterator))
        count++;
    leveldb_iter_destroy(iterator);
    check(count == 1999, "leveldb: an iterator sees every key in order", NULL);

    leveldb_close(db);
    db = leveldb_open(options, path, &error);
    check(db && !error, "leveldb: reopens", error);
    if (db) {
        got = leveldb_get(db, read, "batch", 5, &size, &error);
        check(got && size == 1 && *got == '1', "leveldb: a synced write survives closing", error);
        leveldb_free(got);
        leveldb_close(db);
    }
    leveldb_destroy_db(options, path, &error);
    check(!error, "leveldb: destroys the database", error);
    rmdir(parent);
    leveldb_readoptions_destroy(read);
    leveldb_writeoptions_destroy(async);
    leveldb_writeoptions_destroy(sync);
    leveldb_options_destroy(options);
}

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

int main(int argc, char **argv) {
    test_zlib();
    test_zstd();
    test_brotli();
    test_boringssl();
    test_leveldb();
    test_sqlite();
    test_icu();
    test_libxml();
    test_media();
    test_images();
    test_freetype(argc > 1 ? argv[1] : NULL);
    test_pdfium();
    test_callbacks();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
