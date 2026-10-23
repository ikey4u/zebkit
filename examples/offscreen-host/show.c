/*
 * The host: shows a page that another process draws, with no web engine
 * of its own. It starts the renderer (render.c) and talks to it over the
 * protocol of protocol.h: frames come as shared memory (Linux) or
 * IOSurfaces (macOS), the host keeps a picture of the view made from them
 * (the page, a popup over it) as a window would draw them, and sends
 * input back.
 *
 *   show --renderer=<render> [--url=<url>] [--size=<w>x<h>@<scale>]
 *        [--shots=<dir>] [--test] [-- <engine switches>]
 *
 * Without a window system to draw in, the picture goes to PNG files
 * (--shots). --test drives a page (tests/engine/pages/offscreen.html)
 * through the renderer and checks what comes back: the pixels, a click,
 * typing, an input method's Chinese, a <select>'s list, a resize; and
 * measures the frames: from the page's drawing to the host, bytes a frame
 * on the socket against a JPEG screencast's, the processes' CPU.
 *
 * Linking nothing of zebkit's, this is what an application's own window
 * does: on macOS it wraps the IOSurface in a CVPixelBuffer and paints it
 * (gpui's paint_surface does that); on Linux it uploads the damage of the
 * shared memory to its texture.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#endif

static int sock = -1;
static pid_t renderer;

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

/* ---- Buffers: mapped once each ------------------------------------------- */

typedef struct {
    uint64_t id;
    int fd;
    void *map;
    size_t size;
#ifdef __APPLE__
    IOSurfaceRef surface;
    CVPixelBufferRef pixel_buffer;
#endif
} buffer;

#define BUFFERS 32
static buffer buffers[BUFFERS];
static int buffer_next;

static buffer *find_buffer(uint64_t id) {
    for (int i = 0; i < BUFFERS; ++i)
        if (buffers[i].id == id)
            return &buffers[i];
    return NULL;
}

static buffer *new_buffer(uint64_t id) {
    buffer *b = &buffers[buffer_next];
    buffer_next = (buffer_next + 1) % BUFFERS;
    if (b->map)
        munmap(b->map, b->size);
    if (b->fd > 0)
        close(b->fd);
#ifdef __APPLE__
    if (b->pixel_buffer)
        CVPixelBufferRelease(b->pixel_buffer);
    if (b->surface)
        CFRelease(b->surface);
#endif
    memset(b, 0, sizeof(*b));
    b->id = id;
    b->fd = -1;
    return b;
}

#ifdef __APPLE__
static mach_port_t receive_port = MACH_PORT_NULL;

/* IOSurfaces arrive by mach message before the frame that names them. */
static buffer *receive_surface(uint64_t id) {
    for (int tries = 0; tries < 50; ++tries) {
        zko_surface_received message;
        memset(&message, 0, sizeof(message));
        kern_return_t kr = mach_msg(&message.message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(message),
                                    receive_port, 100, MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
            continue;
        buffer *b = new_buffer(message.message.buffer_id);
        b->surface = IOSurfaceLookupFromMachPort(message.message.surface.name);
        mach_port_deallocate(mach_task_self(), message.message.surface.name);
        /* What a window does with it: a CVPixelBuffer, as gpui's paint_surface takes. */
        if (b->surface)
            CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, b->surface, NULL, &b->pixel_buffer);
        if (b->id == id)
            return b;
    }
    return NULL;
}
#endif

/* ---- What the host shows -------------------------------------------------- */

typedef struct {
    int width, height;
    float scale;
    int bounds[4];
    uint8_t *pixels; /* BGRA */
    uint64_t sequence;
    int frames;
} layer;

static layer layers[2];
static int popup_shown;
static int popup_bounds[4];
static char cursor[32];
static int caret_shown, caret[4];
static char loaded[1024];
static char result[8192];
static uint32_t result_id, waiting_id;
static zko_jpeg jpeg;
static int have_jpeg;

