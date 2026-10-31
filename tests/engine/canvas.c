/* <zebkit/canvas.h> and <zebkit/text.h> without the engine running: pixels
 * drawn and read back. Takes a TrueType font's path (Ahem: every glyph a box). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/canvas.h>
#include <zebkit/services.h>
#include <zebkit/text.h>

static int failures, checks;

static void check(int ok, const char *what) {
    checks++;
    if (!ok)
        failures++;
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static const uint8_t *pixel(const uint8_t *rgba, int width, int x, int y) {
    return rgba + (size_t)y * width * 4 + (size_t)x * 4;
}

static int is(const uint8_t *p, int r, int g, int b, int a) {
    const int tolerance = 3;
    return abs(p[0] - r) <= tolerance && abs(p[1] - g) <= tolerance && abs(p[2] - b) <= tolerance &&
           abs(p[3] - a) <= tolerance;
}

static char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *data = malloc((size_t)length + 1);
    *size = fread(data, 1, (size_t)length, file);
    fclose(file);
    return data;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: canvas <font.ttf>\n");
        return 2;
    }
    size_t font_size = 0;
    char *font_bytes = read_file(argv[1], &font_size);
    check(font_bytes != NULL, "the font is there");

    char *error = NULL;
    /* Refusals. */
    check(!zebkit_canvas_new(0, 10, &error) && error, "a canvas of no size fails");
    zebkit_free(error);
    error = NULL;
    check(!zebkit_canvas_new(40000, 40000, &error) && error, "a huge canvas fails");
    zebkit_free(error);
    error = NULL;
    uint8_t tiny[16];
    check(!zebkit_canvas_wrap(tiny, 2, 2, 4, 0, &error) && error, "too small a stride fails");
    zebkit_free(error);

    zebkit_canvas *c = zebkit_canvas_new(100, 100, NULL);
    check(c && zebkit_canvas_width(c) == 100, "a canvas");

    /* Fill, a matrix, clip, stroke. */
    zebkit_canvas_set_fill_color(c, 0xff0000ff);
    zebkit_canvas_rect(c, 10, 10, 20, 20);
    zebkit_canvas_fill(c, false);
    zebkit_canvas_save(c);
    zebkit_canvas_translate(c, 50, 0);
    zebkit_canvas_scale(c, 2, 2);
    zebkit_canvas_set_fill_color(c, 0x00ff00ff);
    zebkit_canvas_begin_path(c);
    zebkit_canvas_rect(c, 0, 5, 10, 10);
    zebkit_canvas_fill(c, false);
    zebkit_canvas_restore(c);
    zebkit_canvas_begin_path(c);
    zebkit_canvas_set_stroke_color(c, 0x0000ffff);
    zebkit_canvas_set_line_width(c, 4);
    zebkit_canvas_move_to(c, 10, 50);
    zebkit_canvas_line_to(c, 90, 50);
    zebkit_canvas_stroke(c);
    /* A dash leaves gaps. */
    zebkit_canvas_begin_path(c);
    const double dash[] = {10, 10};
    zebkit_canvas_set_dash(c, dash, 2, 0);
    zebkit_canvas_move_to(c, 10, 60);
    zebkit_canvas_line_to(c, 90, 60);
    zebkit_canvas_stroke(c);
    zebkit_canvas_set_dash(c, NULL, 0, 0);
    /* A clip keeps what is drawn inside it. */
    zebkit_canvas_save(c);
    zebkit_canvas_begin_path(c);
    zebkit_canvas_rect(c, 10, 70, 5, 5);
    zebkit_canvas_clip(c, false);
    zebkit_canvas_set_fill_color(c, 0x000000ff);
    zebkit_canvas_begin_path(c);
    zebkit_canvas_rect(c, 0, 65, 30, 30);
    zebkit_canvas_fill(c, false);
    zebkit_canvas_restore(c);
    /* A gradient, a circle, alpha. */
    const double offsets[] = {0, 1};
    const uint32_t colors[] = {0xff0000ff, 0x0000ffff};
    check(zebkit_canvas_set_linear_gradient(c, 50, 0, 90, 0, offsets, colors, 2), "a linear gradient");
    const uint32_t bad_colors[] = {1};
    check(!zebkit_canvas_set_linear_gradient(c, 0, 0, 1, 1, offsets, bad_colors, 1), "a gradient of one stop fails");
    const double bad_offsets[] = {1, 0};
    check(!zebkit_canvas_set_linear_gradient(c, 0, 0, 1, 1, bad_offsets, colors, 2), "stops going down fail");
    zebkit_canvas_begin_path(c);
    zebkit_canvas_rect(c, 50, 70, 40, 10);
    zebkit_canvas_fill(c, false);
    zebkit_canvas_set_fill_color(c, 0x00ff00ff);
    zebkit_canvas_set_alpha(c, 0.5);
    zebkit_canvas_begin_path(c);
    zebkit_canvas_arc(c, 20, 90, 8, 0, 360, false);
    zebkit_canvas_fill(c, false);
    zebkit_canvas_set_alpha(c, 1);

    uint8_t *rgba = zebkit_canvas_pixels(c, NULL);
    check(is(pixel(rgba, 100, 15, 15), 255, 0, 0, 255), "a fill");
    check(is(pixel(rgba, 100, 5, 5), 0, 0, 0, 0), "elsewhere transparent");
    check(is(pixel(rgba, 100, 65, 15), 0, 255, 0, 255) && is(pixel(rgba, 100, 69, 29), 0, 255, 0, 255) &&
              is(pixel(rgba, 100, 71, 31), 0, 0, 0, 0),
          "a matrix: translated and scaled");
    check(is(pixel(rgba, 100, 50, 50), 0, 0, 255, 255) && is(pixel(rgba, 100, 50, 54), 0, 0, 0, 0), "a stroke's width");
    check(is(pixel(rgba, 100, 15, 60), 0, 0, 255, 255) && is(pixel(rgba, 100, 25, 60), 0, 0, 0, 0), "a dash");
    check(is(pixel(rgba, 100, 12, 72), 0, 0, 0, 255) && is(pixel(rgba, 100, 20, 72), 0, 0, 0, 0), "a clip");
    check(pixel(rgba, 100, 52, 75)[0] > 200 && pixel(rgba, 100, 88, 75)[2] > 200, "a gradient from red to blue");
    {
        const uint8_t *p = pixel(rgba, 100, 20, 90);
        check(p[3] > 120 && p[3] < 135 && p[1] > 245, "alpha, read back not premultiplied");
    }
    zebkit_free(rgba);

    /* PNG out, which decodes to the same. */
    size_t png_size = 0;
    uint8_t *png = zebkit_canvas_png(c, &png_size, NULL);
    check(png && png_size > 8 && !memcmp(png, "\x89PNG", 4), "a PNG");
    int w = 0, h = 0;
    uint8_t *decoded = zebkit_image_decode(png, png_size, &w, &h, NULL);
    check(decoded && w == 100 && h == 100 && is(pixel(decoded, 100, 15, 15), 255, 0, 0, 255), "the PNG decodes");
    zebkit_free(decoded);

    /* An image, a blend. */
    const uint8_t image[] = {255, 0, 0, 255, 0, 0, 255, 255, 0, 255, 0, 255, 255, 255, 255, 0};
    zebkit_canvas *d = zebkit_canvas_new(40, 40, NULL);
    check(zebkit_canvas_draw_image(d, image, 2, 2, 0, 0, 40, 40), "an image");
    check(!zebkit_canvas_draw_image(d, image, 0, 2, 0, 0, 40, 40), "an image of no size fails");
    rgba = zebkit_canvas_pixels(d, NULL);
    check(pixel(rgba, 40, 3, 3)[0] > 220 && pixel(rgba, 40, 36, 3)[2] > 220 && pixel(rgba, 40, 3, 36)[1] > 220,
          "the image's top row at the top");
    zebkit_free(rgba);
    zebkit_canvas_close(d);
    d = zebkit_canvas_new(10, 10, NULL);
    zebkit_canvas_clear(d, 0xffff00ff);
    check(zebkit_canvas_set_blend(d, "Multiply") && !zebkit_canvas_set_blend(d, "Nonsense"), "blend modes by name");
    zebkit_canvas_set_fill_color(d, 0x00ffffff);
    zebkit_canvas_rect(d, 0, 0, 10, 10);
    zebkit_canvas_fill(d, false);
    rgba = zebkit_canvas_pixels(d, NULL);
    check(is(pixel(rgba, 10, 5, 5), 0, 255, 0, 255), "Multiply: yellow by cyan is green");
    zebkit_free(rgba);
    zebkit_canvas_close(d);

    /* The caller's pixels, not premultiplied. */
    uint8_t mine[4 * 4 * 4];
    memset(mine, 0, sizeof(mine));
    for (int i = 0; i < 16; ++i) {
        mine[i * 4] = 200;
        mine[i * 4 + 3] = 128;
    }
    zebkit_canvas *over = zebkit_canvas_wrap(mine, 4, 4, 0, ZEBKIT_CANVAS_UNPREMULTIPLIED, NULL);
    check(over != NULL, "a canvas over the caller's pixels");
    zebkit_canvas_set_fill_color(over, 0x0000ffff);
    zebkit_canvas_rect(over, 0, 0, 2, 4);
    zebkit_canvas_fill(over, false);
    zebkit_canvas_close(over);
    check(is(pixel(mine, 4, 0, 0), 0, 0, 255, 255) && is(pixel(mine, 4, 3, 0), 200, 0, 0, 128),
          "written back, not premultiplied");

    /* Text, in a font from its bytes. */
    char *bad_font = NULL;
    check(!zebkit_canvas_font_new("not a font", 10, &bad_font) && bad_font, "what is not a font fails");
    zebkit_free(bad_font);
    zebkit_canvas_font *font = zebkit_canvas_font_new(font_bytes, font_size, NULL);
    check(font != NULL, "a font from bytes");
    zebkit_canvas *t = zebkit_canvas_new(100, 40, NULL);
    char *no_font = NULL;
    check(!zebkit_canvas_fill_text(t, "x", 0, 0, &no_font) && no_font, "text without a font fails");
    zebkit_free(no_font);
    zebkit_canvas_set_font(t, font, 20);
    zebkit_canvas_font_close(font); /* the canvas keeps what it uses */
    zebkit_canvas_set_fill_color(t, 0x000000ff);
    check(zebkit_canvas_fill_text(t, "XX", 10, 30, NULL), "text");
    check(zebkit_canvas_measure_text(t, "XX") == 40, "its advance: Ahem's is one em");
    char *bad_text = NULL;
    check(!zebkit_canvas_fill_text(t, "\xff\xfe", 0, 0, &bad_text) && bad_text, "what is not UTF-8 fails");
    zebkit_free(bad_text);
    rgba = zebkit_canvas_pixels(t, NULL);
    check(is(pixel(rgba, 100, 15, 20), 0, 0, 0, 255) && is(pixel(rgba, 100, 45, 20), 0, 0, 0, 255) &&
              is(pixel(rgba, 100, 55, 20), 0, 0, 0, 0) && is(pixel(rgba, 100, 15, 5), 0, 0, 0, 0),
          "the glyphs where they should be");
    zebkit_free(rgba);
    zebkit_canvas_close(t);

    /* A list of operations; and failures in one. */
    zebkit_canvas *r = zebkit_canvas_new(20, 20, NULL);
    check(zebkit_canvas_run(r, "[[\"fillColor\", 4278190335], [\"rect\", 0, 0, 10, 10], [\"fill\", false]]", NULL),
          "operations");
    rgba = zebkit_canvas_pixels(r, NULL);
    check(is(pixel(rgba, 20, 5, 5), 255, 0, 0, 255), "drawn by them");
    zebkit_free(rgba);
    const char *wrong[] = {"not json", "{}", "[1]", "[[\"nothing\"]]", "[[\"rect\", 1]]", "[[\"rect\", \"a\", 1, 1, 1]]",
                           "[[\"fillColor\", -1]]", "[[\"blend\", \"No\"]]", "[[\"fillText\", \"x\", 0, 0]]",
                           "[[\"image\", \"!!!\", 1, 1, 0, 0, 1, 1]]", "[[\"font\", 1, \"AAAA\"]]", "[[\"setFont\", 9, 1]]",
                           "[[\"image\", \"AAAA\", 100000, 100000, 0, 0, 1, 1]]"};
    for (size_t i = 0; i < sizeof(wrong) / sizeof(wrong[0]); ++i) {
        char *why = NULL;
        check(!zebkit_canvas_run(r, wrong[i], &why) && why, wrong[i]);
        zebkit_free(why);
    }
    zebkit_canvas_close(r);
    zebkit_canvas_close(c);
    zebkit_free(png);

    /* Shaping. */
    char *shaped = zebkit_text_shape(font_bytes, font_size, 20, "AB", "en", "ltr", NULL);
    check(shaped && strstr(shaped, "\"direction\":\"ltr\"") && strstr(shaped, "\"advance\":[40.0,0.0]") &&
              strstr(shaped, "\"cluster\":1"),
          shaped ? shaped : "shaping");
    zebkit_free(shaped);
    shaped = zebkit_text_shape(font_bytes, font_size, 20, "AB", NULL, "rtl", NULL);
    check(shaped && strstr(shaped, "\"direction\":\"rtl\"") &&
              strstr(shaped, "\"cluster\":1") < strstr(shaped, "\"cluster\":0"),
          "right to left: clusters go down");
    zebkit_free(shaped);
    /* Malformed. */
    struct {
        const void *font;
        size_t size;
        double em;
        const char *text, *direction, *what;
    } bad[] = {
        {"nope", 4, 12, "a", NULL, "not a font"},
        {NULL, 0, 12, "a", NULL, "no font"},
        {font_bytes, font_size, -1, "a", NULL, "a negative size"},
        {font_bytes, font_size, 1e9, "a", NULL, "a huge size"},
        {font_bytes, font_size, 12, NULL, NULL, "no text"},
        {font_bytes, font_size, 12, "\xc0", NULL, "not UTF-8"},
        {font_bytes, font_size, 12, "a", "up", "no such direction"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char *why = NULL;
        char *answer = zebkit_text_shape(bad[i].font, bad[i].size, bad[i].em, bad[i].text, NULL, bad[i].direction, &why);
        check(!answer && why, bad[i].what);
        zebkit_free(answer);
        zebkit_free(why);
    }
    free(font_bytes);
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
