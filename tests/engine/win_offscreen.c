/* Off-screen frames on Windows: a view with no window under --headless, each
 * frame's pixels read at once and through its file mapping. The page is pure
 * red on the left and pure blue on the right.
 *
 * Exit 0 when every check passes. */

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/offscreen.h>
#include <zebkit/zebkit.h>

static const char page[] =
    "data:text/html,<!doctype html><title>offscreen</title><style>"
    "html,body{margin:0;height:100%25;background:%23ff0000}"
    "div{position:fixed;left:50%25;top:0;right:0;bottom:0;background:%230000ff}"
    "</style><div></div>";

static int failures, frames;
static zebkit_view *view;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fflush(stdout);
    if (!ok)
        ++failures;
}

static uint32_t at(const uint8_t *pixels, size_t stride, int x, int y) {
    uint32_t value;
    memcpy(&value, pixels + (size_t)y * stride + (size_t)x * 4, 4);
    return value & 0xffffff;
}

static void finish(void *data) {
    (void)data;
    printf("%d frames, %d failures\n", frames, failures);
    fflush(stdout);
    zebkit_view_close(view);
    zebkit_quit();
}

static void frame(zebkit_view *v, const zebkit_frame *f, void *data) {
    (void)v;
    (void)data;
    ++frames;
    printf("frame %llu %dx%d scale %.1f kind %d format %d stride %zu handle %p fd %d damage %zu\n",
           (unsigned long long)f->sequence, f->width, f->height, f->scale, (int)f->kind, (int)f->format, f->stride,
           f->handle, f->fd, f->damage_count);
    fflush(stdout);
    if (f->pixels && at(f->pixels, f->stride, f->width / 4, f->height / 2) == 0xff0000 && failures == 0 && frames < 1000) {
        check(1, "the frame's own pixels: red on the left");
        check(at(f->pixels, f->stride, f->width * 3 / 4, f->height / 2) == 0x0000ff, "the frame's own pixels: blue on the right");
        check(f->handle != NULL, "a file mapping's handle");
        check(f->fd == -1, "no file descriptor");
        if (f->handle) {
            const uint8_t *mapped = MapViewOfFile((HANDLE)f->handle, FILE_MAP_READ, 0, 0, f->size);
            check(mapped != NULL, "MapViewOfFile() of the handle");
            if (mapped) {
                check(at(mapped + f->offset, f->stride, f->width / 4, f->height / 2) == 0xff0000, "through the mapping: red");
                check(at(mapped + f->offset, f->stride, f->width * 3 / 4, f->height / 2) == 0x0000ff, "through the mapping: blue");
                UnmapViewOfFile(mapped);
            }
        }
        frames = 1000;
        zebkit_post_task(finish, NULL);
    }
    zebkit_frame_release(f);
}

static void timeout(void *data) {
    (void)data;
    check(0, "a frame with the page's picture in time");
    finish(NULL);
}

static void ready(void *data) {
    (void)data;
    check(zebkit_is_headless(), "headless");
    view = zebkit_view_new_offscreen(page, 800, 600, 1);
    check(view != NULL, "zebkit_view_new_offscreen()");
    if (!view) {
        zebkit_quit();
        return;
    }
    zebkit_view_on_frame(view, frame, NULL);
    zebkit_post_delayed_task(timeout, NULL, 40000);
}

int main(int argc, char **argv) {
    const int code = zebkit_main(argc, argv, ready, NULL);
    printf("zebkit_main returned %d, %d failures\n", code, failures);
    return code ? code : (failures != 0);
}
