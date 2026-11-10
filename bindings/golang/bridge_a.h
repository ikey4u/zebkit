/* Bridge functions for the callback shapes of engine_more.go and hooks_more.go. */
#ifndef ZEBKIT_GO_BRIDGE_A_H
#define ZEBKIT_GO_BRIDGE_A_H

#include "bridge.h"

void zka_wakeup(int64_t delay_ms, void *data);
void zka_window_open(zebkit_view *opener, zebkit_view *opened, const char *url, void *data);
void zka_export(void *data, const char *handle);
void zka_writable(zebkit_network_stream *stream, void *data);
void zka_body(zebkit_view *view, const char *request_id, const void *bytes, size_t size, bool end, bool truncated,
              void *data);
void zka_item(zebkit_view *view, const char *item_id, void *data);

#endif
