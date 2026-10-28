/*
 * zebkit: views drawn off-screen, for an application that shows a page in
 * its own window, or in another process's.
 *
 * An off-screen view has no window. The engine draws its page and hands
 * every frame to the application as a shared surface, without encoding:
 *
 *   macOS   an IOSurface, drawn by the GPU; IOSurfaceCreateMachPort() (or
 *           zebkit_frame_iosurface_mach_port()) gives another process a
 *           send right to it.
 *   Linux   BGRA pixels in shared memory, a file descriptor the
 *           application maps or passes to another process; or a dmabuf
 *           (planes, strides, a modifier), where the GPU makes one.
 *
 * Each frame says what changed since the one before (damage rectangles),
 * its sequence number, its size in pixels and its scale. Frames come only
 * when the page changes; an idle page sends none. The application gives
 * every frame back (zebkit_frame_release()); while as many frames as
 * zebkit_view_set_max_frames_in_flight() allows are out, the engine keeps
 * only the newest of the rest, with the damage of those it skipped.
 *
 * The application sends the view its input: the mouse, the wheel, keys,
 * an input method's composition, focus. It hears the cursor the page asks
 * for and where the caret is, for the input method's window. A popup
 * widget (a <select>'s list, a date picker) is drawn off-screen too: its
 * frames come as a layer of their own, at the place in the view where it
 * is to be shown, and the input the application sends over that place
 * goes to it.
 *
 * Off-screen views are for a process without windows: one started with
 * --headless, or on Linux without a display, such as an application's
 * helper process that renders pages for it (examples/offscreen-host).
 * Elsewhere zebkit_view_new_offscreen() returns NULL.
 *
 * Threads, strings and ownership are <zebkit/zebkit.h>'s: everything is
 * for the engine's thread, and so are the callbacks, except
 * zebkit_frame_release(), which any thread may call.
 */

#ifndef ZEBKIT_OFFSCREEN_H
#define ZEBKIT_OFFSCREEN_H

#include "zebkit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Off-screen views ------------------------------------------------ */

/*
 * A view without a window, loading @url (NULL for about:blank), @width by
 * @height CSS pixels at @scale device pixels per CSS pixel (0 for 1); its
 * frames are @width * @scale by @height * @scale pixels. Frames start
 * with zebkit_view_on_frame(). Otherwise it is a view as
 * zebkit_view_new() makes one, closed the same way. NULL when the process
 * has windows (see above). The off-screen views of a process share one
 * scale, the last one given: the headless screen's. On macOS it is whole
 * (1, 2, 3), as a Mac's screens are.
 */
ZEBKIT_API zebkit_view *zebkit_view_new_offscreen(const char *url, int width, int height, float scale);
/* zebkit_view_new_offscreen() in @context (<zebkit/zebkit.h>). */
ZEBKIT_API zebkit_view *zebkit_view_new_offscreen_in(zebkit_context *context, const char *url, int width, int height,
                                                     float scale);

/* Whether @view is off-screen. */
ZEBKIT_API bool zebkit_view_is_offscreen(zebkit_view *view);

/*
 * The view's size in CSS pixels and its scale (0 for unchanged): the page
 * is laid out again and the next frame has the new size, all of it
 * damaged.
 */
ZEBKIT_API void zebkit_view_set_size(zebkit_view *view, int width, int height, float scale);

/* ---- Frames ---------------------------------------------------------- */

typedef struct {
    int32_t x, y, width, height;
} zebkit_rect;

typedef enum {
    /* Pixels in shared memory: @pixels in this process, @fd for another. */
    ZEBKIT_FRAME_SHARED_MEMORY = 1,
    /* An IOSurface (macOS): @iosurface. */
    ZEBKIT_FRAME_IOSURFACE = 2,
    /* A dmabuf (Linux): @plane_count planes of @fds, @strides, @offsets, with @modifier. */
    ZEBKIT_FRAME_DMABUF = 3,
} zebkit_frame_kind;

typedef enum {
    /* The view's page. */
    ZEBKIT_LAYER_VIEW = 0,
    /* A popup widget over it: a <select>'s list, a date or colour picker. */
    ZEBKIT_LAYER_POPUP = 1,
} zebkit_frame_layer;

