/*
 * 2D drawing on the engine's Skia: a canvas over a bitmap of its own or over
 * the caller's pixels, with a matrix, paths, fill and stroke, colours,
 * gradients, images, text in a font given as bytes, and PNG out.
 *
 * It works as the web's canvas does: a current path whose points are put
 * through the matrix as they are added, a stack of state (matrix, colours,
 * line style, clip, font) kept by save and restore. The origin is the top
 * left, y down, units are pixels.
 *
 * The rules of <zebkit/services.h> hold: what is returned is the caller's,
 * to free with zebkit_free(); a function that fails returns NULL or false
 * and, when @error is not NULL, sets it to a message to free with
 * zebkit_free(). None of it needs zebkit_main(). A canvas is used by one
 * thread at a time.
 *
 * A canvas handed out by a PDF hook (<zebkit/pdf.h>) that draws an appearance
 * has the same functions and writes PDF operators into the appearance stream
 * rather than pixels: its units are points, and the pixels and PNG functions
 * fail on it. Drawing through Skia and embedding the outcome would make every
 * appearance a bitmap or need a whole PDF backend per appearance, so that
 * canvas is its own backend: paths, text as outlines, images as XObjects,
 * gradients as shadings. docs/API.md says so too.
 */

#ifndef ZEBKIT_CANVAS_H
#define ZEBKIT_CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zebkit/zebkit.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zebkit_canvas zebkit_canvas;
typedef struct zebkit_canvas_font zebkit_canvas_font;

/* ---- Canvases -------------------------------------------------------- */

/* A canvas over a transparent bitmap of its own, at most 32768 on a side and
 * 1 << 28 pixels. */
ZEBKIT_API zebkit_canvas *zebkit_canvas_new(int width, int height, char **error);

typedef enum {
    /*
     * The caller's pixels are not premultiplied: they are premultiplied in
     * place now and put back by zebkit_canvas_close(). Without it they are
     * premultiplied already, which opaque pixels always are.
     */
    ZEBKIT_CANVAS_UNPREMULTIPLIED = 1 << 0,
} zebkit_canvas_flags;

/*
 * A canvas over the caller's R, G, B, A pixels, @stride bytes from row to row
 * (0 for @width * 4), which must stay until zebkit_canvas_close().
 */
ZEBKIT_API zebkit_canvas *zebkit_canvas_wrap(uint8_t *rgba, int width, int height, size_t stride, unsigned flags,
                                             char **error);

ZEBKIT_API void zebkit_canvas_close(zebkit_canvas *canvas);

ZEBKIT_API int zebkit_canvas_width(zebkit_canvas *canvas);
ZEBKIT_API int zebkit_canvas_height(zebkit_canvas *canvas);

/* A copy of the pixels, R, G, B, A not premultiplied, width * height * 4 bytes. */
ZEBKIT_API uint8_t *zebkit_canvas_pixels(zebkit_canvas *canvas, char **error);

/* The pixels as a PNG file. Sets *@size. */
ZEBKIT_API uint8_t *zebkit_canvas_png(zebkit_canvas *canvas, size_t *size, char **error);

/* ---- State ----------------------------------------------------------- */

ZEBKIT_API void zebkit_canvas_save(zebkit_canvas *canvas);
/* Does nothing when nothing is saved. */
ZEBKIT_API void zebkit_canvas_restore(zebkit_canvas *canvas);

ZEBKIT_API void zebkit_canvas_translate(zebkit_canvas *canvas, double x, double y);
ZEBKIT_API void zebkit_canvas_scale(zebkit_canvas *canvas, double x, double y);
ZEBKIT_API void zebkit_canvas_rotate(zebkit_canvas *canvas, double degrees);
/* Multiplies the matrix by [a c e; b d f; 0 0 1], as the web's transform() and PDF's cm. */
ZEBKIT_API void zebkit_canvas_transform(zebkit_canvas *canvas, double a, double b, double c, double d, double e,
                                        double f);
/* Replaces the matrix: what it maps from, to the canvas's first space. */
ZEBKIT_API void zebkit_canvas_set_matrix(zebkit_canvas *canvas, double a, double b, double c, double d, double e,
                                         double f);

