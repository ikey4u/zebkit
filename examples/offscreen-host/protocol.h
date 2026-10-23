/*
 * The reference protocol between a process that draws pages off-screen
 * with zebkit (render.c, "the renderer") and one that shows them (show.c,
 * "the host"), such as an application's window that has no web engine of
 * its own. Neither side needs anything of the other's but this header.
 *
 * Transport: a Unix stream socket, which the host makes with socketpair()
 * and gives the renderer when it starts it (--offscreen-socket=<fd>).
 * Every message starts with zko_header, whose size says where it ends;
 * file descriptors go with SCM_RIGHTS on a message's first bytes.
 * Integers are the machine's own: both ends are on it. zko_send() and
 * zko_receive() below are the whole of it.
 *
 * Frames are not copied through the socket, only described:
 *
 *   Linux   a frame's buffer is a file descriptor (shared memory) or a
 *           dmabuf's planes, passed with SCM_RIGHTS the first time the
 *           buffer is used (zko_frame.fd_count > 0); later frames in the
 *           same buffer name it by buffer_id, and the host keeps its
 *           mapping of it. A buffer of another size replaces the old ones.
 *   macOS   a frame's IOSurface goes as a mach port send right, in a mach
 *           message to the port the host registers with launchd under the
 *           name it passes as --offscreen-mach=<name> (bootstrap_check_in
 *           there, bootstrap_look_up here), once per buffer, before the
 *           first zko_frame that names it: zko_surface_message. The host
 *           turns it into an IOSurfaceRef with IOSurfaceLookupFromMachPort,
 *           and that into a CVPixelBuffer (CVPixelBufferCreateWithIOSurface)
 *           or a Metal texture (newTextureWithDescriptor:iosurface:plane:).
 *
 * Every frame is the whole picture; damage says what changed since the
 * layer's previous frame. The host answers each one with ZKO_RELEASE once
 * it no longer reads the buffer: the renderer has at most
 * zebkit_view_set_max_frames_in_flight() frames out, and keeps the newest
 * of the rest until one comes back.
 */

#ifndef ZEBKIT_EXAMPLES_OFFSCREEN_PROTOCOL_H
#define ZEBKIT_EXAMPLES_OFFSCREEN_PROTOCOL_H

#include <stdint.h>

#define ZKO_VERSION 1

enum zko_type {
    /* renderer -> host */
    ZKO_HELLO = 1,   /* zko_hello: the renderer is up */
    ZKO_FRAME = 2,   /* zko_frame, with fds for a new buffer on Linux */
    ZKO_POPUP = 3,   /* zko_popup */
    ZKO_CURSOR = 4,  /* zko_cursor */
    ZKO_CARET = 5,   /* zko_caret */
    ZKO_RESULT = 6,  /* zko_text: a script's value as JSON, for ZKO_EVAL */
    ZKO_LOADED = 7,  /* zko_text: the page's URL, loaded */
    ZKO_JPEG = 8,    /* zko_jpeg: for ZKO_MEASURE_JPEG */

    /* host -> renderer */
    ZKO_RELEASE = 32,  /* zko_release */
    ZKO_MOUSE = 33,    /* zko_mouse */
    ZKO_WHEEL = 34,    /* zko_wheel */
    ZKO_KEY = 35,      /* zko_key */
    ZKO_IME = 36,      /* zko_ime */
    ZKO_FOCUS = 37,    /* zko_value: 1 or 0 */
    ZKO_RESIZE = 38,   /* zko_resize */
    ZKO_LOAD = 39,     /* zko_text: a URL */
    ZKO_EVAL = 40,     /* zko_text: script, answered by ZKO_RESULT with the same id */
    ZKO_RATE = 41,     /* zko_value: frames a second at most, 0 for no limit */
    ZKO_IN_FLIGHT = 42, /* zko_value: frames the host may hold */
    ZKO_MEASURE_JPEG = 43, /* the next frame also encoded as a JPEG, as a screencast would: its size */
    ZKO_QUIT = 44,
};

typedef struct {
    uint32_t type;
    uint32_t size; /* of the whole message */
} zko_header;

typedef struct {
    zko_header header;
    uint32_t version;
    int32_t pid;
} zko_hello;

enum zko_kind { ZKO_SHARED_MEMORY = 1, ZKO_IOSURFACE = 2, ZKO_DMABUF = 3 };

