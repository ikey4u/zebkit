/* Off-screen views (<zebkit/offscreen.h>), used as an application that
 * draws a page itself uses them: frames arrive in shared surfaces with
 * their size, scale and damage; a small change of the page damages a
 * small part; resizing and scale; input (a click, typing, an input
 * method's Chinese, the wheel); a <select>'s list as a popup layer; no
 * frames while the page is idle; at most as many frames out as allowed,
 * and the newest after; the frame rate limit; the cursor and the caret.
 *
 *   ZEBKIT_TEST_BASE   http://127.0.0.1:<port>, serving tests/engine/pages
 *   ZEBKIT_TEST_SHOTS  a directory for pictures of the frames (optional)
 *
 * scripts/engine-test.sh runs it with --headless. Exit 0 when every check
 * passes. */

#define _GNU_SOURCE
#include "harness.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <zebkit/offscreen.h>

#ifdef __APPLE__
#include <IOSurface/IOSurfaceRef.h>
#endif


static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- What arrives ------------------------------------------------------- */

static zebkit_view *view;
static const char *shots;

/* A copy of the last frame of each layer, as BGRA rows. */
struct picture {
    int width, height;
    float scale;
    zebkit_rect bounds;
    uint64_t sequence;
    zebkit_frame_kind kind;
    zebkit_rect damage;
    zebkit_pixel_format format;
    const void *handle;
    uint8_t *pixels;
};

static struct picture last[2];
static int frames[2];          /* frames of each layer */
static int64_t frame_bytes;    /* bytes a frame takes in its buffer */
static int fds_ok = 1;         /* every shared-memory frame had an fd it can be mapped by */
static int hold;               /* keep frames instead of releasing them */
static const zebkit_frame *held[16];
static int held_count;
static int64_t latency_sum, latency_n, latency_max;
static int64_t input_at;       /* when the input a check waits on was sent */
static int64_t first_change_us; /* when the first frame after it came */
/* A D3D11 texture came (Windows, GPU buffers): this test does not read its
 * pixels. What it can check of one is (size, format, a handle); the checks of
 * pixels are one counted skip, and the waits for pixels wait for the frame. */
static int d3d11_seen, d3d11_skip_said;
static int pixels_unread(void) {
    if (!d3d11_seen)
        return 0;
    if (!d3d11_skip_said++)
        skip("the pixels of the frames", "D3D11 textures, which this test does not read");
    return 1;
}

/* The frame's pixels as BGRA rows, in this process. */
static int read_pixels(const zebkit_frame *frame, uint8_t *out) {
    const size_t row = (size_t)frame->width * 4;
    if (frame->kind == ZEBKIT_FRAME_SHARED_MEMORY) {
#ifdef _WIN32
        /* Through the file mapping as another process would. */
        void *view_of_file = MapViewOfFile((HANDLE)frame->handle, FILE_MAP_READ, 0, 0, frame->size);
        if (!view_of_file) {
            fds_ok = 0;
            return 0;
        }
        for (int y = 0; y < frame->height; ++y)
            memcpy(out + y * row, (const uint8_t *)view_of_file + frame->offset + y * frame->stride, row);
        UnmapViewOfFile(view_of_file);
        return 1;
#endif
        /* Through the fd as another process would, not only the pointer. */
        void *mapped = mmap(NULL, frame->size, PROT_READ, MAP_SHARED, frame->fd, 0);
        const uint8_t *base = frame->pixels;
        if (frame->fd >= 0 && mapped != MAP_FAILED)
            base = (const uint8_t *)mapped + frame->offset;
        else if (frame->fd >= 0)
            fds_ok = 0;
        for (int y = 0; y < frame->height; ++y)
            memcpy(out + y * row, base + y * frame->stride, row);
        if (mapped != MAP_FAILED)
            munmap(mapped, frame->size);
        return 1;
    }
#ifdef __APPLE__
    if (frame->kind == ZEBKIT_FRAME_IOSURFACE) {
        IOSurfaceRef surface = (IOSurfaceRef)frame->iosurface;
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
        const uint8_t *base = IOSurfaceGetBaseAddress(surface);
        size_t stride = IOSurfaceGetBytesPerRow(surface);
        for (int y = 0; y < frame->height; ++y)
            memcpy(out + y * row, base + y * stride, row);
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
        return 1;
    }
#endif
    if (frame->kind == ZEBKIT_FRAME_DMABUF && frame->modifier == 0 && frame->plane_count == 1) {
        size_t size = (size_t)frame->offsets[0] + (size_t)frame->strides[0] * frame->height;
        void *mapped = mmap(NULL, size, PROT_READ, MAP_SHARED, frame->fds[0], 0);
        if (mapped == MAP_FAILED)
            return 0;
        for (int y = 0; y < frame->height; ++y)
            memcpy(out + y * row, (uint8_t *)mapped + frame->offsets[0] + y * frame->strides[0], row);
        munmap(mapped, size);
        return 1;
    }
    return 0;
}

