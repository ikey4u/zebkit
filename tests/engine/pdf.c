/* <zebkit/pdf.h> without the engine running: a document made here, opened,
 * read, rendered, searched, changed, saved and opened again. Prints one
 * line per check; exit 0 when all pass. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/pdf.h>
#include <zebkit/services.h>

static int failures, checks;

static void check(int ok, const char *what, const char *detail) {
    checks++;
    if (!ok)
        failures++;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
}

/* Two pages: "Hello zebkit" and a link out, and "Second page zebkit". */
static const char document[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /Lang (en-US) >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R 6 0 R] /Count 2 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> /Annots [8 0 R] >> endobj\n"
    "4 0 obj << >> stream\n"
    "BT /F1 24 Tf 20 120 Td (Hello zebkit) Tj ET\n"
    "endstream endobj\n"
    "5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "6 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Rotate 90 /Contents 7 0 R"
    " /Resources << /Font << /F1 5 0 R >> >> >> endobj\n"
    "7 0 obj << >> stream\n"
    "BT /F1 18 Tf 20 120 Td (Second page zebkit) Tj ET\n"
    "endstream endobj\n"
    "8 0 obj << /Type /Annot /Subtype /Link /Rect [20 20 120 40]"
    " /A << /S /URI /URI (https://example.com/) >> >> endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

static int contains(const char *text, const char *part) {
    return text && strstr(text, part) != NULL;
}

