/*
 * The renderer: draws one page off-screen with zebkit and gives its frames
 * to the host that started it, over the protocol of protocol.h. The host
 * (show.c) starts it as:
 *
 *   render --headless --offscreen-socket=<fd> [--offscreen-mach=<name>]
 *          --offscreen-url=<url> --offscreen-size=<width>x<height>@<scale>
 *
 * with Chromium's switches besides (--no-sandbox where there is no
 * sandbox). It ends when the host says so or goes away.
 *
 * Messages from the host are read on a thread of the renderer's own, which
 * hands each to the engine's thread with zebkit_post_task(); everything
 * else happens on the engine's thread.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <zebkit/offscreen.h>
#include <zebkit/services.h>

#include "protocol.h"

#ifdef __APPLE__
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#endif

static int sock = -1;
static zebkit_view *view;
static int measure_jpeg;

#ifdef __APPLE__
static mach_port_t host_port = MACH_PORT_NULL;
#endif

/* Buffers the host has been given, by the engine's buffer id. */
#define KNOWN 64
static uint64_t known[KNOWN];
static int known_next;

static int is_known(uint64_t id) {
    for (int i = 0; i < KNOWN; ++i)
        if (known[i] == id)
            return 1;
    return 0;
}

/* The frames the host holds, by layer and sequence. */
#define HELD 16
static const zebkit_frame *held[2][HELD];