/* A PNG of a picture, uncompressed: stored deflate blocks. */
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
    fwrite(data, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)type, 4), data, n);
    be32(head, c);
    fwrite(head, 1, 4, f);
}
static void write_png(const char *path, const struct picture *p) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13];
    be32(ihdr, p->width), be32(ihdr + 4, p->height);
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0; /* RGBA */
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_size = (size_t)p->height * (1 + p->width * 4);
    uint8_t *raw = malloc(raw_size);
    for (int y = 0; y < p->height; ++y) {
        uint8_t *row = raw + y * (1 + p->width * 4);
        row[0] = 0;
        for (int x = 0; x < p->width; ++x) {
            const uint8_t *bgra = p->pixels + (y * p->width + x) * 4;
            row[1 + x * 4] = bgra[2], row[2 + x * 4] = bgra[1], row[3 + x * 4] = bgra[0], row[4 + x * 4] = bgra[3];
        }
    }
    size_t blocks = (raw_size + 65534) / 65535;
    size_t z_size = 2 + raw_size + blocks * 5 + 4;
    uint8_t *z = malloc(z_size), *q = z;
    *q++ = 0x78, *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_size; ++i)
        a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    for (size_t off = 0; off < raw_size; off += 65535) {
        size_t n = raw_size - off < 65535 ? raw_size - off : 65535;
        *q++ = off + n == raw_size;
        *q++ = n & 0xff, *q++ = n >> 8, *q++ = ~n & 0xff, *q++ = (~n >> 8) & 0xff;
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

static void save(const char *name, int layer) {
    if (!shots || !*shots || !last[layer].pixels)
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.png", shots, name);
    write_png(path, &last[layer]);
    printf("     picture: %s\n", path);
}

/* BGRA at (@x, @y) of the last frame of @layer, in CSS pixels. */
static uint32_t pixel_at(int layer, double x, double y) {
    const struct picture *p = &last[layer];
    int px = (int)(x * p->scale), py = (int)(y * p->scale);
    if (!p->pixels || px < 0 || py < 0 || px >= p->width || py >= p->height)
        return 0;
    const uint8_t *v = p->pixels + (py * p->width + px) * 4;
    return (uint32_t)v[2] << 16 | (uint32_t)v[1] << 8 | v[0]; /* 0xRRGGBB */
}

static void on_frame(zebkit_view *v, const zebkit_frame *frame, void *data) {
    (void)v, (void)data;
    int layer = frame->layer == ZEBKIT_LAYER_POPUP;
    ++frames[layer];
    struct picture *p = &last[layer];
    if (p->width != frame->width || p->height != frame->height) {
        free(p->pixels);
        p->pixels = malloc((size_t)frame->width * frame->height * 4);
    }
    p->width = frame->width, p->height = frame->height, p->scale = frame->scale;
    p->bounds = frame->bounds, p->sequence = frame->sequence, p->kind = frame->kind;
    p->format = frame->format, p->handle = frame->handle;
#ifdef _WIN32
    if (frame->kind == ZEBKIT_FRAME_D3D11)
        d3d11_seen = 1;
#endif
    p->damage = frame->damage_count ? frame->damage[0] : (zebkit_rect){0, 0, 0, 0};
#if !defined(__APPLE__) && !defined(_WIN32)
    if (frame->kind == ZEBKIT_FRAME_SHARED_MEMORY && frame->fd < 0)
        fds_ok = 0;
#endif
    frame_bytes = (int64_t)frame->width * frame->height * 4;
    if (!read_pixels(frame, p->pixels))
        memset(p->pixels, 0, (size_t)frame->width * frame->height * 4);
    int64_t latency = frame->ready_us - frame->drawn_us;
    if (frame->drawn_us && latency >= 0 && latency < 1000000) {
        latency_sum += latency, ++latency_n;
        if (latency > latency_max)
            latency_max = latency;
    }
    if (input_at && !first_change_us)
        first_change_us = now_us();
    if (hold && held_count < 16) {
        held[held_count++] = frame;
        return;
    }
    zebkit_frame_release(frame);
}

static int popup_shown, popup_events;
static zebkit_rect popup_rect;
static void on_popup(zebkit_view *v, bool shown, const zebkit_rect *bounds, void *data) {
    (void)v, (void)data;
    ++popup_events;
    popup_shown = shown;
    if (bounds)
        popup_rect = *bounds;
}

static char cursor[32];
static void on_cursor(zebkit_view *v, const char *name, const zebkit_cursor_image *image, void *data) {
    (void)v, (void)image, (void)data;
    snprintf(cursor, sizeof(cursor), "%s", name);
}

static int carets;
static zebkit_rect caret;
static void on_caret(zebkit_view *v, const zebkit_rect *rect, void *data) {
    (void)v, (void)data;
    ++carets;
    if (rect)
        caret = *rect;
}

/* ---- Steps ---------------------------------------------------------------- */

typedef void (*step_func)(void);
static step_func steps[64];
static int step_count, current;
static char result[1024];

static void run_step(void *data) {
    (void)data;
    if (current >= step_count) {
        zebkit_view_close(view);
        return;
    }
    steps[current++]();
}

static void next(uint32_t delay_ms) { zebkit_post_delayed_task(run_step, NULL, delay_ms); }

/* Waits until @done() is true, up to @timeout_ms, then checks it and goes on. */
static int (*waiting)(void);
static const char *waiting_for;
static int64_t waiting_until;
static void poll_wait(void *data) {
    (void)data;
    if (waiting()) {
        check(1, waiting_for, NULL);
        next(0);
    } else if (now_us() > waiting_until) {
        check(0, waiting_for, "timed out");
        next(0);
    } else {
        zebkit_post_delayed_task(poll_wait, NULL, 20);
    }
}
static void wait_for(int (*done)(void), uint32_t timeout_ms, const char *what) {
    waiting = done, waiting_for = what, waiting_until = now_us() + timeout_ms * 1000;
    zebkit_post_delayed_task(poll_wait, NULL, 20);
}

static void evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    snprintf(result, sizeof(result), "%s", json ? json : error ? error : "");
    next(0);
}
static void evaluate(const char *script) { zebkit_view_evaluate(view, script, evaluated, NULL); }