/* 0xRRGGBBAA. */
ZEBKIT_API void zebkit_canvas_set_fill_color(zebkit_canvas *canvas, uint32_t rgba);
ZEBKIT_API void zebkit_canvas_set_stroke_color(zebkit_canvas *canvas, uint32_t rgba);
/* 0 to 1, multiplied into everything drawn. */
ZEBKIT_API void zebkit_canvas_set_alpha(zebkit_canvas *canvas, double alpha);
/* PDF's names: "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten",
 * "ColorDodge", "ColorBurn", "HardLight", "SoftLight", "Difference",
 * "Exclusion", "Hue", "Saturation", "Color", "Luminosity". False for another. */
ZEBKIT_API bool zebkit_canvas_set_blend(zebkit_canvas *canvas, const char *mode);
ZEBKIT_API void zebkit_canvas_set_line_width(zebkit_canvas *canvas, double width);
typedef enum { ZEBKIT_CANVAS_CAP_BUTT, ZEBKIT_CANVAS_CAP_ROUND, ZEBKIT_CANVAS_CAP_SQUARE } zebkit_canvas_cap;
ZEBKIT_API void zebkit_canvas_set_line_cap(zebkit_canvas *canvas, zebkit_canvas_cap cap);
typedef enum { ZEBKIT_CANVAS_JOIN_MITER, ZEBKIT_CANVAS_JOIN_ROUND, ZEBKIT_CANVAS_JOIN_BEVEL } zebkit_canvas_join;
ZEBKIT_API void zebkit_canvas_set_line_join(zebkit_canvas *canvas, zebkit_canvas_join join);
ZEBKIT_API void zebkit_canvas_set_miter_limit(zebkit_canvas *canvas, double limit);
/* @count lengths, on and off in turn; 0 for a solid line. */
ZEBKIT_API void zebkit_canvas_set_dash(zebkit_canvas *canvas, const double *lengths, int count, double phase);

/*
 * Fills from now on with a gradient, in the space of the matrix at this call,
 * clamped beyond its ends: @count (2 or more, at most 256) stops, @offsets from
 * 0 to 1, going up, and their @colors as 0xRRGGBBAA. A fill colour set later
 * ends it. False for stops that are not so.
 */
ZEBKIT_API bool zebkit_canvas_set_linear_gradient(zebkit_canvas *canvas, double x0, double y0, double x1, double y1,
                                                  const double *offsets, const uint32_t *colors, int count);
ZEBKIT_API bool zebkit_canvas_set_radial_gradient(zebkit_canvas *canvas, double x, double y, double radius,
                                                  const double *offsets, const uint32_t *colors, int count);

/* ---- Paths ----------------------------------------------------------- */

ZEBKIT_API void zebkit_canvas_begin_path(zebkit_canvas *canvas);
ZEBKIT_API void zebkit_canvas_move_to(zebkit_canvas *canvas, double x, double y);
ZEBKIT_API void zebkit_canvas_line_to(zebkit_canvas *canvas, double x, double y);
ZEBKIT_API void zebkit_canvas_quad_to(zebkit_canvas *canvas, double cx, double cy, double x, double y);
ZEBKIT_API void zebkit_canvas_curve_to(zebkit_canvas *canvas, double c1x, double c1y, double c2x, double c2y, double x,
                                       double y);
/* From @start to @end degrees, clockwise on the screen, or the other way. */
ZEBKIT_API void zebkit_canvas_arc(zebkit_canvas *canvas, double x, double y, double radius, double start, double end,
                                  bool counterclockwise);
ZEBKIT_API void zebkit_canvas_rect(zebkit_canvas *canvas, double x, double y, double width, double height);
ZEBKIT_API void zebkit_canvas_close_path(zebkit_canvas *canvas);

ZEBKIT_API void zebkit_canvas_fill(zebkit_canvas *canvas, bool even_odd);
ZEBKIT_API void zebkit_canvas_stroke(zebkit_canvas *canvas);
/* What is drawn from now on stays inside the current path too, until restore. */
ZEBKIT_API void zebkit_canvas_clip(zebkit_canvas *canvas, bool even_odd);