/* Measurements. */
static int64_t transit_sum, transit_max, drawn_sum, drawn_max;
static int64_t transit_n;
static int64_t socket_bytes, socket_frames, fds_passed;
static int64_t copied_bytes;

static void send_message(const void *message) { zko_send(sock, message, NULL, 0); }

static void release(uint32_t layer_index, uint64_t sequence) {
    zko_release message = {{ZKO_RELEASE, sizeof(zko_release)}, layer_index, 0, sequence};
    send_message(&message);
}

/* Copies a frame's damage into the layer, as a window would upload it to
 * its texture, then gives the buffer back. */
static void take_frame(const zko_frame *f, const int *fds, int fd_count) {
    int64_t received = now_us();
    buffer *b = find_buffer(f->buffer_id);
    if (!b && f->kind != ZKO_IOSURFACE) {
        b = new_buffer(f->buffer_id);
        if (fd_count > 0) {
            b->fd = fds[0];
            b->size = f->kind == ZKO_SHARED_MEMORY ? f->size : (size_t)f->offsets[0] + (size_t)f->strides[0] * f->height;
            b->map = mmap(NULL, b->size, PROT_READ, MAP_SHARED, b->fd, 0);
            if (b->map == MAP_FAILED)
                b->map = NULL;
        }
        for (int i = 1; i < fd_count; ++i)
            close(fds[i]);
    } else {
        for (int i = 0; i < fd_count; ++i)
            close(fds[i]);
    }
#ifdef __APPLE__
    if (!b && f->kind == ZKO_IOSURFACE)
        b = receive_surface(f->buffer_id);
#endif
    layer *l = &layers[f->layer ? 1 : 0];
    if (l->width != f->width || l->height != f->height) {
        free(l->pixels);
        l->pixels = calloc((size_t)f->width * f->height, 4);
    }
    l->width = f->width, l->height = f->height, l->scale = f->scale;
    memcpy(l->bounds, f->bounds, sizeof(l->bounds));
    l->sequence = f->sequence;
    ++l->frames;

    const uint8_t *base = NULL;
    size_t stride = 0;
    if (b && f->kind == ZKO_SHARED_MEMORY && b->map) {
        base = (const uint8_t *)b->map + f->offset;
        stride = f->stride;
    } else if (b && f->kind == ZKO_DMABUF && b->map && f->modifier == 0) {
        base = (const uint8_t *)b->map + f->offsets[0];
        stride = f->strides[0];
    }
#ifdef __APPLE__
    if (b && f->kind == ZKO_IOSURFACE && b->pixel_buffer) {
        CVPixelBufferLockBaseAddress(b->pixel_buffer, kCVPixelBufferLock_ReadOnly);
        base = CVPixelBufferGetBaseAddress(b->pixel_buffer);
        stride = CVPixelBufferGetBytesPerRow(b->pixel_buffer);
    }
#endif
    if (base) {
        for (uint32_t i = 0; i < f->damage_count; ++i) {
            int x = f->damage[i][0], y = f->damage[i][1], w = f->damage[i][2], h = f->damage[i][3];
            if (x < 0 || y < 0 || x + w > f->width || y + h > f->height)
                continue;
            for (int row = y; row < y + h; ++row)
                memcpy(l->pixels + ((size_t)row * f->width + x) * 4, base + row * stride + x * 4, (size_t)w * 4);
            copied_bytes += (int64_t)w * h * 4;
        }
    }
#ifdef __APPLE__
    if (b && f->kind == ZKO_IOSURFACE && b->pixel_buffer)
        CVPixelBufferUnlockBaseAddress(b->pixel_buffer, kCVPixelBufferLock_ReadOnly);
#endif
    release(f->layer, f->sequence);

    /* From the frame ready in the renderer to here, and from the message sent. */
    int64_t transit = received - f->sent_us, ready = received - f->ready_us;
    if (f->sent_us && transit >= 0 && transit < 1000000 && ready >= 0 && ready < 1000000) {
        transit_sum += transit, ++transit_n;
        if (transit > transit_max)
            transit_max = transit;
        drawn_sum += ready;
        if (ready > drawn_max)
            drawn_max = ready;
    }
    socket_bytes += sizeof(zko_frame);
    ++socket_frames;
    fds_passed += fd_count;
}