static void click(double x, double y) {
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_MOVE, x, y, ZEBKIT_BUTTON_NONE, 0, 0);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_DOWN, x, y, ZEBKIT_BUTTON_LEFT, 1, 0);
    zebkit_view_send_mouse(view, ZEBKIT_MOUSE_UP, x, y, ZEBKIT_BUTTON_LEFT, 1, 0);
}

static void key(int code, const char *dom_code, const char *dom_key, const char *text) {
    zebkit_key_event event = {ZEBKIT_KEY_DOWN, 0, code, 0, dom_code, dom_key, NULL, NULL};
    zebkit_view_send_key(view, &event);
    if (text) {
        event.type = ZEBKIT_KEY_CHAR;
        event.text = event.unmodified_text = text;
        zebkit_view_send_key(view, &event);
    }
    event.type = ZEBKIT_KEY_UP;
    event.text = event.unmodified_text = NULL;
    zebkit_view_send_key(view, &event);
}

/* 1. The first frame: its size, scale, kind and the page's pixels. */
static int frames_at_start;
static int have_frame(void) { return frames[0] > 0 && (d3d11_seen || pixel_at(0, 300, 280) == 0x102030); }
static void s_first_frame(void) { wait_for(have_frame, 15000, "the first frame, the page drawn"); }
static void s_check_first(void) {
    struct picture *p = &last[0];
    check(p->width == 400 && p->height == 300 && p->scale == 1, "the frame is the view's size at its scale",
          "%dx%d at %g", p->width, p->height, p->scale);
#ifdef __APPLE__
    const char *buffers = getenv("ZEBKIT_TEST_BUFFERS");
    if (buffers && strcmp(buffers, "shm") == 0)
        check(p->kind == ZEBKIT_FRAME_SHARED_MEMORY, "shared memory, as asked", "kind %d", p->kind);
    else
        check(p->kind == ZEBKIT_FRAME_IOSURFACE, "an IOSurface", "kind %d", p->kind);
#elif defined(_WIN32)
    check(p->kind == ZEBKIT_FRAME_SHARED_MEMORY || (getenv("ZEBKIT_TEST_BUFFERS") && p->kind == ZEBKIT_FRAME_D3D11),
          "shared memory (or a D3D11 texture, when GPU buffers are asked for)", "kind %d", p->kind);
    if (p->kind == ZEBKIT_FRAME_D3D11)
        check(p->handle != NULL && p->format == ZEBKIT_FORMAT_BGRA8, "a D3D11 texture: a shared handle, BGRA",
              "handle %p, format %d", p->handle, (int)p->format);
#else
    check(p->kind == ZEBKIT_FRAME_SHARED_MEMORY || (getenv("ZEBKIT_TEST_BUFFERS") && p->kind == ZEBKIT_FRAME_DMABUF),
          "shared memory (or a dmabuf, when GPU buffers are asked for)", "kind %d", p->kind);
#endif
#ifndef __APPLE__
    /* macOS's shared memory is a mach port, not an fd: IOSurfaces are for another process there. */
    check(fds_ok, "the buffer maps through its fd, as another process would map it", NULL);
#endif
    if (!pixels_unread())
        check(pixel_at(0, 300, 280) == 0x102030 && pixel_at(0, 40, 35) == 0xff0000,
              "the pixels are the page's: its background, the red box", "%06x %06x", pixel_at(0, 300, 280),
              pixel_at(0, 40, 35));
    save("offscreen-first", 0);
    next(300);
}

