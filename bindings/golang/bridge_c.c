#include "bridge_c.h"
#include "_cgo_export.h"

static uintptr_t h(void *data) { return (uintptr_t)data; }

void zkc_pixels(zebkit_view *view, const unsigned char *pixels, int width, int height, int stride,
                const char *error, void *data) {
    goZkCPixels((void *)pixels, width, height, stride, (char *)error, h(data));
}
void zkc_automation_event(zebkit_view *view, const char *event, const char *json, void *data) {
    goZkCEvent((char *)event, (char *)json, h(data));
}
void zkc_listen(const char *url, const char *error, void *data) {
    goZkCListen((char *)url, (char *)error, h(data));
}
void zkc_frame(zebkit_view *view, const zebkit_frame *frame, void *data) {
    goZkCFrame((zebkit_frame *)frame, h(data));
}
void zkc_caret(zebkit_view *view, const zebkit_rect *caret, void *data) {
    goZkCRect(caret != NULL, (zebkit_rect *)caret, h(data));
}
void zkc_cursor(zebkit_view *view, const char *cursor, const zebkit_cursor_image *image, void *data) {
    goZkCCursor((char *)cursor, (zebkit_cursor_image *)image, h(data));
}
void zkc_popup(zebkit_view *view, bool shown, const zebkit_rect *bounds, void *data) {
    goZkCRect(shown && bounds != NULL, (zebkit_rect *)bounds, h(data));
}
void zkc_dialog(zebkit_view *view, const zebkit_dialog *dialog, void *data) {
    goZkCDialog((zebkit_dialog *)dialog, h(data));
}
void zkc_dialog_closed(zebkit_view *view, uint64_t id, bool accepted, const char *user_input, void *data) {
    goZkCDialogClosed(id, accepted, (char *)user_input, h(data));
}
bool zkc_drag(zebkit_view *view, const zebkit_drag *drag, void *data) {
    return goZkCDrag((zebkit_drag *)drag, h(data));
}
void zkc_drag_operation(zebkit_view *view, unsigned operation, void *data) {
    goZkCOperation(operation, h(data));
}
void zkc_capture_json(zebkit_media_capture *capture, const char *json, void *data) {
    goZkCJSON((char *)json, h(data));
}
void zkc_tap(zebkit_media_tap *tap, const char *json, const uint8_t *bytes, size_t size, void *data) {
    goZkCTap((char *)json, (void *)bytes, size, h(data));
}
int zkc_progress(int64_t packets, int64_t time_us, void *data) {
    return goZkCProgress(packets, time_us, h(data));
}
