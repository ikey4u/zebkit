/* See plugin.h. A plain C file over the public API. */

#include "plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/services.h>

enum { kMaxSteps = 256, kMaxHooks = 8 };

struct plugin {
    int hooks[kMaxHooks];
    int hook_count;
    /* The inverses of what has happened, and of what has been undone. */
    char *undo[kMaxSteps];
    int undo_count;
    char *redo[kMaxSteps];
    int redo_count;
    int applying; /* the operation being told is the plugin's own undo or redo */
    int redoing;
};

/* "inverse":{...} of an operation, as text: the object's braces matched. */
static char *inverse_of(const char *operation) {
    const char *at = strstr(operation, "\"inverse\":{");
    if (!at)
        return NULL;
    at += strlen("\"inverse\":");
    int depth = 0, in_string = 0;
    for (const char *c = at; *c; ++c) {
        if (in_string) {
            if (*c == '\\')
                ++c;
            else if (*c == '"')
                in_string = 0;
        } else if (*c == '"') {
            in_string = 1;
        } else if (*c == '{') {
            ++depth;
        } else if (*c == '}' && --depth == 0) {
            const size_t size = (size_t)(c - at) + 1;
            char *out = malloc(size + 1);
            memcpy(out, at, size);
            out[size] = 0;
            return out;
        }
    }
    return NULL;
}

static void push(char **list, int *count, char *text) {
    if (*count == kMaxSteps) {
        free(list[0]);
        memmove(list, list + 1, sizeof(*list) * (kMaxSteps - 1));
        --*count;
    }
    list[(*count)++] = text;
}

static void clear(char **list, int *count) {
    while (*count > 0)
        free(list[--*count]);
}

/* Told of every change. Reversible ones go in the history. */
static void on_change(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf;
    plugin *self = data;
    char *inverse = inverse_of(operation);
    if (!inverse)
        return;
    if (self->applying && !self->redoing) {
        push(self->redo, &self->redo_count, inverse); /* what the undo just did, reversed */
    } else {
        push(self->undo, &self->undo_count, inverse);
        if (!self->applying)
            clear(self->redo, &self->redo_count);
    }
}

/* A sticker's appearance: a yellow disc, a frown or a smile by its mood. */
static bool draw_sticker(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index, const char *annotation,
                         double width, double height, void *data) {
    (void)pdf;
    (void)page;
    (void)index;
    (void)data;
    const double radius = (width < height ? width : height) / 2;
    zebkit_canvas_set_fill_color(canvas, 0xffdd00ff);
    zebkit_canvas_begin_path(canvas);
    zebkit_canvas_arc(canvas, width / 2, height / 2, radius, 0, 360, false);
    zebkit_canvas_fill(canvas, false);
    zebkit_canvas_set_stroke_color(canvas, strstr(annotation, "\"mood\":\"sad\"") ? 0x0000ffff : 0xff0000ff);
    zebkit_canvas_set_line_width(canvas, radius / 5);
    zebkit_canvas_begin_path(canvas);
    if (strstr(annotation, "\"mood\":\"sad\""))
        zebkit_canvas_arc(canvas, width / 2, height / 2 + radius * 0.8, radius * 0.5, 200, 340, false);
    else
        zebkit_canvas_arc(canvas, width / 2, height / 2, radius * 0.5, 20, 160, false);
    zebkit_canvas_stroke(canvas);
    return true;
}

/* It is round: its corners are not on it. */
static bool sticker_hit(zebkit_pdf *pdf, int page, int index, const char *annotation, double x, double y, void *data) {
    (void)pdf;
    (void)page;
    (void)index;
    (void)data;
    double l = 0, b = 0, r = 0, t = 0;
    const char *rect = strstr(annotation, "\"rect\":[");
    if (!rect || sscanf(rect, "\"rect\":[%lf,%lf,%lf,%lf", &l, &b, &r, &t) != 4)
        return false;
    const double dx = x - (l + r) / 2, dy = y - (b + t) / 2, radius = (r - l) / 2;
    return dx * dx + dy * dy <= radius * radius;
}

/* A sticker of no mood would say nothing. */
static char *check_sticker(const char *data_json, void *data) {
    (void)data;
    return strstr(data_json, "\"mood\":\"\"") ? zebkit_pdf_string("a sticker's mood is not empty") : NULL;
}

/* Text for a page without any: this example's OCR reads one line. */
static char *read_page(zebkit_pdf *pdf, int page, const uint8_t *rgba, int width, int height, double scale,
                       void *data) {
    (void)pdf;
    (void)data;
    /* That it was given the page: its size, and pixels. */
    if (!rgba || width <= 0 || height <= 0 || scale <= 0)
        return NULL;
    char json[128];
    snprintf(json, sizeof(json), "[{\"text\":\"read from page %d\",\"box\":[10,10,%d,30]}]", page, (int)(width / scale));
    return zebkit_pdf_string(json);
}

plugin *plugin_install(zebkit_pdf *pdf) {
    plugin *self = calloc(1, sizeof(plugin));
    zebkit_pdf_kind kind = {0};
    kind.name = PLUGIN_STICKER;
    kind.prefix = "Sample_";
    kind.schema = "{\"mood\":{\"type\":\"string\",\"enum\":[\"happy\",\"sad\",\"\"],\"required\":true},"
                  "\"size\":{\"type\":\"number\",\"min\":1,\"max\":9}}";
    kind.check = check_sticker;
    kind.draw = draw_sticker;
    kind.hit_test = sticker_hit;
    self->hooks[self->hook_count++] = zebkit_pdf_hook_kind(pdf, &kind, NULL);
    self->hooks[self->hook_count++] = zebkit_pdf_hook_after(pdf, "*", on_change, self, NULL);
    self->hooks[self->hook_count++] = zebkit_pdf_hook_ocr(pdf, read_page, self, NULL);
    return self;
}

void plugin_remove(plugin *self) {
    for (int i = 0; i < self->hook_count; ++i)
        zebkit_pdf_hook_remove(self->hooks[i]);
    clear(self->undo, &self->undo_count);
    clear(self->redo, &self->redo_count);
    free(self);
}

int plugin_history(const plugin *self) {
    return self->undo_count;
}

bool plugin_undo(plugin *self, zebkit_pdf *pdf) {
    if (!self->undo_count)
        return false;
    char *step = self->undo[--self->undo_count];
    self->applying = 1;
    const bool ok = zebkit_pdf_apply(pdf, step, NULL);
    self->applying = 0;
    free(step);
    return ok;
}

bool plugin_redo(plugin *self, zebkit_pdf *pdf) {
    if (!self->redo_count)
        return false;
    char *step = self->redo[--self->redo_count];
    self->applying = 1;
    self->redoing = 1;
    const bool ok = zebkit_pdf_apply(pdf, step, NULL);
    self->applying = 0;
    self->redoing = 0;
    free(step);
    return ok;
}