/* 2. A small change damages a small part. */
static uint64_t seq_before;
static int changed(void) { return last[0].sequence > seq_before && (d3d11_seen || pixel_at(0, 40, 35) == 0x0000ff); }
static void s_change(void) {
    seq_before = last[0].sequence;
    input_at = now_us(), first_change_us = 0;
    zebkit_view_evaluate(view, "document.getElementById('box').style.background = 'rgb(0, 0, 255)'", NULL, NULL);
    wait_for(changed, 5000, "a frame of the change");
}
static void s_check_damage(void) {
    zebkit_rect d = last[0].damage;
    int covers = d.x <= 20 && d.y <= 20 && d.x + d.width >= 60 && d.y + d.height >= 50;
    int small = d.width * d.height <= 400 * 300 / 10;
    check(covers && small, "the damage is the box, not the page", "%d,%d %dx%d", d.x, d.y, d.width, d.height);
    printf("     from the script to the frame: %.1f ms\n", (first_change_us - input_at) / 1000.0);
    input_at = 0;
    next(0);
}

/* 3. Idle: no frames. */
static void s_idle(void) {
    frames_at_start = frames[0];
    next(1500);
}
static void s_check_idle(void) {
    check(frames[0] == frames_at_start, "no frames while the page is idle", "%d in 1.5 s",
          frames[0] - frames_at_start);
    next(0);
}

/* 4. A click on the button changes the page. */
static void s_click(void) {
    click(160, 40);
    next(300);
}
static void s_check_click(void) { evaluate("document.body.dataset.clicks || '0'"); }
static void s_after_click(void) {
    check(strcmp(result, "\"1\"") == 0, "a click on the button reaches the page", "%s", result);
    check(strcmp(cursor, "pointer") == 0, "the cursor over the button is the page's", "%s", cursor);
    next(0);
}

