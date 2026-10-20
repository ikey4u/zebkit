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

/* A form: a text field with a value, and a check box with Yes and Off. */
static const char form[] =
    "%PDF-1.7\n"
    "1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R] /DA (/Helv 0 Tf 0 g)"
    " /DR << /Font << /Helv 8 0 R >> >> >> >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Annots [6 0 R 7 0 R] >> endobj\n"
    "6 0 obj << /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Ada) /Rect [20 150 200 170] /P 3 0 R"
    " /DA (/Helv 12 Tf 0 g) /F 4 >> endobj\n"
    "7 0 obj << /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off /Rect [20 120 35 135] /P 3 0 R"
    " /F 4 /AP << /N << /Yes 9 0 R /Off 10 0 R >> >> >> endobj\n"
    "8 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\n"
    "9 0 obj << /Type /XObject /Subtype /Form /BBox [0 0 15 15] >> stream\n0 0 0 rg 2 2 11 11 re f\nendstream endobj\n"
    "10 0 obj << /Type /XObject /Subtype /Form /BBox [0 0 15 15] >> stream\n\nendstream endobj\n"
    "trailer << /Root 1 0 R >>\n"
    "%%EOF\n";

static void test_redaction(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open(document, sizeof(document) - 1, NULL, &error);
    if (!pdf)
        return;
    /* A mark over "Hello", the first word of "Hello zebkit" (24 points, from x 20). */
    int mark = zebkit_pdf_annotation_add(
        pdf, 0, "{\"type\": \"redact\", \"rect\": [18, 112, 80, 148], \"quads\": [[18, 148, 80, 148, 18, 112, 80, 112]]}",
        &error);
    check(mark >= 0, "a redaction mark", error);
    int areas = zebkit_pdf_redact(pdf, 0, NULL, 0x000000ff, &error);
    char detail[64];
    snprintf(detail, sizeof(detail), "%d", areas);
    check(areas == 1, "zebkit_pdf_redact: the page's mark", error ? error : detail);
    char *text = zebkit_pdf_text(pdf, 0, &error);
    check(text && !strstr(text, "Hello") && strstr(text, "zebkit"), "the word under it is gone, the rest is there",
          text);
    zebkit_free(text);
    char *annotations = zebkit_pdf_annotations(pdf, 0, &error);
    check(annotations && !strstr(annotations, "redact"), "the mark is gone", annotations);
    zebkit_free(annotations);
    int width = 0, height = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, NULL, &width, &height, &error);
    /* The middle of the area, in pixels from the top: x 50, y 200 - 130. */
    const uint8_t *middle = pixels ? pixels + ((size_t)(200 - 130) * width + 50) * 4 : NULL;
    check(middle && middle[0] < 20 && middle[1] < 20 && middle[2] < 20, "a black box over the area", NULL);
    zebkit_free(pixels);
    /* Saved and opened again, the word is not in the file's text. */
    size_t size = 0;
    uint8_t *saved = zebkit_pdf_save(pdf, 0, &size, &error);
    zebkit_pdf *again = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    text = again ? zebkit_pdf_text(again, 0, &error) : NULL;
    check(text && !strstr(text, "Hello") && strstr(text, "zebkit"), "and not in the saved document", text);
    zebkit_free(text);
    /* An area given, on the second page: "Second" goes, with no box. */
    areas = again ? zebkit_pdf_redact(again, 1, "[[15, 110, 80, 145]]", 0, &error) : -1;
    text = again ? zebkit_pdf_text(again, 1, &error) : NULL;
    check(areas == 1 && text && !strstr(text, "Second") && strstr(text, "page zebkit"), "zebkit_pdf_redact: an area",
          text);
    zebkit_free(text);
    zebkit_pdf_close(again);
    zebkit_free(saved);
    zebkit_pdf_close(pdf);
}