/* Pixel formats: 32 bits a pixel, alpha premultiplied, in memory order. */
typedef enum {
    ZEBKIT_FORMAT_BGRA8 = 1, /* DRM_FORMAT_ARGB8888, kCVPixelFormatType_32BGRA */
    ZEBKIT_FORMAT_RGBA8 = 2, /* DRM_FORMAT_ABGR8888 */
} zebkit_pixel_format;

typedef struct zebkit_frame {
    /* 1, 2, 3, ... for each layer of a view (a popup's start again at 1).
     * While the application holds as many frames as it may, the engine
     * keeps the newest of the rest and drops the others: the frame it
     * sends next carries their damage too. */
    uint64_t sequence;
    zebkit_frame_layer layer;
    /* Where the frame goes in the view, in CSS pixels: (0, 0, width,
     * height) for the view; for a popup, its place, which may reach past
     * the view's edges. */
    zebkit_rect bounds;
    /* The frame's size in pixels, and device pixels per CSS pixel. */
    int32_t width, height;
    float scale;
    /* What changed since the previous frame of this layer, in the frame's
     * pixels; the whole frame when its size changed or it is the first.
     * The frame always holds the whole picture: the damage is for an
     * application that copies or redraws only what changed. */
    const zebkit_rect *damage;
    size_t damage_count;
    /* When the compositor means the frame to be shown (its display time,
     * which may be a few milliseconds ahead), and when it was ready, in
     * microseconds of the system's monotonic clock (CLOCK_MONOTONIC;
     * mach_absolute_time() on macOS). */
    int64_t drawn_us;
    int64_t ready_us;

    zebkit_frame_kind kind;
    zebkit_pixel_format format;
    /* The same for every frame in the same buffer while the buffer lives:
     * an application or another process can keep a mapping or a texture
     * per buffer. */
    uint64_t buffer_id;

    /* ZEBKIT_FRAME_SHARED_MEMORY: the first pixel's row at @pixels (in this
     * process), @stride bytes a row. @fd is the engine's, open until the
     * frame is released: mmap(@size bytes at 0, PROT_READ, MAP_SHARED),
     * the pixels at @offset, or send it to another process (SCM_RIGHTS);
     * dup() it to keep it. */
    const uint8_t *pixels;
    size_t stride;
    int fd;
    size_t size;
    size_t offset;

    /* ZEBKIT_FRAME_IOSURFACE: an IOSurfaceRef, the engine's until the frame
     * is released; CFRetain() it to keep it. A CVPixelBuffer is
     * CVPixelBufferCreateWithIOSurface(), a Metal texture
     * -[MTLDevice newTextureWithDescriptor:iosurface:plane:]. */
    void *iosurface;

    /* ZEBKIT_FRAME_DMABUF: the planes, the engine's fds until the frame is
     * released; @fourcc is the DRM format. */
    int plane_count;
    int fds[4];
    uint32_t strides[4];
    uint32_t offsets[4];
    uint64_t modifier;
    uint32_t fourcc;
} zebkit_frame;

/*
 * Called with each frame of @view; @frame is valid, and its buffer the
 * application's to read, until zebkit_frame_release(@frame), which is
 * for every frame once, also after its view has closed. NULL stops the
 * frames.
 */
typedef void (*zebkit_frame_func)(zebkit_view *view, const zebkit_frame *frame, void *data);
ZEBKIT_API void zebkit_view_on_frame(zebkit_view *view, zebkit_frame_func func, void *data);

/*
 * Gives a frame back: its buffer may be drawn into again. Any thread,
 * once for each frame.
 */
ZEBKIT_API void zebkit_frame_release(const zebkit_frame *frame);

/* At most @fps frames a second (0 for no limit but the display's, 60). */
ZEBKIT_API void zebkit_view_set_frame_rate(zebkit_view *view, double fps);

/* How many frames the application may hold at once, 1 to 8; 2 by default. */
ZEBKIT_API void zebkit_view_set_max_frames_in_flight(zebkit_view *view, int count);

typedef enum {
    /* The fastest the platform has: IOSurfaces on macOS, shared memory on
     * Linux. */
    ZEBKIT_BUFFERS_AUTO = 0,
    /* Pixels in shared memory, everywhere. */
    ZEBKIT_BUFFERS_SHARED_MEMORY = 1,
    /* GPU buffers: IOSurfaces, dmabufs on Linux where the GPU makes them
     * (shared memory where it does not). */
    ZEBKIT_BUFFERS_GPU = 2,
} zebkit_frame_buffers;

