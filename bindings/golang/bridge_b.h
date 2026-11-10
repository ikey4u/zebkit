/* The C side of the PDF, canvas, text and services bindings: functions with
 * the engine's exact callback types that pass each call on to the Go
 * function its handle names. */
#ifndef ZEBKIT_GO_BRIDGE_B_H
#define ZEBKIT_GO_BRIDGE_B_H

#include <stdint.h>
#include <stdlib.h>

#include <zebkit/zebkit.h>
#include <zebkit/canvas.h>
#include <zebkit/pdf.h>
#include <zebkit/services.h>
#include <zebkit/text.h>

void zkb_destroy(void *data);
void zkb_fetch(const zebkit_response *response, void *data);
zebkit_pdf_decision zkb_before(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data);
void zkb_after(zebkit_pdf *pdf, const char *operation, void *data);
bool zkb_appearance(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index, const char *annotation,
                    double width, double height, void *data);
char *zkb_check(const char *data_json, void *data);
bool zkb_hit(zebkit_pdf *pdf, int page, int index, const char *annotation, double x, double y, void *data);
void zkb_layer(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data);
char *zkb_ocr(zebkit_pdf *pdf, int page, const uint8_t *rgba, int width, int height, double scale, void *data);
uint8_t *zkb_font(zebkit_pdf *pdf, const char *family, size_t *size, void *data);
zebkit_pdf_trust zkb_trust(zebkit_pdf *pdf, const char *signature, const uint8_t *contents, size_t size, void *data);
bool zkb_action(zebkit_pdf *pdf, const char *action, void *data);
char *zkb_input(zebkit_pdf *pdf, const char *tool, const char *points, void *data);

#endif