int main(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open("not a pdf", 9, NULL, &error);
    check(pdf == NULL && error != NULL, "zebkit_pdf_open refuses what is not a PDF", error);
    zebkit_free(error);
    error = NULL;

    pdf = zebkit_pdf_open(document, sizeof(document) - 1, NULL, &error);
    check(pdf != NULL, "zebkit_pdf_open", error);
    if (!pdf)
        return 1;

    char *info = zebkit_pdf_info(pdf);
    check(contains(info, "\"pages\":2") && contains(info, "\"language\":\"en-US\"") &&
              contains(info, "\"encrypted\":false"),
          "zebkit_pdf_info", info);
    zebkit_free(info);
    check(zebkit_pdf_page_count(pdf) == 2, "zebkit_pdf_page_count", NULL);

    char *page = zebkit_pdf_page(pdf, 1, &error);
    check(contains(page, "\"rotation\":90") && contains(page, "\"width\":200.0") && contains(page, "\"height\":300.0") &&
              contains(page, "\"mediaBox\":[0.0,0.0,300.0,200.0]"),
          "zebkit_pdf_page", page);
    zebkit_free(page);
    page = zebkit_pdf_page(pdf, 2, &error);
    check(page == NULL && error != NULL, "zebkit_pdf_page refuses a page that is not there", error);
    zebkit_free(error);
    error = NULL;

    int width = 0, height = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
    int dark = 0;
    for (int i = 0; pixels && i < width * height; i++)
        if (pixels[i * 4] < 128 && pixels[i * 4 + 3] == 255)
            dark++;
    char detail[96];
    snprintf(detail, sizeof(detail), "%dx%d, %d dark pixels", width, height, dark);
    check(pixels && width == 300 && height == 200 && dark > 200, "zebkit_pdf_render", detail);
    zebkit_free(pixels);

    /* Rotated by the page's own 90 degrees, at twice the size, on red. */
    zebkit_pdf_render_options options = {.scale = 2, .background = 0xff0000ff};
    pixels = zebkit_pdf_render(pdf, 1, &options, &width, &height, &error);
    snprintf(detail, sizeof(detail), "%dx%d, first pixel %d,%d,%d,%d", width, height, pixels ? pixels[0] : -1,
             pixels ? pixels[1] : -1, pixels ? pixels[2] : -1, pixels ? pixels[3] : -1);
    check(pixels && width == 400 && height == 600 && pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0 &&
              pixels[3] == 255,
          "zebkit_pdf_render: rotation, scale, background", detail);
    zebkit_free(pixels);

    /* A tile: the top left 50 by 40 pixels. */
    zebkit_pdf_render_options tile = {.scale = 1, .background = 0xffffffff, .x = 10, .y = 20, .width = 50, .height = 40};
    pixels = zebkit_pdf_render(pdf, 0, &tile, &width, &height, &error);
    check(pixels && width == 50 && height == 40, "zebkit_pdf_render: a tile", NULL);
    if (pixels) {
        size_t size = 0;
        uint8_t *png = zebkit_image_encode(ZEBKIT_IMAGE_PNG, pixels, width, height, 0, &size, NULL);
        check(png && size > 8, "the tile as PNG (zebkit_image_encode)", NULL);
        zebkit_free(png);
    }
    zebkit_free(pixels);

    char *text = zebkit_pdf_text(pdf, 0, &error);
    check(contains(text, "Hello zebkit"), "zebkit_pdf_text", text);
    zebkit_free(text);
    char *layout = zebkit_pdf_text_layout(pdf, 0, &error);
    check(contains(layout, "\"c\":\"H\"") && contains(layout, "\"font\":\"Helvetica\"") &&
              contains(layout, "\"size\":24.0"),
          "zebkit_pdf_text_layout", NULL);
    zebkit_free(layout);
    char *rects = zebkit_pdf_text_rects(pdf, 0, 0, 5, &error);
    check(rects && rects[0] == '[' && rects[1] == '[', "zebkit_pdf_text_rects", rects);
    zebkit_free(rects);
    int at = zebkit_pdf_char_at(pdf, 0, 25, 128, 2);
    snprintf(detail, sizeof(detail), "%d", at);
    check(at == 0, "zebkit_pdf_char_at: the H", detail);
    check(zebkit_pdf_char_at(pdf, 0, 280, 10, 1) == -1, "zebkit_pdf_char_at: nothing there", NULL);

    char *found = zebkit_pdf_search(pdf, "ZEBKIT", 0, 0, 0, &error);
    check(contains(found, "\"page\":0") && contains(found, "\"page\":1") && contains(found, "\"count\":6") &&
              contains(found, "Second page"),
          "zebkit_pdf_search, both pages", found);
    zebkit_free(found);
    found = zebkit_pdf_search(pdf, "ZEBKIT", ZEBKIT_PDF_SEARCH_CASE, 0, 0, &error);
    check(found && strcmp(found, "[]") == 0, "zebkit_pdf_search, matching case", found);
    zebkit_free(found);

    char *links = zebkit_pdf_links(pdf, 0, &error);
    check(contains(links, "\"url\":\"https://example.com/\"") && contains(links, "[20.0,20.0,120.0,40.0]"),
          "zebkit_pdf_links", links);
    zebkit_free(links);
    char *outline = zebkit_pdf_outline(pdf, &error);
    check(outline && strcmp(outline, "[]") == 0, "zebkit_pdf_outline, none", outline);
    zebkit_free(outline);

    /* Annotations: the link the document has, then one of each kind made. */
    char *annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check(contains(annotations, "\"type\":\"link\"") && contains(annotations, "https://example.com/"),
          "zebkit_pdf_annotations: the link", annotations);
    zebkit_free(annotations);
    static const struct {
        const char *json;
        const char *what;
    } kinds[] = {
        {"{\"type\": \"highlight\", \"rect\": [18, 115, 160, 145], \"color\": [255, 230, 0],"
         " \"quads\": [[18, 145, 160, 145, 18, 115, 160, 115]], \"contents\": \"a note\", \"author\": \"zebkit\"}",
         "highlight"},
        {"{\"type\": \"ink\", \"rect\": [200, 100, 280, 180], \"color\": [0, 0, 255], \"border\": 3,"
         " \"ink\": [[[210, 110], [270, 170]], [[210, 170], [270, 110]]]}",
         "ink"},
        {"{\"type\": \"line\", \"rect\": [0, 0, 1, 1], \"color\": [255, 0, 0], \"border\": 2,"
         " \"line\": [20, 60, 280, 60]}",
         "line"},
        {"{\"type\": \"polygon\", \"rect\": [0, 0, 1, 1], \"color\": [0, 128, 0], \"interiorColor\": [0, 255, 0],"
         " \"border\": 1, \"vertices\": [[150, 10], [190, 10], [170, 45]]}",
         "polygon"},
        {"{\"type\": \"square\", \"rect\": [230, 10, 290, 50], \"color\": [0, 0, 0], \"border\": 1,"
         " \"opacity\": 0.5}",
         "square"},
        {"{\"type\": \"text\", \"rect\": [5, 170, 25, 190], \"contents\": \"sticky\"}", "note"},
    };
    int last = -1;
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        int index = zebkit_pdf_annotation_add(pdf, 0, kinds[i].json, &error);
        char what[64];
        snprintf(what, sizeof(what), "zebkit_pdf_annotation_add: %s", kinds[i].what);
        check(index > last, what, error);
        zebkit_free(error);
        error = NULL;
        last = index;
    }
    check(zebkit_pdf_annotation_add(pdf, 0, "{\"type\": \"sound\", \"rect\": [0, 0, 1, 1]}", &error) == -1 &&
              contains(error, "sound"),
          "zebkit_pdf_annotation_add refuses a sound", error);
    zebkit_free(error);
    error = NULL;
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check(contains(annotations, "\"type\":\"highlight\"") && contains(annotations, "\"color\":[255,230,0]") &&
              contains(annotations, "\"contents\":\"a note\"") && contains(annotations, "\"author\":\"zebkit\"") &&
              contains(annotations, "\"quads\":[[18.0,145.0,160.0,145.0,18.0,115.0,160.0,115.0]]") &&
              contains(annotations, "\"ink\":[[[210.0,110.0],[270.0,170.0]]") &&
              contains(annotations, "\"line\":[20.0,60.0,280.0,60.0]") &&
              contains(annotations, "\"vertices\":[[150.0,10.0],[190.0,10.0],[170.0,45.0]]") &&
              contains(annotations, "\"opacity\":0.5") && contains(annotations, "\"modified\":\"D:"),
          "zebkit_pdf_annotations: what was made", annotations);
    zebkit_free(annotations);

    /* What each draws: yellow under the text, blue ink, a red line, a
     * green polygon. */
    pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