static void test_forms_and_pages(void) {
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open(form, sizeof(form) - 1, NULL, &error);
    check(pdf != NULL, "a document with a form", error);
    if (!pdf)
        return;
    char *fields = zebkit_pdf_form_fields(pdf, &error);
    check(contains(fields, "\"name\":\"name\"") && contains(fields, "\"type\":\"text\"") &&
              contains(fields, "\"value\":\"Ada\"") && contains(fields, "\"type\":\"checkbox\"") &&
              contains(fields, "\"exportValue\":\"Yes\"") && contains(fields, "\"checked\":false"),
          "zebkit_pdf_form_fields", fields);
    zebkit_free(fields);
    check(zebkit_pdf_form_set(pdf, "name", "Grace Hopper", &error), "zebkit_pdf_form_set: a text field", error);
    check(zebkit_pdf_form_set(pdf, "agree", "Yes", &error), "zebkit_pdf_form_set: a check box", error);
    check(!zebkit_pdf_form_set(pdf, "nobody", "x", &error) && contains(error, "no field"),
          "zebkit_pdf_form_set refuses a field that is not there", error);
    zebkit_free(error);
    error = NULL;
    fields = zebkit_pdf_form_fields(pdf, &error);
    check(contains(fields, "\"value\":\"Grace Hopper\"") && contains(fields, "\"checked\":true"),
          "the values are set", fields);
    zebkit_free(fields);
    /* The text drawn into the field's new appearance, with forms on. */
    zebkit_pdf_render_options options = {.scale = 1, .background = 0xffffffff, .flags = ZEBKIT_PDF_RENDER_FORMS};
    int width = 0, height = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, &options, &width, &height, &error);
    int dark = 0;
    for (int y = 200 - 170; pixels && y < 200 - 150; y++)
        for (int x = 20; x < 200; x++)
            dark += pixels[(y * width + x) * 4] < 128;
    char detail[64];
    snprintf(detail, sizeof(detail), "%d dark pixels in the field", dark);
    check(dark > 30, "the field draws its new value", detail);
    zebkit_free(pixels);
    size_t size = 0;
    uint8_t *saved = zebkit_pdf_save(pdf, 0, &size, &error);
    check(zebkit_pdf_form_reset(pdf, &error), "zebkit_pdf_form_reset", error);
    fields = zebkit_pdf_form_fields(pdf, &error);
    check(!contains(fields, "Grace") && contains(fields, "\"checked\":false"), "the values are reset", fields);
    zebkit_free(fields);
    zebkit_pdf_close(pdf);
    zebkit_pdf *again = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    fields = again ? zebkit_pdf_form_fields(again, &error) : NULL;
    check(contains(fields, "Grace Hopper") && contains(fields, "\"checked\":true"), "the saved form keeps its values",
          fields);
    zebkit_free(fields);
    zebkit_free(saved);

    /* Pages: one blank page inserted, turned, moved, others copied in. */
    check(zebkit_pdf_page_insert(again, 1, 100, 50, &error), "zebkit_pdf_page_insert", error);
    check(zebkit_pdf_page_rotate(again, 1, 90, &error), "zebkit_pdf_page_rotate", error);
    char *page = zebkit_pdf_page(again, 1, &error);
    check(contains(page, "\"rotation\":90") && contains(page, "\"width\":50.0"), "the new page, turned", page);
    zebkit_free(page);
    int first[] = {1};
    check(zebkit_pdf_pages_move(again, first, 1, 0, &error), "zebkit_pdf_pages_move", error);
    page = zebkit_pdf_page(again, 0, &error);
    check(contains(page, "\"rotation\":90"), "the moved page is first", page);
    zebkit_free(page);
    zebkit_pdf *merged = zebkit_pdf_new(&error);
    check(merged && zebkit_pdf_pages_import(merged, again, NULL, 0, 0, &error) &&
              zebkit_pdf_page_count(merged) == 2,
          "zebkit_pdf_new and zebkit_pdf_pages_import, all pages", error);
    int second[] = {1};
    check(merged && zebkit_pdf_pages_import(merged, again, second, 1, 2, &error) &&
              zebkit_pdf_page_count(merged) == 3,
          "zebkit_pdf_pages_import, one page", error);
    int both[] = {0, 2};
    check(merged && zebkit_pdf_pages_delete(merged, both, 2, &error) && zebkit_pdf_page_count(merged) == 1,
          "zebkit_pdf_pages_delete", error);
    int bad[] = {5};
    check(!zebkit_pdf_pages_delete(merged, bad, 1, &error), "zebkit_pdf_pages_delete refuses a page not there",
          error);
    zebkit_free(error);
    error = NULL;
    saved = merged ? zebkit_pdf_save(merged, 0, &size, &error) : NULL;
    zebkit_pdf *built = saved ? zebkit_pdf_open(saved, size, NULL, &error) : NULL;
    check(built && zebkit_pdf_page_count(built) == 1, "the built document saved and opened", error);
    zebkit_pdf_close(built);
    zebkit_free(saved);
    zebkit_pdf_close(merged);
    zebkit_pdf_close(again);
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
        {"{\"type\": \"circle\", \"rect\": [100, 150, 130, 180], \"color\": [0, 0, 0], \"border\": 1}", "circle"},
        {"{\"type\": \"freeText\", \"rect\": [150, 150, 290, 190], \"contents\": \"free text\"}", "free text"},
        {"{\"type\": \"redact\", \"rect\": [20, 115, 80, 145],"
         " \"quads\": [[20, 145, 80, 145, 20, 115, 80, 115]]}",
         "redaction mark"},
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
              contains(annotations, "\"opacity\":0.5") && contains(annotations, "\"modified\":\"D:") &&
              contains(annotations, "\"type\":\"circle\"") && contains(annotations, "\"contents\":\"free text\"") &&
              contains(annotations, "\"type\":\"redact\""),
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

    test_forms_and_pages();
    test_redaction();
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