/* One message, if one comes within @timeout_ms. */
static int pump_once(int timeout_ms) {
    struct pollfd p = {sock, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0)
        return 0;
    static char data[sizeof(zko_text) + 64];
    int fds[4], fd_count = 0;
    if (zko_receive(sock, data, sizeof(data), fds, &fd_count) < 0) {
        fprintf(stderr, "show: the renderer is gone\n");
        exit(1);
    }
    zko_header *header = (zko_header *)data;
    switch (header->type) {
    case ZKO_FRAME:
        take_frame((zko_frame *)data, fds, fd_count);
        break;
    case ZKO_POPUP: {
        zko_popup *m = (zko_popup *)data;
        popup_shown = m->shown;
        memcpy(popup_bounds, m->bounds, sizeof(popup_bounds));
        if (!m->shown)
            layers[1].frames = 0;
        break;
    }
    case ZKO_CURSOR:
        snprintf(cursor, sizeof(cursor), "%.*s", (int)sizeof(cursor) - 1, ((zko_cursor *)data)->name);
        break;
    case ZKO_CARET: {
        zko_caret *m = (zko_caret *)data;
        caret_shown = m->shown;
        memcpy(caret, m->rect, sizeof(caret));
        break;
    }
    case ZKO_RESULT:
        snprintf(result, sizeof(result), "%.*s", (int)sizeof(result) - 1, ((zko_text *)data)->text);
        result_id = ((zko_text *)data)->id;
        break;
    case ZKO_LOADED:
        snprintf(loaded, sizeof(loaded), "%.*s", (int)sizeof(loaded) - 1, ((zko_text *)data)->text);
        break;
    case ZKO_JPEG:
        jpeg = *(zko_jpeg *)data;
        have_jpeg = 1;
        break;
    default:
        for (int i = 0; i < fd_count; ++i)
            close(fds[i]);
        break;
    }
    return 1;
}

static void pump(int ms) {
    int64_t until = now_us() + (int64_t)ms * 1000;
    for (int64_t left; (left = until - now_us()) > 0;)
        pump_once((int)(left / 1000) + 1);
}

static int wait_until(int (*done)(void), int timeout_ms) {
    int64_t until = now_us() + (int64_t)timeout_ms * 1000;
    while (!done()) {
        if (now_us() > until)
            return 0;
        pump_once(20);
    }
    return 1;
}

static int got_result(void) { return result_id == waiting_id; }

static const char *eval(const char *script) {
    static uint32_t next_id;
    zko_text message = {{ZKO_EVAL, sizeof(zko_text)}, ++next_id, {0}};
    snprintf(message.text, sizeof(message.text), "%s", script);
    waiting_id = message.id;
    send_message(&message);
    if (!wait_until(got_result, 5000))
        return "(no answer)";
    return result;
}

/* ---- Input ---------------------------------------------------------------- */

static void mouse(uint32_t type, float x, float y, uint32_t button, int clicks) {
    zko_mouse m = {{ZKO_MOUSE, sizeof(zko_mouse)}, type, button, x, y, clicks, 0};
    send_message(&m);
}

static void click(float x, float y) {
    mouse(0, x, y, 0, 0);
    mouse(1, x, y, 1, 1);
    mouse(2, x, y, 1, 1);
}

static void key(int code, const char *dom_code, const char *dom_key, const char *text) {
    zko_key k;
    memset(&k, 0, sizeof(k));
    k.header = (zko_header){ZKO_KEY, sizeof(zko_key)};
    k.key_code = code;
    snprintf(k.code, sizeof(k.code), "%s", dom_code);
    snprintf(k.key, sizeof(k.key), "%s", dom_key);
    send_message(&k);
    if (text) {
        k.type = 2;
        snprintf(k.text, sizeof(k.text), "%s", text);
        send_message(&k);
        k.text[0] = 0;
    }
    k.type = 1;
    send_message(&k);
}