#define PIXEL(x, y) (pixels + ((size_t)(height - (y)) * width + (x)) * 4)
    int yellow = 0, blue = 0, red = 0, green = 0;
    for (int y = 1; pixels && y < height; y++)
        for (int x = 0; x < width; x++) {
            const uint8_t *p = PIXEL(x, y);
            if (p[0] > 200 && p[1] > 180 && p[2] < 80)
                yellow += y > 115 && y < 145;
            if (p[0] < 60 && p[1] < 60 && p[2] > 200)
                blue += x > 200 && y > 100;
            if (p[0] > 200 && p[1] < 60 && p[2] < 60)
                red += y >= 57 && y <= 63;
            if (p[0] < 60 && p[1] > 200 && p[2] < 60)
                green += y < 50 && x > 140 && x < 200;
        }
    snprintf(detail, sizeof(detail), "yellow %d, blue %d, red %d, green %d", yellow, blue, red, green);
    check(yellow > 1000 && blue > 50 && red > 200 && green > 200, "the annotations are drawn", detail);
    zebkit_free(pixels);

    check(zebkit_pdf_annotation_update(pdf, 0, 1, "{\"color\": [255, 0, 255], \"contents\": null}", &error),
          "zebkit_pdf_annotation_update", error);
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check(contains(annotations, "\"color\":[255,0,255]") && !contains(annotations, "a note"),
          "the change is there", NULL);
    zebkit_free(annotations);
    check(!zebkit_pdf_annotation_update(pdf, 0, 1, "{\"ink\": [[[0, 0], [1, 1]]]}", &error) && error,
          "zebkit_pdf_annotation_update refuses ink on a highlight", error);
    zebkit_free(error);
    error = NULL;
    check(zebkit_pdf_annotation_remove(pdf, 0, last, &error), "zebkit_pdf_annotation_remove", error);
    annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check(!contains(annotations, "sticky"), "the note is gone", NULL);
    zebkit_free(annotations);

    check(zebkit_pdf_attach(pdf, "data.csv", "a,b\n1,2\n", 8, &error), "zebkit_pdf_attach", error);
    char *attachments = zebkit_pdf_attachments(pdf, &error);
    check(contains(attachments, "\"name\":\"data.csv\"") && contains(attachments, "\"size\":8"),
          "zebkit_pdf_attachments", attachments);
    zebkit_free(attachments);
    size_t size = 0;
    uint8_t *file = zebkit_pdf_attachment(pdf, 0, &size, &error);
    check(file && size == 8 && memcmp(file, "a,b\n1,2\n", 8) == 0, "zebkit_pdf_attachment", NULL);
    zebkit_free(file);

    check(zebkit_pdf_set_metadata(pdf, "{\"title\": \"Résumé 履历\", \"author\": \"zebkit\"}", &error),
          "zebkit_pdf_set_metadata", error);
    check(!zebkit_pdf_set_metadata(pdf, "{\"colour\": \"red\"}", &error) && contains(error, "colour"),
          "zebkit_pdf_set_metadata refuses an unknown entry", error);
    zebkit_free(error);
    error = NULL;

    uint8_t *saved = zebkit_pdf_save(pdf, 0, &size, &error);
    check(saved && size > 100, "zebkit_pdf_save", error);
    zebkit_pdf_close(pdf);

    zebkit_pdf *again = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    info = again ? zebkit_pdf_info(again) : NULL;
    check(contains(info, "\"title\":\"Résumé 履历\"") && contains(info, "\"author\":\"zebkit\""),
          "the saved document has its new metadata", info);
    zebkit_free(info);
    attachments = again ? zebkit_pdf_attachments(again, &error) : NULL;
    check(contains(attachments, "data.csv"), "and its attachment", attachments);
    zebkit_free(attachments);
    annotations = again ? zebkit_pdf_annotations(again, 0, &error) : NULL;
    check(contains(annotations, "\"type\":\"polygon\"") && contains(annotations, "\"color\":[255,0,255]"),
          "and its annotations", NULL);
    zebkit_free(annotations);
    check(again && zebkit_pdf_flatten(again, 0, false, &error), "zebkit_pdf_flatten", error);
    annotations = again ? zebkit_pdf_annotations(again, 0, &error) : NULL;
    check(annotations && !contains(annotations, "polygon"), "the flattened page has no polygon annotation",
          annotations);
    zebkit_free(annotations);
    check(again && zebkit_pdf_detach(again, 0, &error), "zebkit_pdf_detach", error);
    zebkit_pdf_close(again);
    zebkit_free(saved);

    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
