/* What the PDF tests share: checks, a blank document, pixels of a page. */

#ifndef ZEBKIT_TESTS_PDF_SUPPORT_H_
#define ZEBKIT_TESTS_PDF_SUPPORT_H_

#include "harness.h"

#include <zebkit/pdf.h>
#include <zebkit/services.h>

/* What fails must say why. */
static void refused(int failed, char **error, const char *what) {
    check_text(failed && *error && **error, what, *error);
    zebkit_free(*error);
    *error = NULL;
}

static zebkit_pdf *blank(double width, double height) {
    zebkit_pdf *pdf = zebkit_pdf_new(NULL);
    zebkit_pdf_page_insert(pdf, 0, width, height, NULL);
    return pdf;
}

/* A page's pixels; rows top to bottom. */
typedef struct {
    uint8_t *rgba;
    int width, height;
} shot;

static shot render_at(zebkit_pdf *pdf, int page, float scale) {
    shot s = {0};
    zebkit_pdf_render_options options = {0};
    options.scale = scale;
    options.flags = ZEBKIT_PDF_RENDER_ANNOTATIONS;
    options.background = 0xffffffff;
    s.rgba = zebkit_pdf_render(pdf, page, &options, &s.width, &s.height, NULL);
    return s;
}

static shot render(zebkit_pdf *pdf, int page) {
    return render_at(pdf, page, 1);
}

static void done(shot *s) {
    zebkit_free(s->rgba);
    s->rgba = NULL;
}

/* The pixel at page point (x, y), y up. */
static const uint8_t *at(const shot *s, double x, double y) {
    int px = (int)x, py = s->height - 1 - (int)y;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= s->width) px = s->width - 1;
    if (py >= s->height) py = s->height - 1;
    return s->rgba + (size_t)py * s->width * 4 + (size_t)px * 4;
}

static int near_color(const uint8_t *p, int r, int g, int b) {
    const int t = 40;
    return abs(p[0] - r) <= t && abs(p[1] - g) <= t && abs(p[2] - b) <= t;
}

static int white(const uint8_t *p) {
    return p[0] > 240 && p[1] > 240 && p[2] > 240;
}

/* How many pixels of the box of the page are not white. */
static int inked(const shot *s, double l, double b, double r, double t) {
    int count = 0;
    for (double y = b; y < t; y += 1) {
        for (double x = l; x < r; x += 1) {
            if (!white(at(s, x, y)))
                count++;
        }
    }
    return count;
}

/* Saves and opens again: what a file keeps. */
static zebkit_pdf *reopened(zebkit_pdf *pdf) {
    size_t size = 0;
    uint8_t *file = zebkit_pdf_save(pdf, 0, &size, NULL);
    zebkit_pdf *again = file ? zebkit_pdf_open(file, size, NULL, NULL) : NULL;
    zebkit_free(file);
    return again;
}

static char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc((size_t)length + 1);
    *size = fread(data, 1, (size_t)length, file);
    data[*size] = 0;
    fclose(file);
    return data;
}

static int same_pixels(const shot *a, const shot *b) {
    return a->rgba && b->rgba && a->width == b->width && a->height == b->height &&
           !memcmp(a->rgba, b->rgba, (size_t)a->width * a->height * 4);
}

#endif /* ZEBKIT_TESTS_PDF_SUPPORT_H_ */