/* The engine's clock: CLOCK_MONOTONIC on Linux, mach_absolute_time() on macOS. */
static int64_t now_us(void) {
#ifdef __APPLE__
    return (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

static void send_message(const void *message, size_t size, const int *fds, int fd_count) {
    (void)size;
    zko_send(sock, message, fds, fd_count);
}

static void send_text(uint32_t type, uint32_t id, const char *text) {
    zko_text message = {{type, sizeof(zko_text)}, id, {0}};
    snprintf(message.text, sizeof(message.text), "%s", text ? text : "");
    send_message(&message, sizeof(message), NULL, 0);
}

/* A frame as a JPEG, as CDP's screencast would send it: its size and the
 * time it takes, to compare with passing the buffer. */
static void measure(const zebkit_frame *frame) {
    const uint8_t *base = frame->pixels;
    size_t stride = frame->stride;
#ifdef __APPLE__
    IOSurfaceRef surface = frame->kind == ZEBKIT_FRAME_IOSURFACE ? (IOSurfaceRef)frame->iosurface : NULL;
    if (surface) {
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        base = IOSurfaceGetBaseAddress(surface);
        stride = IOSurfaceGetBytesPerRow(surface);
    }
#endif
    if (!base)
        return;
    int64_t start = now_us();
    size_t pixels = (size_t)frame->width * frame->height;
    uint8_t *rgba = malloc(pixels * 4);
    for (int y = 0; y < frame->height; ++y) {
        const uint8_t *row = base + y * stride;
        for (int x = 0; x < frame->width; ++x) {
            uint8_t *out = rgba + ((size_t)y * frame->width + x) * 4;
            out[0] = row[x * 4 + 2], out[1] = row[x * 4 + 1], out[2] = row[x * 4], out[3] = row[x * 4 + 3];
        }
    }
    size_t size = 0;
    uint8_t *jpeg = zebkit_image_encode(ZEBKIT_IMAGE_JPEG, rgba, frame->width, frame->height, 80, &size, NULL);
    int64_t took = now_us() - start;
    zko_jpeg message = {{ZKO_JPEG, sizeof(zko_jpeg)}, frame->width, frame->height, size, (size + 2) / 3 * 4, took};
    send_message(&message, sizeof(message), NULL, 0);
    zebkit_free(jpeg);
    free(rgba);
#ifdef __APPLE__
    if (surface)
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
#endif
}

static void on_frame(zebkit_view *v, const zebkit_frame *frame, void *data) {
    (void)v, (void)data;
    if (measure_jpeg && frame->layer == ZEBKIT_LAYER_VIEW) {
        measure_jpeg = 0;
        measure(frame);
    }
    zko_frame message;
    memset(&message, 0, sizeof(message));
    message.header = (zko_header){ZKO_FRAME, sizeof(zko_frame)};
    message.layer = frame->layer;
    message.sequence = frame->sequence;
    message.bounds[0] = frame->bounds.x, message.bounds[1] = frame->bounds.y;
    message.bounds[2] = frame->bounds.width, message.bounds[3] = frame->bounds.height;
    message.width = frame->width, message.height = frame->height;
    message.scale = frame->scale;
    message.format = frame->format;
    message.buffer_id = frame->buffer_id;
    message.damage_count = frame->damage_count < 4 ? (uint32_t)frame->damage_count : 4;
    for (uint32_t i = 0; i < message.damage_count; ++i) {
        message.damage[i][0] = frame->damage[i].x, message.damage[i][1] = frame->damage[i].y;
        message.damage[i][2] = frame->damage[i].width, message.damage[i][3] = frame->damage[i].height;
    }
    message.drawn_us = frame->drawn_us;
    message.ready_us = frame->ready_us;

    int fds[4], fd_count = 0;
    const int fresh = !is_known(frame->buffer_id);
    switch (frame->kind) {
    case ZEBKIT_FRAME_SHARED_MEMORY:
        message.kind = ZKO_SHARED_MEMORY;
        message.stride = frame->stride, message.offset = frame->offset, message.size = frame->size;
        if (fresh)
            fds[fd_count++] = frame->fd;
        break;
    case ZEBKIT_FRAME_DMABUF:
        message.kind = ZKO_DMABUF;
        message.plane_count = (uint32_t)frame->plane_count;
        for (int i = 0; i < frame->plane_count; ++i) {
            message.strides[i] = frame->strides[i], message.offsets[i] = frame->offsets[i];
            if (fresh)
                fds[fd_count++] = frame->fds[i];
        }
        message.modifier = frame->modifier, message.fourcc = frame->fourcc;
        break;
    case ZEBKIT_FRAME_IOSURFACE:
        message.kind = ZKO_IOSURFACE;
#ifdef __APPLE__
        if (fresh && host_port != MACH_PORT_NULL) {
            zko_surface_message surface;
            memset(&surface, 0, sizeof(surface));
            surface.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
            surface.header.msgh_size = sizeof(surface);
            surface.header.msgh_remote_port = host_port;
            surface.body.msgh_descriptor_count = 1;
            surface.surface.name = zebkit_frame_iosurface_mach_port(frame);
            surface.surface.disposition = MACH_MSG_TYPE_MOVE_SEND;
            surface.surface.type = MACH_MSG_PORT_DESCRIPTOR;
            surface.buffer_id = frame->buffer_id;
            if (mach_msg(&surface.header, MACH_SEND_MSG, sizeof(surface), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                         MACH_PORT_NULL) != KERN_SUCCESS)
                mach_port_deallocate(mach_task_self(), surface.surface.name);
        }
#endif
        break;
    }
    if (fresh) {
        known[known_next] = frame->buffer_id;
        known_next = (known_next + 1) % KNOWN;
    }
    message.fd_count = (uint32_t)fd_count;
    message.sent_us = now_us();
    /* Held until the host releases it. */
    held[frame->layer == ZEBKIT_LAYER_POPUP][frame->sequence % HELD] = frame;
    send_message(&message, sizeof(message), fds, fd_count);
}

static void on_popup(zebkit_view *v, bool shown, const zebkit_rect *bounds, void *data) {
    (void)v, (void)data;
    zko_popup message = {{ZKO_POPUP, sizeof(zko_popup)}, shown, {0, 0, 0, 0}};
    if (bounds)
        message.bounds[0] = bounds->x, message.bounds[1] = bounds->y, message.bounds[2] = bounds->width,
        message.bounds[3] = bounds->height;
    send_message(&message, sizeof(message), NULL, 0);
}

static void on_cursor(zebkit_view *v, const char *name, const zebkit_cursor_image *image, void *data) {
    (void)v, (void)image, (void)data;
    zko_cursor message = {{ZKO_CURSOR, sizeof(zko_cursor)}, {0}};
    snprintf(message.name, sizeof(message.name), "%s", name);
    send_message(&message, sizeof(message), NULL, 0);
}

static void on_caret(zebkit_view *v, const zebkit_rect *rect, void *data) {
    (void)v, (void)data;
    zko_caret message = {{ZKO_CARET, sizeof(zko_caret)}, rect != NULL, {0, 0, 0, 0}};
    if (rect)
        message.rect[0] = rect->x, message.rect[1] = rect->y, message.rect[2] = rect->width,
        message.rect[3] = rect->height;
    send_message(&message, sizeof(message), NULL, 0);
}

static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)data;
    if (event == ZEBKIT_LOAD_FINISHED)
        send_text(ZKO_LOADED, 0, url);
}

static void on_result(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v;
    send_text(ZKO_RESULT, (uint32_t)(uintptr_t)data, json ? json : error);
}

static void on_close(zebkit_view *v, void *data) {
    (void)v, (void)data;
    view = NULL;
    zebkit_quit();
}

