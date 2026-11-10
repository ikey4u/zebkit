#include "bridge_a.h"
#include "_cgo_export.h"

static uintptr_t handle(void *data) { return (uintptr_t)data; }

void zka_wakeup(int64_t delay_ms, void *data) { goZkAWakeup(delay_ms, handle(data)); }
void zka_window_open(zebkit_view *opener, zebkit_view *opened, const char *url, void *data) {
    goZkAWindowOpen(opened, (char *)url, handle(data));
}
void zka_export(void *data, const char *handle_text) { goZkAExport((char *)handle_text, handle(data)); }
void zka_writable(zebkit_network_stream *stream, void *data) { goZkTask(handle(data)); }
void zka_body(zebkit_view *view, const char *request_id, const void *bytes, size_t size, bool end, bool truncated,
              void *data) {
    goZkABody((char *)request_id, (void *)bytes, size, end, truncated, handle(data));
}
void zka_item(zebkit_view *view, const char *item_id, void *data) { goZkAItem((char *)item_id, handle(data)); }