/* What frames are made in; from the next frame. */
ZEBKIT_API void zebkit_view_set_frame_buffers(zebkit_view *view, zebkit_frame_buffers buffers);

/* A frame now, all of it damaged, even if nothing changed: for an
 * application that lost what it had drawn. */
ZEBKIT_API void zebkit_view_request_frame(zebkit_view *view);

/*
 * A new send right to the frame's IOSurface (macOS), for another process:
 * IOSurfaceLookupFromMachPort() there. The caller's, to send in a mach
 * message or mach_port_deallocate(). 0 for another kind of frame or
 * elsewhere.
 */
ZEBKIT_API uint32_t zebkit_frame_iosurface_mach_port(const zebkit_frame *frame);

/* ---- Input ----------------------------------------------------------- */

/* Modifier keys and buttons held, for every event. */
typedef enum {
    ZEBKIT_MODIFIER_SHIFT = 1 << 0,
    ZEBKIT_MODIFIER_CONTROL = 1 << 1,
    ZEBKIT_MODIFIER_ALT = 1 << 2,    /* Option on macOS */
    ZEBKIT_MODIFIER_META = 1 << 3,   /* Command on macOS, the Super key elsewhere */
    ZEBKIT_MODIFIER_CAPS_LOCK = 1 << 4,
    ZEBKIT_MODIFIER_NUM_LOCK = 1 << 5,
    ZEBKIT_MODIFIER_LEFT_BUTTON = 1 << 6,
    ZEBKIT_MODIFIER_MIDDLE_BUTTON = 1 << 7,
    ZEBKIT_MODIFIER_RIGHT_BUTTON = 1 << 8,
    ZEBKIT_MODIFIER_KEYPAD = 1 << 9,      /* a key of the numeric keypad */
    ZEBKIT_MODIFIER_AUTO_REPEAT = 1 << 10, /* a key held down, repeating */
} zebkit_modifiers;

typedef enum {
    ZEBKIT_MOUSE_MOVE = 0,
    ZEBKIT_MOUSE_DOWN = 1,
    ZEBKIT_MOUSE_UP = 2,
    /* The pointer left the view. */
    ZEBKIT_MOUSE_LEAVE = 3,
} zebkit_mouse_type;

typedef enum {
    ZEBKIT_BUTTON_NONE = 0,
    ZEBKIT_BUTTON_LEFT = 1,
    ZEBKIT_BUTTON_MIDDLE = 2,
    ZEBKIT_BUTTON_RIGHT = 3,
    ZEBKIT_BUTTON_BACK = 4,
    ZEBKIT_BUTTON_FORWARD = 5,
} zebkit_mouse_button;

/*
 * A mouse event at (@x, @y) in the view, CSS pixels from its top left.
 * @click_count is 1 for a click, 2 for a double click, ... (for DOWN and
 * UP; 0 otherwise). Over a popup widget it goes to the widget. A move
 * with a button held has that button in @modifiers
 * (ZEBKIT_MODIFIER_LEFT_BUTTON, ...), or as @button: it drags (a slider's
 * thumb, a selection, a drag and drop, <zebkit/page.h>).
 */
ZEBKIT_API void zebkit_view_send_mouse(zebkit_view *view, zebkit_mouse_type type, float x, float y,
                                       zebkit_mouse_button button, int click_count, unsigned modifiers);

typedef enum {
    ZEBKIT_WHEEL_PHASE_NONE = 0,  /* a mouse's wheel */
    ZEBKIT_WHEEL_PHASE_BEGAN = 1, /* a trackpad's gesture: */
    ZEBKIT_WHEEL_PHASE_CHANGED = 2,
    ZEBKIT_WHEEL_PHASE_ENDED = 3,
    ZEBKIT_WHEEL_PHASE_MOMENTUM = 4, /* the scroll going on after the fingers left */
} zebkit_wheel_phase;

/*
 * A scroll at (@x, @y) by (@delta_x, @delta_y) CSS pixels, positive to the
 * right and down, as the DOM's WheelEvent has it. @precise for a
 * trackpad's deltas, scrolled as they come; otherwise a wheel's notches,
 * scrolled smoothly.
 */
ZEBKIT_API void zebkit_view_send_wheel(zebkit_view *view, float x, float y, float delta_x, float delta_y,
                                       bool precise, zebkit_wheel_phase phase, unsigned modifiers);

