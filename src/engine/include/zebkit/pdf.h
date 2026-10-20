/*
 * zebkit's PDF API: Chromium's PDFium inside the engine, as documents an
 * application opens, reads, renders, searches and saves.
 *
 * The rules of <zebkit/services.h> hold: buffers and strings returned are
 * the caller's, to free with zebkit_free(); a function that fails returns
 * NULL (or false, or -1) and, when @error is not NULL, sets it to a message
 * to free with zebkit_free(). Structured answers are JSON, UTF-8.
 *
 * None of it needs zebkit_main(). Any thread may call it; PDFium is not
 * thread-safe, so the calls are made one at a time inside. A document is
 * used by one thread at a time.
 *
 * Coordinates are PDF page space: points (1/72 inch), the origin at the
 * bottom left of the page's media box, y up, before the page's rotation.
 * A rectangle is [left, bottom, right, top]. Pages are counted from 0.
 *
 * PDFium itself, with its own API, is <zebkit/vendor/pdfium.h>.
 */

#ifndef ZEBKIT_PDF_H
#define ZEBKIT_PDF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zebkit/zebkit.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zebkit_pdf zebkit_pdf;

/* ---- Documents ------------------------------------------------------- */

/*
 * Opens a PDF from @size bytes at @data, which are copied, with @password
 * (NULL for none). Fails with "password" when one is needed or wrong.
 */
ZEBKIT_API zebkit_pdf *zebkit_pdf_open(const void *data, size_t size, const char *password, char **error);

/* The same from a file, which is read as it is needed and must stay. */
ZEBKIT_API zebkit_pdf *zebkit_pdf_open_file(const char *path, const char *password, char **error);

ZEBKIT_API void zebkit_pdf_close(zebkit_pdf *pdf);

typedef enum {
    ZEBKIT_PDF_SAVE_INCREMENTAL = 1 << 0, /* append the changes; keeps signatures valid */
    ZEBKIT_PDF_SAVE_NO_SECURITY = 1 << 1, /* write it without its encryption */
} zebkit_pdf_save_flags;

/* The document as a file, with the changes made to it. Sets *@size. */
ZEBKIT_API uint8_t *zebkit_pdf_save(zebkit_pdf *pdf, unsigned flags, size_t *size, char **error);

/*
 * What the document is, as JSON:
 *   {"pages": 12, "version": 17, "encrypted": false, "securityRevision": -1,
 *    "permissions": {"print", "modify", "copy", "annotate", "fillForms",
 *                    "extract", "assemble", "printHighQuality"} (booleans),
 *    "form": "none" | "acro" | "xfa",
 *    "tagged": false, "language": "en-US",
 *    "metadata": {"title", "author", "subject", "keywords", "creator",
 *                 "producer", "creationDate", "modDate"} (those it has)}
 */
ZEBKIT_API char *zebkit_pdf_info(zebkit_pdf *pdf);

/*
 * Sets the document information: @json is an object with the keys of
 * "metadata" above; a string sets the entry, null removes it.
 */
ZEBKIT_API bool zebkit_pdf_set_metadata(zebkit_pdf *pdf, const char *json, char **error);

/* ---- Pages ----------------------------------------------------------- */

ZEBKIT_API int zebkit_pdf_page_count(zebkit_pdf *pdf);

/*
 * A page, as JSON: {"width": 612, "height": 792, "rotation": 0 | 90 | 180 |
 * 270, "label": "iv", "mediaBox": [l, b, r, t], "cropBox", "bleedBox",
 * "trimBox", "artBox" (those it has)}. Width and height are the page as it
 * is shown: its crop box, turned by its rotation, as zebkit_pdf_render()
 * draws it at scale 1.
 */
ZEBKIT_API char *zebkit_pdf_page(zebkit_pdf *pdf, int page, char **error);