typedef struct {
    zko_header header;
    uint32_t layer; /* 0 the page, 1 a popup */
    uint32_t kind;  /* zko_kind */
    uint64_t sequence;
    int32_t bounds[4]; /* x, y, width, height in the view, CSS pixels */
    int32_t width, height;
    float scale;
    uint32_t format;    /* zebkit_pixel_format: 1 BGRA */
    uint64_t buffer_id; /* the buffer, as the renderer names it */
    uint32_t fd_count;  /* fds passed with this message: a new buffer */
    uint32_t plane_count;
    uint64_t stride, offset, size; /* shared memory: the pixels at offset, stride a row, the map's size */
    uint32_t strides[4], offsets[4]; /* a dmabuf's planes */
    uint64_t modifier;
    uint32_t fourcc;
    uint32_t damage_count;
    int32_t damage[4][4]; /* x, y, width, height in the frame's pixels */
    int64_t drawn_us;     /* the page drawn, the frame ready, the message sent: CLOCK_MONOTONIC */
    int64_t ready_us;
    int64_t sent_us;
} zko_frame;

typedef struct {
    zko_header header;
    uint32_t layer;
    uint32_t pad;
    uint64_t sequence;
} zko_release;

typedef struct {
    zko_header header;
    int32_t shown;
    int32_t bounds[4];
} zko_popup;

typedef struct {
    zko_header header;
    char name[32]; /* CSS's name of it */
} zko_cursor;

typedef struct {
    zko_header header;
    int32_t shown;
    int32_t rect[4];
} zko_caret;

typedef struct {
    zko_header header;
    uint32_t type; /* zebkit_mouse_type */
    uint32_t button;
    float x, y;
    int32_t click_count;
    uint32_t modifiers;
} zko_mouse;

typedef struct {
    zko_header header;
    float x, y, delta_x, delta_y;
    int32_t precise;
    uint32_t phase;
    uint32_t modifiers;
} zko_wheel;

typedef struct {
    zko_header header;
    uint32_t type; /* zebkit_key_type */
    uint32_t modifiers;
    int32_t key_code, native_key_code;
    char code[32], key[32], text[16];
} zko_key;

enum zko_ime_op { ZKO_IME_COMPOSE = 0, ZKO_IME_COMMIT = 1, ZKO_IME_CANCEL = 2 };

typedef struct {
    zko_header header;
    uint32_t op;
    int32_t selection_start, selection_end;
    char text[256];
} zko_ime;

typedef struct {
    zko_header header;
    int32_t width, height;
    float scale;
} zko_resize;

typedef struct {
    zko_header header;
    int32_t value;
} zko_value;

typedef struct {
    zko_header header;
    uint32_t id;
    char text[8192];
} zko_text;

typedef struct {
    zko_header header;
    int32_t width, height;
    uint64_t bytes;     /* the JPEG at quality 80 */
    uint64_t base64;    /* as CDP's Page.screencastFrame carries it */
    int64_t encode_us;  /* the time it took */
} zko_jpeg;

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

/* Sends @message (its header says its size), with @fd_count fds. */
static inline int zko_send(int sock, const void *message, const int *fds, int fd_count) {
    const zko_header *header = (const zko_header *)message;
    const char *at = (const char *)message;
    size_t left = header->size;
    char control[CMSG_SPACE(sizeof(int) * 4)];
    while (left > 0) {
        struct iovec iov = {(void *)at, left};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        if (fd_count > 0 && at == (const char *)message) {
            memset(control, 0, sizeof(control));
            msg.msg_control = control;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
            struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
            memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
        }
        ssize_t n = sendmsg(sock, &msg, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        at += n;
        left -= (size_t)n;
    }
    return 0;
}

/* Receives one message into @buffer (@capacity bytes), and the fds that
 * came with it (up to 4). Returns its size, or -1 when the other end is
 * gone. */
static inline ssize_t zko_receive(int sock, void *buffer, size_t capacity, int *fds, int *fd_count) {
    char *at = (char *)buffer;
    size_t want = sizeof(zko_header), got = 0;
    *fd_count = 0;
    while (got < want) {
        char control[CMSG_SPACE(sizeof(int) * 4)];
        struct iovec iov = {at + got, want - got};
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        ssize_t n = recvmsg(sock, &msg, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int count = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < count; ++i) {
                    int fd;
                    memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    if (*fd_count < 4)
                        fds[(*fd_count)++] = fd;
                }
            }
        }
        got += (size_t)n;
        if (got == sizeof(zko_header)) {
            want = ((zko_header *)buffer)->size;
            if (want < sizeof(zko_header) || want > capacity)
                return -1;
        }
    }
    return (ssize_t)got;
}

#ifdef __APPLE__
#include <mach/message.h>

/* A frame's IOSurface, as a mach message to the host's port. */
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t surface;
    uint64_t buffer_id;
} zko_surface_message;

typedef struct {
    zko_surface_message message;
    mach_msg_trailer_t trailer;
} zko_surface_received;
#endif

#endif /* ZEBKIT_EXAMPLES_OFFSCREEN_PROTOCOL_H */