/* A message from the host, on the engine's thread. */
static void handle(void *data) {
    zko_header *header = data;
    if (!view) {
        free(data);
        return;
    }
    switch (header->type) {
    case ZKO_RELEASE: {
        zko_release *m = data;
        const zebkit_frame **slot = &held[m->layer == 1][m->sequence % HELD];
        if (*slot && (*slot)->sequence == m->sequence) {
            zebkit_frame_release(*slot);
            *slot = NULL;
        }
        break;
    }
    case ZKO_MOUSE: {
        zko_mouse *m = data;
        zebkit_view_send_mouse(view, m->type, m->x, m->y, m->button, m->click_count, m->modifiers);
        break;
    }
    case ZKO_WHEEL: {
        zko_wheel *m = data;
        zebkit_view_send_wheel(view, m->x, m->y, m->delta_x, m->delta_y, m->precise, m->phase, m->modifiers);
        break;
    }
    case ZKO_KEY: {
        zko_key *m = data;
        zebkit_key_event event = {m->type, m->modifiers, m->key_code, m->native_key_code,
                                  m->code[0] ? m->code : NULL, m->key[0] ? m->key : NULL,
                                  m->text[0] ? m->text : NULL, m->text[0] ? m->text : NULL};
        zebkit_view_send_key(view, &event);
        break;
    }
    case ZKO_IME: {
        zko_ime *m = data;
        if (m->op == ZKO_IME_COMPOSE)
            zebkit_view_ime_set_composition(view, m->text, m->selection_start, m->selection_end);
        else if (m->op == ZKO_IME_COMMIT)
            zebkit_view_ime_commit(view, m->text);
        else
            zebkit_view_ime_cancel(view);
        break;
    }
    case ZKO_FOCUS:
        zebkit_view_set_focus(view, ((zko_value *)data)->value != 0);
        break;
    case ZKO_RESIZE: {
        zko_resize *m = data;
        zebkit_view_set_size(view, m->width, m->height, m->scale);
        break;
    }
    case ZKO_LOAD:
        zebkit_view_load_url(view, ((zko_text *)data)->text);
        break;
    case ZKO_EVAL: {
        zko_text *m = data;
        zebkit_view_evaluate(view, m->text, on_result, (void *)(uintptr_t)m->id);
        break;
    }
    case ZKO_RATE:
        zebkit_view_set_frame_rate(view, ((zko_value *)data)->value);
        break;
    case ZKO_IN_FLIGHT:
        zebkit_view_set_max_frames_in_flight(view, ((zko_value *)data)->value);
        break;
    case ZKO_MEASURE_JPEG:
        measure_jpeg = 1;
        zebkit_view_request_frame(view);
        break;
    case ZKO_QUIT:
        zebkit_view_close(view);
        break;
    }
    free(data);
}

static void host_gone(void *data) {
    (void)data;
    if (view)
        zebkit_view_close(view);
    else
        zebkit_quit();
}

static void *reader(void *data) {
    (void)data;
    for (;;) {
        char *buffer = malloc(sizeof(zko_text) + 64);
        int fds[4], fd_count;
        ssize_t n = zko_receive(sock, buffer, sizeof(zko_text) + 64, fds, &fd_count);
        for (int i = 0; i < fd_count; ++i)
            close(fds[i]);
        if (n < 0) {
            free(buffer);
            zebkit_post_task(host_gone, NULL);
            return NULL;
        }
        zebkit_post_task(handle, buffer);
    }
}

static const char *url = "about:blank";
static int width = 800, height = 600;
static float scale = 1;

static void ready(void *data) {
    (void)data;
    view = zebkit_view_new_offscreen(url, width, height, scale);
    if (!view) {
        fprintf(stderr, "render: no off-screen view (is it --headless?)\n");
        zebkit_quit();
        return;
    }
    zebkit_view_on_close(view, on_close, NULL);
    zebkit_view_on_load(view, on_load, NULL);
    zebkit_view_on_popup(view, on_popup, NULL);
    zebkit_view_on_cursor(view, on_cursor, NULL);
    zebkit_view_on_caret(view, on_caret, NULL);
    zebkit_view_on_frame(view, on_frame, NULL);
    zko_hello hello = {{ZKO_HELLO, sizeof(zko_hello)}, ZKO_VERSION, (int32_t)getpid()};
    send_message(&hello, sizeof(hello), NULL, 0);
    pthread_t thread;
    pthread_create(&thread, NULL, reader, NULL);
    pthread_detach(thread);
}

static const char *option(int argc, char **argv, const char *name) {
    size_t length = strlen(name);
    for (int i = 1; i < argc; ++i)
        if (strncmp(argv[i], name, length) == 0 && argv[i][length] == '=')
            return argv[i] + length + 1;
    return NULL;
}

int main(int argc, char **argv) {
    const char *value = option(argc, argv, "--offscreen-socket");
    if (!value) {
        fprintf(stderr, "render: started by the host (show), with --offscreen-socket=<fd>\n");
        return 2;
    }
    sock = atoi(value);
    if ((value = option(argc, argv, "--offscreen-url")))
        url = value;
    if ((value = option(argc, argv, "--offscreen-size")))
        sscanf(value, "%dx%d@%f", &width, &height, &scale);
#ifdef __APPLE__
    if ((value = option(argc, argv, "--offscreen-mach")) &&
        bootstrap_look_up(bootstrap_port, value, &host_port) != KERN_SUCCESS) {
        fprintf(stderr, "render: no mach port %s\n", value);
        return 2;
    }
#endif
    return zebkit_main(argc, argv, ready, NULL);
}
