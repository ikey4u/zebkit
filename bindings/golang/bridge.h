/* The C side of the Go binding: functions with the engine's exact callback
 * types, each passing the call on to the Go function its handle names
 * (exports.go). One function per callback shape; the handle is the data
 * pointer the engine gives back. */
#ifndef ZEBKIT_GO_BRIDGE_H
#define ZEBKIT_GO_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zebkit/zebkit.h>
#include <zebkit/automation.h>
#include <zebkit/canvas.h>
#include <zebkit/hooks.h>
#include <zebkit/media.h>
#include <zebkit/media_tap.h>
#include <zebkit/media_tools.h>
#include <zebkit/offscreen.h>
#include <zebkit/page.h>
#include <zebkit/pdf.h>
#include <zebkit/services.h>
#include <zebkit/text.h>
#include <zebkit/window.h>

/* A handle as the data pointer the engine keeps. */
static inline void *zk_data(uintptr_t handle) { return (void *)handle; }

/* The engine's thread: the one that called zebkit_init() or zebkit_main(). */
void zk_mark_engine_thread(void);
bool zk_on_engine_thread(void);

/* The engine and views. */
void zk_task(void *data);
void zk_text(const char *text, void *data);
void zk_view_text(zebkit_view *view, const char *text, void *data);
void zk_view_closed(zebkit_view *view, void *data);
void zk_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data);
void zk_result(zebkit_view *view, const char *json, const char *error, void *data);
void zk_bytes(zebkit_view *view, const uint8_t *bytes, size_t size, const char *error, void *data);
void zk_bind(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data);
void zk_scheme(zebkit_request *request, void *data);
void zk_asked(zebkit_view *view, void *request, const char *json, void *data);
void zk_wakeup(int64_t delay_ms, void *data);
void zk_window_open(zebkit_view *opener, zebkit_view *opened, const char *url, void *data);
void zk_export(void *data, const char *handle);
void zk_writable(zebkit_network_stream *stream, void *data);
void zk_body(zebkit_view *view, const char *request_id, const void *bytes, size_t size, bool end, bool truncated,
             void *data);

/* Automation, off-screen views, the page's window, media. */
void zk_pixels(zebkit_view *view, const unsigned char *pixels, int width, int height, int stride, const char *error,
               void *data);
void zk_automation_event(zebkit_view *view, const char *event, const char *json, void *data);
void zk_listen(const char *url, const char *error, void *data);
void zk_frame(zebkit_view *view, const zebkit_frame *frame, void *data);
void zk_caret(zebkit_view *view, const zebkit_rect *caret, void *data);
void zk_cursor(zebkit_view *view, const char *cursor, const zebkit_cursor_image *image, void *data);
void zk_popup(zebkit_view *view, bool shown, const zebkit_rect *bounds, void *data);
void zk_dialog(zebkit_view *view, const zebkit_dialog *dialog, void *data);
void zk_dialog_closed(zebkit_view *view, uint64_t id, bool accepted, const char *user_input, void *data);
bool zk_drag(zebkit_view *view, const zebkit_drag *drag, void *data);
void zk_drag_operation(zebkit_view *view, unsigned operation, void *data);
void zk_capture_text(zebkit_media_capture *capture, const char *json, void *data);
void zk_tap(zebkit_media_tap *tap, const char *json, const uint8_t *bytes, size_t size, void *data);
int zk_progress(int64_t packets, int64_t time_us, void *data);

/* Services and PDF. */
void zk_release(void *data);
void zk_fetch(const zebkit_response *response, void *data);
zebkit_pdf_decision zk_before(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data);
void zk_after(zebkit_pdf *pdf, const char *operation, void *data);
bool zk_appearance(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index, const char *annotation, double width,
                   double height, void *data);
char *zk_check(const char *data_json, void *data);
bool zk_hit(zebkit_pdf *pdf, int page, int index, const char *annotation, double x, double y, void *data);
void zk_layer(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data);
char *zk_ocr(zebkit_pdf *pdf, int page, const uint8_t *rgba, int width, int height, double scale, void *data);
uint8_t *zk_font(zebkit_pdf *pdf, const char *family, size_t *size, void *data);
zebkit_pdf_trust zk_trust(zebkit_pdf *pdf, const char *signature, const uint8_t *contents, size_t size, void *data);
bool zk_action(zebkit_pdf *pdf, const char *action, void *data);
char *zk_input(zebkit_pdf *pdf, const char *tool, const char *points, void *data);

#endif
