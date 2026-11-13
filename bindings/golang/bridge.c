#include "bridge.h"
#include "_cgo_export.h"

#ifdef _WIN32
#include <windows.h>
static DWORD engine_thread;
static bool engine_thread_set;
void zk_mark_engine_thread(void) {
    engine_thread = GetCurrentThreadId();
    engine_thread_set = true;
}
bool zk_on_engine_thread(void) { return !engine_thread_set || GetCurrentThreadId() == engine_thread; }
#else
#include <pthread.h>
static pthread_t engine_thread;
static bool engine_thread_set;
void zk_mark_engine_thread(void) {
    engine_thread = pthread_self();
    engine_thread_set = true;
}
bool zk_on_engine_thread(void) { return !engine_thread_set || pthread_equal(pthread_self(), engine_thread); }
#endif

static uintptr_t handle(void *data) { return (uintptr_t)data; }

void zk_task(void *data) { goZkTask(handle(data)); }
void zk_text(const char *text, void *data) { goZkText((char *)text, handle(data)); }
void zk_view_text(zebkit_view *view, const char *text, void *data) { goZkText((char *)text, handle(data)); }
void zk_view_closed(zebkit_view *view, void *data) { goZkViewClosed(view); }
void zk_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    goZkLoad((int)event, (char *)url, handle(data));
}
void zk_result(zebkit_view *view, const char *json, const char *error, void *data) {
    goZkResult((char *)json, (char *)error, handle(data));
}
void zk_bytes(zebkit_view *view, const uint8_t *bytes, size_t size, const char *error, void *data) {
    goZkBytes((void *)bytes, size, (char *)error, handle(data));
}
void zk_bind(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    goZkBind((char *)origin, (char *)args, call, handle(data));
}
void zk_scheme(zebkit_request *request, void *data) { goZkScheme(request, handle(data)); }
void zk_asked(zebkit_view *view, void *request, const char *json, void *data) {
    goZkAsked(request, (char *)json, handle(data));
}
void zk_wakeup(int64_t delay_ms, void *data) { goZkWakeup(delay_ms, handle(data)); }
void zk_window_open(zebkit_view *opener, zebkit_view *opened, const char *url, void *data) {
    goZkWindowOpen(opened, (char *)url, handle(data));
}
void zk_export(zebkit_view *view, const char *text, void *data) { goZkExport((char *)text, handle(data)); }
void zk_writable(zebkit_network_stream *stream, void *data) { goZkTask(handle(data)); }
void zk_body(zebkit_view *view, const char *request_id, const void *bytes, size_t size, bool end, bool truncated,
             void *data) {
    goZkBody((char *)request_id, (void *)bytes, size, end, truncated, handle(data));
}

void zk_pixels(zebkit_view *view, const unsigned char *pixels, int width, int height, int stride, const char *error,
               void *data) {
    goZkPixels((void *)pixels, width, height, stride, (char *)error, handle(data));
}
void zk_automation_event(zebkit_view *view, const char *event, const char *json, void *data) {
    goZkEvent((char *)event, (char *)json, handle(data));
}
void zk_listen(const char *url, const char *error, void *data) {
    goZkResult((char *)url, (char *)error, handle(data));
}
void zk_cdp_listen(unsigned port, const char *url, const char *error, void *data) {
    goZkListen(port, (char *)url, (char *)error, handle(data));
}
void zk_desktop_done(bool done, const char *error, void *data) {
    goZkResult(NULL, done ? NULL : (char *)(error ? error : "the system did not do it"), handle(data));
}
void zk_frame(zebkit_view *view, const zebkit_frame *frame, void *data) {
    goZkFrame((zebkit_frame *)frame, handle(data));
}
void zk_caret(zebkit_view *view, const zebkit_rect *caret, void *data) {
    goZkRect(caret != NULL, (zebkit_rect *)caret, handle(data));
}
void zk_cursor(zebkit_view *view, const char *cursor, const zebkit_cursor_image *image, void *data) {
    goZkCursor((char *)cursor, (zebkit_cursor_image *)image, handle(data));
}
void zk_popup(zebkit_view *view, bool shown, const zebkit_rect *bounds, void *data) {
    goZkRect(shown && bounds != NULL, (zebkit_rect *)bounds, handle(data));
}
void zk_dialog(zebkit_view *view, const zebkit_dialog *dialog, void *data) {
    goZkDialog((zebkit_dialog *)dialog, handle(data));
}
void zk_dialog_closed(zebkit_view *view, uint64_t id, bool accepted, const char *user_input, void *data) {
    goZkDialogClosed(id, accepted, (char *)user_input, handle(data));
}
bool zk_drag(zebkit_view *view, const zebkit_drag *drag, void *data) {
    return goZkDrag((zebkit_drag *)drag, handle(data));
}
void zk_drag_operation(zebkit_view *view, unsigned operation, void *data) {
    goZkOperation(operation, handle(data));
}
void zk_capture_text(zebkit_media_capture *capture, const char *json, void *data) {
    goZkText((char *)json, handle(data));
}
void zk_tap(zebkit_media_tap *tap, const char *json, const uint8_t *bytes, size_t size, void *data) {
    goZkTap((char *)json, (void *)bytes, size, handle(data));
}
int zk_progress(int64_t packets, int64_t time_us, void *data) { return goZkProgress(packets, time_us, handle(data)); }

void zk_release(void *data) { goZkRelease(handle(data)); }
void zk_fetch(const zebkit_response *r, void *data) {
    goZkFetch(r->status, (char *)r->url, (char *)r->headers, (void *)r->body, r->body_size, (char *)r->error,
              handle(data));
}
zebkit_pdf_decision zk_before(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    return (zebkit_pdf_decision)goZkBefore(pdf, (char *)operation, change, reason, handle(data));
}
void zk_after(zebkit_pdf *pdf, const char *operation, void *data) { goZkAfter(pdf, (char *)operation, handle(data)); }
bool zk_appearance(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index, const char *annotation, double width,
                   double height, void *data) {
    return goZkAppearance(canvas, pdf, page, index, (char *)annotation, width, height, handle(data));
}
char *zk_check(const char *data_json, void *data) { return goZkCheck((char *)data_json, handle(data)); }
bool zk_hit(zebkit_pdf *pdf, int page, int index, const char *annotation, double x, double y, void *data) {
    return goZkHit(pdf, page, index, (char *)annotation, x, y, handle(data));
}
void zk_layer(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data) {
    goZkLayer(canvas, pdf, page, (zebkit_pdf_layer_info *)info, handle(data));
}
char *zk_ocr(zebkit_pdf *pdf, int page, const uint8_t *rgba, int width, int height, double scale, void *data) {
    return goZkOcr(pdf, page, (void *)rgba, width, height, scale, handle(data));
}
uint8_t *zk_font(zebkit_pdf *pdf, const char *family, size_t *size, void *data) {
    return goZkFont(pdf, (char *)family, size, handle(data));
}
zebkit_pdf_trust zk_trust(zebkit_pdf *pdf, const char *signature, const uint8_t *contents, size_t size, void *data) {
    return (zebkit_pdf_trust)goZkTrust(pdf, (char *)signature, (void *)contents, size, handle(data));
}
bool zk_action(zebkit_pdf *pdf, const char *action, void *data) { return goZkAction(pdf, (char *)action, handle(data)); }
char *zk_input(zebkit_pdf *pdf, const char *tool, const char *points, void *data) {
    return goZkInput(pdf, (char *)tool, (char *)points, handle(data));
}