static void ime(uint32_t op, const char *text) {
    zko_ime m;
    memset(&m, 0, sizeof(m));
    m.header = (zko_header){ZKO_IME, sizeof(zko_ime)};
    m.op = op, m.selection_start = m.selection_end = -1;
    snprintf(m.text, sizeof(m.text), "%s", text ? text : "");
    send_message(&m);
}

static void value(uint32_t type, int v) {
    zko_value m = {{type, sizeof(zko_value)}, v};
    send_message(&m);
}

/* ---- The picture ---------------------------------------------------------- */

/* The view as a window would show it: the page, the popup over it. */
static uint8_t *compose(int *width, int *height) {
    layer *page = &layers[0], *popup = &layers[1];
    *width = page->width, *height = page->height;
    uint8_t *out = malloc((size_t)page->width * page->height * 4);
    memcpy(out, page->pixels, (size_t)page->width * page->height * 4);
    if (popup_shown && popup->frames && popup->pixels) {
        int ox = (int)(popup->bounds[0] * page->scale), oy = (int)(popup->bounds[1] * page->scale);
        for (int y = 0; y < popup->height; ++y) {
            int ty = oy + y;
            if (ty < 0 || ty >= page->height)
                continue;
            for (int x = 0; x < popup->width; ++x) {
                int tx = ox + x;
                if (tx >= 0 && tx < page->width)
                    memcpy(out + ((size_t)ty * page->width + tx) * 4, popup->pixels + ((size_t)y * popup->width + x) * 4,
                           4);
            }
        }
    }
    return out;
}

static uint32_t pixel(double x, double y) {
    layer *page = &layers[0];
    int px = (int)(x * page->scale), py = (int)(y * page->scale);
    if (!page->pixels || px < 0 || py < 0 || px >= page->width || py >= page->height)
        return 0;
    const uint8_t *v = page->pixels + ((size_t)py * page->width + px) * 4;
    return (uint32_t)v[2] << 16 | (uint32_t)v[1] << 8 | v[0];
}

static uint32_t crc_table[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k)
                v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c ^= 0xffffffffu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t head[8];
    be32(head, n);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    be32(head, crc(crc(0, (const uint8_t *)type, 4), data, n));
    fwrite(head, 1, 4, f);
}

/* A PNG of BGRA pixels, with stored (uncompressed) deflate blocks. */
static void write_png(const char *path, const uint8_t *bgra, int width, int height) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {0};
    be32(ihdr, (uint32_t)width), be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8, ihdr[9] = 6;
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_size = (size_t)height * (1 + (size_t)width * 4);
    uint8_t *raw = malloc(raw_size);
    for (int y = 0; y < height; ++y) {
        uint8_t *row = raw + y * (1 + (size_t)width * 4);
        row[0] = 0;
        for (int x = 0; x < width; ++x) {
            const uint8_t *p = bgra + ((size_t)y * width + x) * 4;
            row[1 + x * 4] = p[2], row[2 + x * 4] = p[1], row[3 + x * 4] = p[0], row[4 + x * 4] = p[3];
        }
    }
    size_t blocks = (raw_size + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_size + blocks * 5 + 4), *q = z;
    *q++ = 0x78, *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_size; ++i)
        a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    for (size_t off = 0; off < raw_size; off += 65535) {
        size_t n = raw_size - off < 65535 ? raw_size - off : 65535;
        *q++ = off + n == raw_size;
        *q++ = n & 0xff, *q++ = (n >> 8) & 0xff, *q++ = ~n & 0xff, *q++ = (~n >> 8) & 0xff;
        memcpy(q, raw + off, n);
        q += n;
    }
    be32(q, (b << 16) | a);
    q += 4;
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

static const char *shots;

static void shoot(const char *name) {
    if (!shots || !layers[0].pixels)
        return;
    int w, h;
    uint8_t *picture = compose(&w, &h);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", shots, name);
    write_png(path, picture, w, h);
    free(picture);
    printf("     picture: %s\n", path);
}

/* ---- CPU ------------------------------------------------------------------ */

