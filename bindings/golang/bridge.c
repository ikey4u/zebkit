#include "bridge.h"
#include "_cgo_export.h"

static uintptr_t handle(void *data) { return (uintptr_t)data; }

void zk_task(void *data) { goZkTask(handle(data)); }
void zk_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data) {
    goZkLoad((int)event, (char *)url, handle(data));
}
void zk_title(zebkit_view *view, const char *title, void *data) { goZkText((char *)title, handle(data)); }
void zk_view(zebkit_view *view, void *data) { goZkTask(handle(data)); }
void zk_result(zebkit_view *view, const char *json, const char *error, void *data) {
    goZkResult((char *)json, (char *)error, handle(data));
}
void zk_capture(zebkit_view *view, const uint8_t *png, size_t size, const char *error, void *data) {
    goZkCapture((void *)png, size, (char *)error, handle(data));
}
void zk_bind(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data) {
    goZkBind((char *)origin, (char *)args, call, handle(data));
}
void zk_scheme(zebkit_request *request, void *data) { goZkScheme(request, handle(data)); }
void zk_asked(zebkit_view *view, void *request, const char *json, void *data) {
    goZkAsked(request, (char *)json, handle(data));
}
void zk_told(zebkit_view *view, const char *json, void *data) { goZkText((char *)json, handle(data)); }
void zk_json(const char *json, void *data) { goZkText((char *)json, handle(data)); }
void zk_menu(const char *id, void *data) { goZkText((char *)id, handle(data)); }