typedef enum {
    ZEBKIT_PDF_RENDER_ANNOTATIONS = 1 << 0, /* annotations' appearances */
    ZEBKIT_PDF_RENDER_FORMS = 1 << 1,       /* form fields, as filled in */
    ZEBKIT_PDF_RENDER_LCD_TEXT = 1 << 2,    /* subpixel text, for an opaque background */
    ZEBKIT_PDF_RENDER_GRAYSCALE = 1 << 3,
    ZEBKIT_PDF_RENDER_PRINTING = 1 << 4,    /* as for printing: print-only annotations */
    ZEBKIT_PDF_RENDER_NO_SMOOTHING = 1 << 5,
} zebkit_pdf_render_flags;

typedef struct {
    float scale;          /* pixels per point; 0 is 1 */
    int rotation;         /* 0, 90, 180 or 270, added to the page's own */
    unsigned flags;       /* zebkit_pdf_render_flags */
    uint32_t background;  /* 0xRRGGBBAA; 0 is transparent */
    /*
     * A tile of the rendered page, in its pixels: from (@x, @y), @width by
     * @height. 0 for @width or @height is the whole page.
     */
    int x, y, width, height;
} zebkit_pdf_render_options;

/*
 * Renders a page to 8-bit RGBA, not premultiplied, rows top to bottom,
 * *@width * *@height * 4 bytes. @options NULL is the whole page at scale
 * 1 with annotations, on white.
 */
ZEBKIT_API uint8_t *zebkit_pdf_render(zebkit_pdf *pdf, int page, const zebkit_pdf_render_options *options,
                                      int *width, int *height, char **error);

/* ---- Text ------------------------------------------------------------ */

/* The page's text, in reading order as PDFium extracts it. */
ZEBKIT_API char *zebkit_pdf_text(zebkit_pdf *pdf, int page, char **error);

/*
 * The page's characters, as JSON: {"text": "...", "chars": [{"c": "H",
 * "box": [l, b, r, t], "size": 18, "font": "Helvetica", "angle": 0}, ...]}.
 * Character i of "chars" is UTF-16 unit i of the page's text as PDFium
 * counts it, which the other text functions take as indexes; characters
 * PDFium makes up (spaces, line ends) have no box.
 */
ZEBKIT_API char *zebkit_pdf_text_layout(zebkit_pdf *pdf, int page, char **error);

/*
 * The rectangles that cover @count characters from @start, a line's run
 * each, as a selection draws them: JSON [[l, b, r, t], ...].
 */
ZEBKIT_API char *zebkit_pdf_text_rects(zebkit_pdf *pdf, int page, int start, int count, char **error);

/* The index of the character at (@x, @y), within @tolerance points; -1 for none. */
ZEBKIT_API int zebkit_pdf_char_at(zebkit_pdf *pdf, int page, double x, double y, double tolerance);

typedef enum {
    ZEBKIT_PDF_SEARCH_CASE = 1 << 0,       /* match case */
    ZEBKIT_PDF_SEARCH_WHOLE_WORD = 1 << 1,
} zebkit_pdf_search_flags;

/*
 * Every match of @query in the document, from page @first on, at most
 * @limit (0 for all): JSON [{"page": 3, "start": 120, "count": 5,
 * "rects": [[l, b, r, t], ...], "context": "...text around it..."}, ...].
 */
ZEBKIT_API char *zebkit_pdf_search(zebkit_pdf *pdf, const char *query, unsigned flags, int first, int limit,
                                   char **error);

/* ---- Navigation ------------------------------------------------------ */

/*
 * The outline (bookmarks), as JSON: [{"title": "...", "page": 4,
 * "x": 72, "y": 700, "zoom": 0, "url": "...", "open": true,
 * "children": [...]}, ...]; "page" for a place in the document, "url" for
 * a link out, either or neither.
 */
ZEBKIT_API char *zebkit_pdf_outline(zebkit_pdf *pdf, char **error);

