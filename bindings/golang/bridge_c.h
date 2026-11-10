/* C trampolines of automation.go, offscreen.go, page.go and media.go: the
 * engine's exact callback types, passing each call on to the Go function
 * its handle names. */
#ifndef ZEBKIT_GO_BRIDGE_C_H
#define ZEBKIT_GO_BRIDGE_C_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include <zebkit/zebkit.h>
#include <zebkit/automation.h>
#include <zebkit/media.h>
#include <zebkit/media_tap.h>
#include <zebkit/media_tools.h>
#include <zebkit/offscreen.h>
#include <zebkit/page.h>

void zkc_pixels(zebkit_view *view, const unsigned char *pixels, int width, int height, int stride,
                const char *error, void *data);
void zkc_automation_event(zebkit_view *view, const char *event, const char *json, void *data);
void zkc_listen(const char *url, const char *error, void *data);
void zkc_frame(zebkit_view *view, const zebkit_frame *frame, void *data);
void zkc_caret(zebkit_view *view, const zebkit_rect *caret, void *data);
void zkc_cursor(zebkit_view *view, const char *cursor, const zebkit_cursor_image *image, void *data);
void zkc_popup(zebkit_view *view, bool shown, const zebkit_rect *bounds, void *data);
void zkc_dialog(zebkit_view *view, const zebkit_dialog *dialog, void *data);
void zkc_dialog_closed(zebkit_view *view, uint64_t id, bool accepted, const char *user_input, void *data);
bool zkc_drag(zebkit_view *view, const zebkit_drag *drag, void *data);
void zkc_drag_operation(zebkit_view *view, unsigned operation, void *data);
void zkc_capture_json(zebkit_media_capture *capture, const char *json, void *data);
void zkc_tap(zebkit_media_tap *tap, const char *json, const uint8_t *bytes, size_t size, void *data);
int zkc_progress(int64_t packets, int64_t time_us, void *data);

#endif