/* 5. Typing into a field, with focus. */
static void s_type(void) {
    zebkit_view_set_focus(view, true);
    click(120, 115);
    key(0x41, "KeyA", "a", "a");
    key(0x42, "KeyB", "b", "b");
    key(0x43, "KeyC", "c", "c");
    key(0x08, "Backspace", "Backspace", NULL);
    key(0x44, "KeyD", "d", "d");
    next(400);
}
static void s_check_type(void) { evaluate("document.getElementById('field').value"); }
static void s_after_type(void) {
    check(strcmp(result, "\"abd\"") == 0, "keys typed into the focused field", "%s", result);
    check(carets > 0 && caret.x >= 20 && caret.x <= 220 && caret.y >= 100 && caret.y <= 130,
          "the caret, in the field", "%d calls, at %d,%d %dx%d", carets, caret.x, caret.y, caret.width,
          caret.height);
    next(0);
}

/* 6. An input method: a composition, then Chinese committed. */
static void s_ime(void) {
    zebkit_view_ime_set_composition(view, "ni", -1, -1);
    next(200);
}
static void s_check_composing(void) { evaluate("document.getElementById('field').value"); }
static void s_ime_commit(void) {
    check(strcmp(result, "\"abdni\"") == 0, "a composition is in the field", "%s", result);
    zebkit_view_ime_commit(view, "你好");
    zebkit_view_ime_set_composition(view, "x", -1, -1);
    zebkit_view_ime_cancel(view);
    next(300);
}
static void s_check_ime(void) { evaluate("document.getElementById('field').value"); }
static void s_after_ime(void) {
    check(strcmp(result, "\"abd你好\"") == 0, "Chinese committed in place of it, a cancelled one gone", "%s",
          result);
    save("offscreen-typed", 0);
    next(0);
}

/* 7. The wheel scrolls the page. */
static void s_wheel(void) {
    for (int i = 0; i < 9; ++i)
        zebkit_view_send_wheel(view, 200, 200, 0, 100, true, i ? ZEBKIT_WHEEL_PHASE_CHANGED : ZEBKIT_WHEEL_PHASE_BEGAN,
                               0);
    zebkit_view_send_wheel(view, 200, 200, 0, 0, true, ZEBKIT_WHEEL_PHASE_ENDED, 0);
    zebkit_view_send_wheel(view, 200, 200, 0, 53, false, ZEBKIT_WHEEL_PHASE_NONE, 0);
    next(800);
}
static void s_check_wheel(void) { evaluate("Math.round(scrollY)"); }
static void s_after_wheel(void) {
    int y = atoi(result);
    check(y >= 900, "the wheel scrolled the page, a trackpad's deltas and a notch", "scrollY %d", y);
    if (!pixels_unread())
        check(pixel_at(0, 200, 1025 - y) == 0x00ff00, "the frame shows it scrolled", "%06x", pixel_at(0, 200, 1025 - y));
    save("offscreen-scrolled", 0);
    zebkit_view_evaluate(view, "scrollTo(0, 0)", NULL, NULL);
    next(400);
}