/* The CPU time of a process and its descendants, in microseconds (Linux). */
static int64_t tree_cpu_us(pid_t root) {
#ifdef __linux__
    int64_t total = 0;
    long ticks = sysconf(_SC_CLK_TCK);
    pid_t tree[256];
    int count = 1;
    tree[0] = root;
    FILE *ps = popen("ps -e -o pid=,ppid=", "r");
    if (ps) {
        int pid, ppid;
        /* A few passes: children are listed after their parents, mostly. */
        int pairs[4096][2], n = 0;
        while (n < 4096 && fscanf(ps, "%d %d", &pid, &ppid) == 2)
            pairs[n][0] = pid, pairs[n][1] = ppid, ++n;
        pclose(ps);
        for (int pass = 0; pass < 4; ++pass)
            for (int i = 0; i < n; ++i) {
                int in_tree = 0, parent_in = 0;
                for (int k = 0; k < count; ++k) {
                    in_tree |= tree[k] == pairs[i][0];
                    parent_in |= tree[k] == pairs[i][1];
                }
                if (!in_tree && parent_in && count < 256)
                    tree[count++] = pairs[i][0];
            }
    }
    for (int k = 0; k < count; ++k) {
        char path[64], line[1024];
        snprintf(path, sizeof(path), "/proc/%d/stat", tree[k]);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(line, sizeof(line), f)) {
            char *p = strrchr(line, ')');
            unsigned long utime = 0, stime = 0;
            if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) == 2)
                total += (int64_t)(utime + stime) * 1000000 / ticks;
        }
        fclose(f);
    }
    return total;
#else
    (void)root;
    return -1;
#endif
}

static int64_t self_cpu_us(void) {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (int64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000 + usage.ru_utime.tv_usec +
           usage.ru_stime.tv_usec;
}

/* ---- The test ------------------------------------------------------------- */

static int failures, checks;

static void check(int ok, const char *what, const char *format, ...) {
    ++checks;
    if (!ok)
        ++failures;
    char detail[512] = "";
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf(detail, sizeof(detail), format, args);
        va_end(args);
    }
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, *detail ? ": " : "", detail);
    fflush(stdout);
}

static int page_drawn(void) { return layers[0].frames > 0 && pixel(300, 280) == 0x102030 && loaded[0]; }
static int popup_drawn(void) { return popup_shown && layers[1].frames > 0; }
static int resized(void) { return layers[0].width == 1280 && layers[0].height == 720; }
static int jpeg_came(void) { return have_jpeg; }
static uint64_t sequence_before;
static int box_blue(void) { return layers[0].sequence > sequence_before && pixel(40, 35) == 0x0000ff; }