/* Paints every pixel inside the clip with @rgba, replacing what is there. On
 * a canvas that writes PDF, what is there stays beneath. */
ZEBKIT_API void zebkit_canvas_clear(zebkit_canvas *canvas, uint32_t rgba);

/* ---- Images ---------------------------------------------------------- */

/*
 * Draws R, G, B, A pixels, not premultiplied, @width * 4 bytes a row, over the
 * rectangle (@x, @y) @dest_width by @dest_height. False for sizes that are not
 * positive or over 1 << 14. zebkit_image_decode() (<zebkit/services.h>) makes
 * such pixels from a file.
 */
ZEBKIT_API bool zebkit_canvas_draw_image(zebkit_canvas *canvas, const uint8_t *rgba, int width, int height, double x,
                                         double y, double dest_width, double dest_height);

/* ---- Text ------------------------------------------------------------ */

/* A font from its bytes, which are copied; TrueType or OpenType outlines. */
ZEBKIT_API zebkit_canvas_font *zebkit_canvas_font_new(const void *data, size_t size, char **error);
/* Canvases keep what they use: it may be closed once none is set to draw it anew. */
ZEBKIT_API void zebkit_canvas_font_close(zebkit_canvas_font *font);

ZEBKIT_API void zebkit_canvas_set_font(zebkit_canvas *canvas, zebkit_canvas_font *font, double size);

/*
 * Shapes @text (UTF-8, as zebkit_text_shape()) and fills its outlines with the
 * baseline's left end at (@x, @y). False, with *@error, without a font or for
 * text that is not UTF-8. Glyphs without outlines (colour bitmaps) draw nothing.
 */
ZEBKIT_API bool zebkit_canvas_fill_text(zebkit_canvas *canvas, const char *text, double x, double y, char **error);
/* The same, adding the outlines to the current path, to stroke or clip. */
ZEBKIT_API bool zebkit_canvas_text_path(zebkit_canvas *canvas, const char *text, double x, double y, char **error);
/* The advance of @text in the canvas's font, or -1. */
ZEBKIT_API double zebkit_canvas_measure_text(zebkit_canvas *canvas, const char *text);

/* ---- A list of operations -------------------------------------------- */

/*
 * Runs @ops, a JSON array of operations, each an array with its name first:
 * ["save"], ["restore"], ["translate", x, y], ["scale", x, y], ["rotate", deg],
 * ["transform", a, b, c, d, e, f], ["setMatrix", a, b, c, d, e, f],
 * ["fillColor", rgba], ["strokeColor", rgba], ["alpha", a], ["blend", name],
 * ["lineWidth", w], ["lineCap", 0-2], ["lineJoin", 0-2], ["miterLimit", m],
 * ["dash", [lengths], phase],
 * ["linearGradient", x0, y0, x1, y1, [offsets], [rgba]],
 * ["radialGradient", x, y, r, [offsets], [rgba]],
 * ["beginPath"], ["moveTo", x, y], ["lineTo", x, y], ["quadTo", cx, cy, x, y],
 * ["curveTo", c1x, c1y, c2x, c2y, x, y], ["arc", x, y, r, start, end, ccw],
 * ["rect", x, y, w, h], ["closePath"], ["fill", evenOdd], ["stroke"],
 * ["clip", evenOdd], ["clear", rgba],
 * ["image", base64 RGBA, width, height, x, y, w, h],
 * ["font", id, base64 file], ["setFont", id, size], ["fillText", text, x, y],
 * ["textPath", text, x, y]
 * rgba being 0xRRGGBBAA as a number. This is what bindings without direct
 * calls, and zebkit.canvas in JavaScript, send. At most 1000000 operations and
 * 256 MiB. Stops at the first that is wrong, with its index in *@error; those
 * before it stay drawn.
 */
ZEBKIT_API bool zebkit_canvas_run(zebkit_canvas *canvas, const char *ops, char **error);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_CANVAS_H */