/* 8. A <select>'s list: a popup layer, chosen from with the keyboard and the mouse. */
static int popup_with_frame(void) { return popup_shown && frames[1] > 0; }
static void s_select(void) {
    frames[1] = 0;
    click(100, 175);
    wait_for(popup_with_frame, 5000, "the <select>'s list shown, with frames of its own");
}
static void s_check_popup(void) {
    check(popup_rect.x >= 15 && popup_rect.x <= 30 && popup_rect.y >= 185 && popup_rect.height > 30,
          "the list is under the <select>", "%d,%d %dx%d", popup_rect.x, popup_rect.y, popup_rect.width,
          popup_rect.height);
    check(last[1].width == (int)(popup_rect.width * last[1].scale + 0.5) && last[1].bounds.y == popup_rect.y,
          "its frames are its size, at its place", "%dx%d at %d,%d", last[1].width, last[1].height,
          last[1].bounds.x, last[1].bounds.y);
    save("offscreen-select-list", 1);
    key(0x28, "ArrowDown", "ArrowDown", NULL);
    key(0x0D, "Enter", "Enter", NULL);
    next(400);
}
static void s_check_select(void) { evaluate("document.getElementById('pick').value"); }
static void s_select_mouse(void) {
    check(strcmp(result, "\"b\"") == 0 && !popup_shown, "chosen with the keyboard, and the list gone", "%s, %s",
          result, popup_shown ? "shown" : "gone");
    click(100, 175);
    wait_for(popup_with_frame, 5000, "the list again");
}
static void s_select_click(void) {
    /* The third item: the list's last third. */
    double x = popup_rect.x + popup_rect.width / 2.0;
    double y = popup_rect.y + popup_rect.height * 5.0 / 6.0;
    click(x, y);
    next(400);
}
static void s_after_select(void) {
    check(strcmp(result, "\"c\"") == 0, "chosen with the mouse", "%s", result);
    next(0);
}
static void s_check_select2(void) { evaluate("document.getElementById('pick').value"); }

/* 9. At most as many frames out as allowed; the newest when one comes back. */
static int animation_frames;
static void s_backpressure(void) {
    zebkit_view_set_max_frames_in_flight(view, 1);
    hold = 1, held_count = 0;
    frames_at_start = frames[0];
    zebkit_view_evaluate(view, "animate(600)", NULL, NULL);
    next(900);
}
static void s_check_backpressure(void) {
    check(frames[0] - frames_at_start == 1 && held_count == 1, "one frame out while the application holds it",
          "%d frames, %d held", frames[0] - frames_at_start, held_count);
    hold = 0;
    uint64_t seq = last[0].sequence;
    zebkit_frame_release(held[0]);
    held_count = 0;
    seq_before = seq;
    next(300);
}
static void s_after_release(void) {
    check(frames[0] - frames_at_start == 2 && last[0].sequence == seq_before + 1 &&
              (pixels_unread() || pixel_at(0, 40, 35) == 0xff0000),
          "released, the newest frame comes, the animation's end", "%d frames, sequence %llu, %06x",
          frames[0] - frames_at_start, (unsigned long long)last[0].sequence, pixel_at(0, 40, 35));
    zebkit_view_set_max_frames_in_flight(view, 2);
    next(0);
}

/* 10. The frame rate limit. */
static void s_rate(void) {
    zebkit_view_set_frame_rate(view, 10);
    frames_at_start = frames[0];
    zebkit_view_evaluate(view, "animate(1000).then((n) => n)", evaluated, NULL);
}
static void s_check_rate(void) {
    animation_frames = atoi(result);
    int n = frames[0] - frames_at_start;
    check(n >= 4 && n <= 13, "10 frames a second at most", "%d frames in 1 s, of %d the page drew", n,
          animation_frames);
    zebkit_view_set_frame_rate(view, 0);
    next(300);
}

/* 11. Resized, and at another scale. */
/* The page is wider and taller than 320x240, so Windows' classic scrollbars take
 * the last 15 pixels of each side: its background is looked for inside them.
 * A resized surface may arrive before the renderer has repainted at the
 * new scale. Wait for the finished page, rather than asserting on that
 * intermediate frame (the same race occurs with the previous engine). */