static int run_test(void) {
    int drawn = wait_until(page_drawn, 20000);
    check(drawn, "the page's first frame, in this process", "%d frames", layers[0].frames);
    check(pixel(40, 35) == 0xff0000 && pixel(300, 280) == 0x102030, "its pixels are the page's", "%06x %06x",
          pixel(40, 35), pixel(300, 280));
    shoot("host-first");

    /* From the host's input to its picture: a click that turns the box blue. */
    eval("document.getElementById('button').onclick = () => {"
         " document.body.dataset.clicks = Number(document.body.dataset.clicks || 0) + 1;"
         " document.getElementById('box').style.background = 'rgb(0, 0, 255)'; }; 1");
    pump(300);
    sequence_before = layers[0].sequence;
    int64_t sent = now_us();
    click(160, 40);
    int ok = wait_until(box_blue, 3000);
    int64_t took = now_us() - sent;
    check(ok, "a click sent from this process changes the page, and the frame shows it",
          "%.1f ms from the click to the picture", took / 1000.0);
    check(strcmp(eval("document.body.dataset.clicks"), "\"1\"") == 0, "the page counted one click", "%s", result);
    check(strcmp(cursor, "pointer") == 0, "the cursor over the button", "%s", cursor);

    value(ZKO_FOCUS, 1);
    click(120, 115);
    key(0x41, "KeyA", "a", "a");
    key(0x42, "KeyB", "b", "b");
    key(0x43, "KeyC", "c", "c");
    ime(ZKO_IME_COMPOSE, "zhong");
    ime(ZKO_IME_COMMIT, "中文");
    pump(500);
    check(strcmp(eval("document.getElementById('field').value"), "\"abc中文\"") == 0,
          "typing and an input method's Chinese", "%s", result);
    check(caret_shown && caret[1] >= 100 && caret[1] <= 130, "the caret, for the input method's window",
          "%d,%d %dx%d", caret[0], caret[1], caret[2], caret[3]);
    shoot("host-typed");

    click(100, 175);
    int shown = wait_until(popup_drawn, 5000);
    check(shown && popup_bounds[1] >= 185, "a <select>'s list, as a layer over the page, under it", "at %d,%d %dx%d",
          popup_bounds[0], popup_bounds[1], popup_bounds[2], popup_bounds[3]);
    pump(200);
    shoot("host-select-open");
    key(0x28, "ArrowDown", "ArrowDown", NULL);
    key(0x28, "ArrowDown", "ArrowDown", NULL);
    key(0x0D, "Enter", "Enter", NULL);
    pump(400);
    check(strcmp(eval("document.getElementById('pick').value"), "\"c\"") == 0 && !popup_shown,
          "chosen from it, and the list gone", "%s", result);
    shoot("host-select-chosen");

    /* Frames for a while: what they cost. */
    transit_sum = transit_max = drawn_sum = drawn_max = transit_n = 0;
    socket_bytes = socket_frames = copied_bytes = 0;
    int frames_before = layers[0].frames;
    int64_t cpu_renderer = tree_cpu_us(renderer), cpu_self = self_cpu_us();
    int64_t start = now_us();
    eval("animate(3000), 1");
    pump(1000);
    send_message(&(zko_header){ZKO_MEASURE_JPEG, sizeof(zko_header)});
    pump(2200);
    int64_t elapsed = now_us() - start;
    int frames = layers[0].frames - frames_before;
    cpu_renderer = tree_cpu_us(renderer) - cpu_renderer;
    cpu_self = self_cpu_us() - cpu_self;
    wait_until(jpeg_came, 3000);
    check(frames > 30, "an animation's frames", "%d in %.1f s", frames, elapsed / 1e6);
    printf("     frames: %.1f a second at %dx%d\n", frames * 1e6 / elapsed, layers[0].width, layers[0].height);
    if (transit_n)
        printf("     from the frame ready in the renderer to the host: %.2f ms on average, %.2f at most; the message "
               "alone %.3f ms\n",
               drawn_sum / 1000.0 / transit_n, drawn_max / 1000.0, transit_sum / 1000.0 / transit_n);
    printf("     on the socket: %lld bytes a frame and no pixels; copied into the host's picture: %lld bytes a frame "
           "(the damage)\n",
           socket_frames ? (long long)(socket_bytes / socket_frames) : 0,
           socket_frames ? (long long)(copied_bytes / socket_frames) : 0);
    if (have_jpeg)
        printf("     a JPEG screencast frame would be %llu bytes (%llu as CDP's base64), %.2f ms to encode, for %d "
               "pixels\n",
               (unsigned long long)jpeg.bytes, (unsigned long long)jpeg.base64, jpeg.encode_us / 1000.0,
               jpeg.width * jpeg.height);
    if (tree_cpu_us(renderer) >= 0)
        printf("     CPU: renderer and its processes %.0f%%, host %.0f%% of a core\n", cpu_renderer * 100.0 / elapsed,
               cpu_self * 100.0 / elapsed);
    else
        printf("     CPU: host %.0f%% of a core (the renderer's is not measured here)\n", cpu_self * 100.0 / elapsed);

    /* Idle: nothing comes. */
    pump(500);
    frames_before = layers[0].frames;
    cpu_renderer = tree_cpu_us(renderer);
    pump(2000);
    cpu_renderer = tree_cpu_us(renderer) - cpu_renderer;
    check(layers[0].frames == frames_before, "an idle page sends nothing", "%d frames in 2 s",
          layers[0].frames - frames_before);
    if (tree_cpu_us(renderer) >= 0)
        printf("     CPU while idle: %.1f%% of a core\n", cpu_renderer * 100.0 / 2e6);

    zko_resize resize = {{ZKO_RESIZE, sizeof(zko_resize)}, 640, 360, 2};
    send_message(&resize);
    int ok_size = wait_until(resized, 5000);
    check(ok_size, "resized to 640x360 at 2: 1280x720 pixels", "%dx%d at %g", layers[0].width, layers[0].height,
          layers[0].scale);
    pump(300);
    /* The animation ended with the box red again. */
    check(pixel(40, 35) == 0xff0000 && pixel(600, 340) == 0x102030, "drawn again at the new size", "%06x %06x",
          pixel(40, 35), pixel(600, 340));
    shoot("host-resized");

    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}