/*
 * The page's links: the link annotations, and addresses written in its
 * text ("detected": true): JSON [{"rect": [l, b, r, t], "page": 2 | "url":
 * "...", ...}, ...].
 */
ZEBKIT_API char *zebkit_pdf_links(zebkit_pdf *pdf, int page, char **error);

/* ---- Annotations ----------------------------------------------------- */

/*
 * The page's annotations, in their order on the page, as JSON:
 *   [{"index": 0, "type": "highlight", "rect": [l, b, r, t],
 *     "color": [r, g, b], "interiorColor": [r, g, b],
 *     "opacity": 0.5, "border": 1, "flags": 4,
 *     "contents": "...", "author": "...", "subject": "...", "name": "...",
 *     "modified": "D:2026...", "created": "D:2026...",
 *     "quads": [[x1, y1, x2, y2, x3, y3, x4, y4], ...],
 *     "ink": [[[x, y], ...], ...], "vertices": [[x, y], ...],
 *     "line": [x1, y1, x2, y2], "url": "...", "page": 3,
 *     "popup": 1, "inReplyTo": 0, "field": "name"}, ...]
 * with the keys an annotation has. Types: text, link, freeText, line,
 * square, circle, polygon, polyline, highlight, underline, squiggly,
 * strikeOut, stamp, caret, ink, popup, fileAttachment, sound, movie,
 * widget, screen, printerMark, trapNet, watermark, threeD, richMedia,
 * xfaWidget, redact, unknown. Colours are 0-255, opacity 0-1; flags are
 * the PDF's (print 4, hidden 2, readOnly 64, locked 128, ...). Dates are
 * PDF dates ("D:20261020120000Z"); "modified" is set to now by a change
 * that does not set it.
 */
ZEBKIT_API char *zebkit_pdf_annotations(zebkit_pdf *pdf, int page, char **error);

/*
 * Adds an annotation to the page from @json, an object with the keys
 * above ("type" and "rect" at least; "index" and "popup" are not set this
 * way), and returns its index. Text markup (highlight, underline,
 * squiggly, strikeOut) takes "quads", ink takes "ink", polygon and
 * polyline "vertices", line "line", link "url" or "page". The types made
 * this way are those of the list above up to ink, and popup and redact;
 * their appearance is generated from what is set.
 */
ZEBKIT_API int zebkit_pdf_annotation_add(zebkit_pdf *pdf, int page, const char *json, char **error);

/*
 * Changes annotation @index of the page: the keys @json has are set, null
 * removes one. Its appearance is generated again, for the types whose
 * appearance PDFium makes.
 */
ZEBKIT_API bool zebkit_pdf_annotation_update(zebkit_pdf *pdf, int page, int index, const char *json, char **error);

ZEBKIT_API bool zebkit_pdf_annotation_remove(zebkit_pdf *pdf, int page, int index, char **error);

/*
 * Makes the page's annotations and form fields part of its content, as
 * they are shown (@for_print false) or printed: they are no longer
 * annotations. Links, which draw nothing, go too.
 */
ZEBKIT_API bool zebkit_pdf_flatten(zebkit_pdf *pdf, int page, bool for_print, char **error);

/* ---- Attachments ----------------------------------------------------- */

/* The files attached to the document: JSON [{"name": "data.csv", "size": 1234}, ...]. */
ZEBKIT_API char *zebkit_pdf_attachments(zebkit_pdf *pdf, char **error);

/* Attachment @index's bytes. Sets *@size. */
ZEBKIT_API uint8_t *zebkit_pdf_attachment(zebkit_pdf *pdf, int index, size_t *size, char **error);

/* Attaches @size bytes at @data as @name. */
ZEBKIT_API bool zebkit_pdf_attach(zebkit_pdf *pdf, const char *name, const void *data, size_t size, char **error);

ZEBKIT_API bool zebkit_pdf_detach(zebkit_pdf *pdf, int index, char **error);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_PDF_H */
