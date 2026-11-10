#include "bridge_b.h"
#include "_cgo_export.h"

static uintptr_t handle_b(void *data) { return (uintptr_t)data; }

void zkb_destroy(void *data) { goZkBDestroy(handle_b(data)); }

void zkb_fetch(const zebkit_response *r, void *data) {
    goZkBFetch(r->status, (char *)r->url, (char *)r->headers, (void *)r->body, r->body_size, (char *)r->error,
               handle_b(data));
}

zebkit_pdf_decision zkb_before(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    return (zebkit_pdf_decision)goZkBBefore(pdf, (char *)operation, change, reason, handle_b(data));
}

void zkb_after(zebkit_pdf *pdf, const char *operation, void *data) {
    goZkBAfter(pdf, (char *)operation, handle_b(data));
}

bool zkb_appearance(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index, const char *annotation,
                    double width, double height, void *data) {
    return goZkBAppearance(canvas, pdf, page, index, (char *)annotation, width, height, handle_b(data));
}

char *zkb_check(const char *data_json, void *data) { return goZkBCheck((char *)data_json, handle_b(data)); }

bool zkb_hit(zebkit_pdf *pdf, int page, int index, const char *annotation, double x, double y, void *data) {
    return goZkBHit(pdf, page, index, (char *)annotation, x, y, handle_b(data));
}

void zkb_layer(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data) {
    goZkBLayer(canvas, pdf, page, (zebkit_pdf_layer_info *)info, handle_b(data));
}

char *zkb_ocr(zebkit_pdf *pdf, int page, const uint8_t *rgba, int width, int height, double scale, void *data) {
    return goZkBOcr(pdf, page, (void *)rgba, width, height, scale, handle_b(data));
}

uint8_t *zkb_font(zebkit_pdf *pdf, const char *family, size_t *size, void *data) {
    return goZkBFont(pdf, (char *)family, size, handle_b(data));
}

zebkit_pdf_trust zkb_trust(zebkit_pdf *pdf, const char *signature, const uint8_t *contents, size_t size, void *data) {
    return (zebkit_pdf_trust)goZkBTrust(pdf, (char *)signature, (void *)contents, size, handle_b(data));
}

bool zkb_action(zebkit_pdf *pdf, const char *action, void *data) {
    return goZkBAction(pdf, (char *)action, handle_b(data));
}

char *zkb_input(zebkit_pdf *pdf, const char *tool, const char *points, void *data) {
    return goZkBInput(pdf, (char *)tool, (char *)points, handle_b(data));
}