static int resized(void) {
    return last[0].width == 640 && last[0].height == 480 && last[0].scale == 2 &&
           (d3d11_seen || (pixel_at(0, 40, 35) == 0xff0000 && pixel_at(0, 21, 21) == 0xff0000 &&
           pixel_at(0, 290, 210) == 0x102030));
}
static void s_resize(void) {
    zebkit_view_set_size(view, 320, 240, 2);
    wait_for(resized, 5000, "a frame of the new size");
}
static void s_check_resize(void) {
    check(last[0].scale == 2, "at the new scale", "%g", last[0].scale);
    if (!pixels_unread())
        check(pixel_at(0, 40, 35) == 0xff0000 && pixel_at(0, 21, 21) == 0xff0000 && pixel_at(0, 290, 210) == 0x102030,
              "the page laid out again, drawn at twice the pixels", "%06x %06x", pixel_at(0, 40, 35),
              pixel_at(0, 290, 210));
    /* A new scale reaches the page as two changes, the display's scale and
     * then the window's size, and a busy machine runs the page's script
     * between them: the page sees the size it was given within a moment of
     * the first frame of it, not always by then. */
    evaluate("new Promise((done) => { const start = performance.now(); (function wait() {"
             " if ((innerWidth == 320 && innerHeight == 240) || performance.now() - start > 3000)"
             " done([innerWidth, innerHeight, devicePixelRatio]); else setTimeout(wait, 20); })(); })");
}
static void s_after_resize(void) {
    check(strcmp(result, "[320,240,2]") == 0, "the page sees its new size and scale", "%s", result);
    save("offscreen-scale-2", 0);
    printf("     frames %d, bytes a frame at 320x240 at 2: %lld; display time to ready %.2f ms on average, %.2f at most\n",
           frames[0], (long long)frame_bytes, latency_n ? latency_sum / 1000.0 / latency_n : 0,
           latency_max / 1000.0);
    next(0);
}

static void on_close(zebkit_view *v, void *data) {
    (void)v, (void)data;
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    const char *base = test_base();
    shots = getenv("ZEBKIT_TEST_SHOTS");
    char url[512];
    snprintf(url, sizeof(url), "%s/offscreen.html", base);
    view = zebkit_view_new_offscreen(url, 400, 300, 1);
    check(view != NULL, "zebkit_view_new_offscreen", NULL);
    if (!view) {
        zebkit_quit();
        return;
    }
    check(zebkit_view_is_offscreen(view), "zebkit_view_is_offscreen", NULL);
    zebkit_view_on_close(view, on_close, NULL);
    /* ZEBKIT_TEST_BUFFERS=shm: frames in shared memory on macOS too; =gpu: GPU buffers on Linux. */
    const char *buffers = getenv("ZEBKIT_TEST_BUFFERS");
    /* Another kind of buffer, not less of the test: said, not "narrowed". */
    if (buffers && *buffers)
        printf("note: ZEBKIT_TEST_BUFFERS=%s: frames in %s\n", buffers,
               strcmp(buffers, "gpu") == 0 ? "GPU buffers" : strcmp(buffers, "shm") == 0 ? "shared memory" : "the default buffers");
    if (buffers && strcmp(buffers, "shm") == 0)
        zebkit_view_set_frame_buffers(view, ZEBKIT_BUFFERS_SHARED_MEMORY);
    else if (buffers && strcmp(buffers, "gpu") == 0)
        zebkit_view_set_frame_buffers(view, ZEBKIT_BUFFERS_GPU);
    zebkit_view_on_frame(view, on_frame, NULL);
    zebkit_view_on_popup(view, on_popup, NULL);
    zebkit_view_on_cursor(view, on_cursor, NULL);
    zebkit_view_on_caret(view, on_caret, NULL);
    step_func all[] = {
        s_first_frame, s_check_first, s_change, s_check_damage, s_idle, s_check_idle,
        s_click, s_check_click, s_after_click, s_type, s_check_type, s_after_type,
        s_ime, s_check_composing, s_ime_commit, s_check_ime, s_after_ime,
        s_wheel, s_check_wheel, s_after_wheel,
        s_select, s_check_popup, s_check_select, s_select_mouse, s_select_click, s_check_select2, s_after_select,
        s_backpressure, s_check_backpressure, s_after_release,
        s_rate, s_check_rate, s_resize, s_check_resize, s_after_resize,
    };
    step_count = (int)(sizeof(all) / sizeof(all[0]));
    memcpy(steps, all, sizeof(all));
    next(0);
}

int main(int argc, char **argv) {
    test_base();
    int code = zebkit_main(argc, argv, ready, NULL);
    if (code)
        return code;
    return summary() || harness_checks < 20 ? 1 : 0;
}
