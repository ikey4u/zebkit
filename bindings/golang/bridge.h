/* The C side of the Go binding: functions with the engine's exact callback
 * types that pass each call on to the Go function its handle names. */
#ifndef ZEBKIT_GO_BRIDGE_H
#define ZEBKIT_GO_BRIDGE_H

#include <stdint.h>
#include <stdlib.h>

#include <zebkit/zebkit.h>
#include <zebkit/hooks.h>
#include <zebkit/window.h>

void zk_task(void *data);
void zk_load(zebkit_view *view, zebkit_load_event event, const char *url, void *data);
void zk_title(zebkit_view *view, const char *title, void *data);
void zk_view(zebkit_view *view, void *data);
void zk_result(zebkit_view *view, const char *json, const char *error, void *data);
void zk_capture(zebkit_view *view, const uint8_t *png, size_t size, const char *error, void *data);
void zk_bind(zebkit_view *view, const char *origin, const char *args, zebkit_call *call, void *data);
void zk_scheme(zebkit_request *request, void *data);
void zk_asked(zebkit_view *view, void *request, const char *json, void *data);
void zk_told(zebkit_view *view, const char *json, void *data);
void zk_json(const char *json, void *data);
void zk_menu(const char *id, void *data);

#endif