static const char *option(int argc, char **argv, const char *name) {
    size_t length = strlen(name);
    for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; ++i)
        if (strncmp(argv[i], name, length) == 0 && argv[i][length] == '=')
            return argv[i] + length + 1;
    return NULL;
}

static int has(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc && strcmp(argv[i], "--") != 0; ++i)
        if (strcmp(argv[i], name) == 0)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = option(argc, argv, "--renderer");
    const char *url = option(argc, argv, "--url");
    const char *size = option(argc, argv, "--size");
    shots = option(argc, argv, "--shots");
    if (!path) {
        fprintf(stderr, "usage: show --renderer=<render> [--url=<url>] [--size=WxH@scale] [--shots=<dir>] [--test] "
                        "[-- <engine switches>]\n");
        return 2;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
        perror("socketpair");
        return 1;
    }
    sock = pair[0];
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    char mach_name[128] = "";
#ifdef __APPLE__
    snprintf(mach_name, sizeof(mach_name), "org.zebkit.offscreen-host.%d", (int)getpid());
    if (bootstrap_check_in(bootstrap_port, mach_name, &receive_port) != KERN_SUCCESS) {
        fprintf(stderr, "show: no mach service %s\n", mach_name);
        return 1;
    }
#endif

    char socket_arg[64], url_arg[2048], size_arg[64], mach_arg[160];
    snprintf(socket_arg, sizeof(socket_arg), "--offscreen-socket=%d", pair[1]);
    snprintf(url_arg, sizeof(url_arg), "--offscreen-url=%s", url ? url : "about:blank");
    snprintf(size_arg, sizeof(size_arg), "--offscreen-size=%s", size ? size : "400x300@1");
    snprintf(mach_arg, sizeof(mach_arg), "--offscreen-mach=%s", mach_name);
    char *args[64];
    int n = 0;
    args[n++] = (char *)path;
    args[n++] = "--headless";
    args[n++] = socket_arg;
    args[n++] = url_arg;
    args[n++] = size_arg;
    if (mach_name[0])
        args[n++] = mach_arg;
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--") == 0)
            for (++i; i < argc && n < 63; ++i)
                args[n++] = argv[i];
    args[n] = NULL;

    renderer = fork();
    if (renderer == 0) {
        close(pair[0]);
        execv(path, args);
        perror("execv");
        _exit(127);
    }
    close(pair[1]);

    int code = 0;
    if (has(argc, argv, "--test")) {
        code = run_test();
    } else {
        /* Shows the page until the renderer ends: a picture a second. */
        for (int i = 0;; ++i) {
            pump(1000);
            char name[32];
            snprintf(name, sizeof(name), "host-%04d", i);
            if (i % 5 == 0)
                shoot(name);
        }
    }
    send_message(&(zko_header){ZKO_QUIT, sizeof(zko_header)});
    int status = 0;
    for (int i = 0; i < 100 && waitpid(renderer, &status, WNOHANG) == 0; ++i)
        usleep(50000);
    if (waitpid(renderer, &status, WNOHANG) == 0)
        kill(renderer, SIGKILL);
    return code;
}