typedef enum {
    /* A key pressed: what keydown sees, without the character. */
    ZEBKIT_KEY_DOWN = 0,
    ZEBKIT_KEY_UP = 1,
    /* The character a key typed, after its DOWN: what keypress and the
     * text input see. */
    ZEBKIT_KEY_CHAR = 2,
} zebkit_key_type;

typedef struct {
    zebkit_key_type type;
    unsigned modifiers;
    /* The key's Windows virtual key code, which is the DOM's keyCode
     * (0x41 for A, 0x0D for Enter); 0 for none. */
    int key_code;
    /* The system's own code for the key, or 0. */
    int native_key_code;
    /* The DOM's code ("KeyA", "Enter") and key ("a", "Enter"); NULL to
     * leave them to the engine. */
    const char *code;
    const char *key;
    /* For CHAR: the text typed (UTF-8, one character as a rule), and what
     * it would be without the modifiers. NULL otherwise. */
    const char *text;
    const char *unmodified_text;
} zebkit_key_event;

/*
 * A key, to the focused element (or the open popup widget). For a key
 * that types, DOWN, then CHAR with its text, then UP, as the system gives
 * them.
 */
ZEBKIT_API void zebkit_view_send_key(zebkit_view *view, const zebkit_key_event *event);

/*
 * An input method's composition: @text (UTF-8) in the focused field,
 * underlined, not yet typed, with the selection in it from
 * @selection_start to @selection_end (UTF-16 code units; both -1 for its
 * end). Called again as it changes.
 */
ZEBKIT_API void zebkit_view_ime_set_composition(zebkit_view *view, const char *text, int selection_start,
                                                int selection_end);

/* Types @text (UTF-8) in place of the composition, or at the caret when there is none. */
ZEBKIT_API void zebkit_view_ime_commit(zebkit_view *view, const char *text);

/* Takes the composition away, typing nothing. */
ZEBKIT_API void zebkit_view_ime_cancel(zebkit_view *view);

/*
 * The caret, or the end of the selection, in the view in CSS pixels, as it
 * moves, for an input method's window; NULL when there is none.
 */
typedef void (*zebkit_caret_func)(zebkit_view *view, const zebkit_rect *caret, void *data);
ZEBKIT_API void zebkit_view_on_caret(zebkit_view *view, zebkit_caret_func func, void *data);

/*
 * Whether the view has the keyboard: the focused element, the blinking
 * caret, :focus and focus events, as when its window is active. A view
 * starts without it.
 */
ZEBKIT_API void zebkit_view_set_focus(zebkit_view *view, bool focused);

/*
 * Enables Passkey authentication UI. Normal views enable it
 * by default; off-screen views do not. An opted-in off-screen view may show a
 * temporary authentication window only while the host marks it focused and
 * its page has a recent user activation. Keep set_focus() in sync with the
 * host's active tab/window. UI is closed with the request. Returns false for a NULL view. No credentials are synthesized.
 */
ZEBKIT_API bool zebkit_view_set_authentication_ui(zebkit_view *view, bool enabled);

/*
 * The cursor the page asks for, as CSS names it: "default", "pointer",
 * "text", "wait", "crosshair", "move", "not-allowed", "grab", "ew-resize",
 * ..., "none", or "custom" with @image: @width by @height BGRA pixels
 * (premultiplied), @hot_x, @hot_y its hot spot, at @scale; @image is NULL
 * for the others.
 */
typedef struct {
    const uint8_t *pixels;
    int32_t width, height;
    int32_t hot_x, hot_y;
    float scale;
} zebkit_cursor_image;
typedef void (*zebkit_cursor_func)(zebkit_view *view, const char *cursor, const zebkit_cursor_image *image,
                                   void *data);
ZEBKIT_API void zebkit_view_on_cursor(zebkit_view *view, zebkit_cursor_func func, void *data);

/*
 * A popup widget shown over the view (@shown, at @bounds in the view in
 * CSS pixels) or gone (@shown false, @bounds NULL). Its frames come as
 * ZEBKIT_LAYER_POPUP; the application draws them at @bounds over the
 * view's until it is gone.
 */
typedef void (*zebkit_popup_func)(zebkit_view *view, bool shown, const zebkit_rect *bounds, void *data);
ZEBKIT_API void zebkit_view_on_popup(zebkit_view *view, zebkit_popup_func func, void *data);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_OFFSCREEN_H */
