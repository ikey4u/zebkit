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
 *    "permissionBits": 4294967292, "userPermissionBits": ... (the /P bits,
 *                    with and without the owner's password),
 *    "form": "none" | "acro" | "xfa", "formDefaultAppearance": "/Helv 0 Tf
 *    0 g" (the form's /DA, which fields without their own take),
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
 * 270, "objectNumber": 12, "label": "iv", "mediaBox": [l, b, r, t], "cropBox", "bleedBox",
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

/*
 * Renders appearance @mode (0 normal, 1 rollover, 2 down: those
 * zebkit_pdf_annotations() lists under "appearances") of annotation
 * @annotation of the page, alone: an image of its rectangle at the
 * options' scale, turned clockwise by their rotation (not by the page's),
 * on their background, which here is transparent when @options is NULL.
 * The tile and the flags are not used. Fails with "no appearance" when
 * there is none to draw, as for a hidden annotation.
 */
ZEBKIT_API uint8_t *zebkit_pdf_render_annotation(zebkit_pdf *pdf, int page, int annotation, int mode,
                                                 const zebkit_pdf_render_options *options, int *width, int *height,
                                                 char **error);

/* ---- Text ------------------------------------------------------------ */

/* The page's text, in reading order as PDFium extracts it. */
ZEBKIT_API char *zebkit_pdf_text(zebkit_pdf *pdf, int page, char **error);

/*
 * The page's characters, as JSON: {"text": "...", "chars": [{"c": "H",
 * "box": [l, b, r, t], "loose": [l, b, r, t], "size": 18, "font":
 * "Helvetica", "angle": 0, "object": 0}, ...], "objects": [{"font":
 * "Helvetica-Bold", "family": "Helvetica", "weight": 700, "italic": false,
 * "monospaced": false, "embedded": false, "size": 12, "color": [r, g, b,
 * a]}, ...]}. Character i of "chars" is UTF-16 unit i of the page's text as
 * PDFium counts it, which the other text functions take as indexes;
 * characters PDFium makes up (spaces, line ends) have no box. "box" is
 * the glyph's own; "loose" the line's height at its advance, as a
 * selection covers it. "object" is the text object that draws the
 * character, an index of "objects", numbered as they first come.
 */
ZEBKIT_API char *zebkit_pdf_text_layout(zebkit_pdf *pdf, int page, char **error);

/*
 * The rectangles that cover @count characters from @start, a line's run
 * each, as a selection draws them: JSON [[l, b, r, t], ...].
 */
ZEBKIT_API char *zebkit_pdf_text_rects(zebkit_pdf *pdf, int page, int start, int count, char **error);

/*
 * The page's text as lines, as PDFium breaks it: JSON [{"text": "...",
 * "box": [l, b, r, t], "unmapped": 0}, ...], "box" covering the line's
 * glyphs, "unmapped" how many of its characters have no Unicode of their
 * font's own (PDFium's guess stands for them: text to OCR instead).
 */
ZEBKIT_API char *zebkit_pdf_text_lines(zebkit_pdf *pdf, int page, char **error);

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

/* ---- Images -------------------------------------------------------- */

/*
 * The images the page draws, inside forms too: JSON [{"width": 2480,
 * "height": 3508, "rect": [l, b, r, t], "jpeg": true}, ...], "width" and
 * "height" in pixels, "rect" where it is drawn on the page, "jpeg" whether
 * it is stored as a JPEG file. A page that is one large image is a scan.
 */
ZEBKIT_API char *zebkit_pdf_images(zebkit_pdf *pdf, int page, char **error);

/*
 * Image @image of the page, numbered as zebkit_pdf_images() lists them: a
 * JPEG file as the document stores it ("jpeg": true there, and *@jpeg set),
 * otherwise its pixels decoded, as PNG. NULL with *@error when there is no
 * such image or it cannot be decoded; zebkit_free() frees it.
 */
ZEBKIT_API uint8_t *zebkit_pdf_image_data(zebkit_pdf *pdf, int page, int image, size_t *size, bool *jpeg,
                                          char **error);

/* ---- Signatures ---------------------------------------------------- */

/*
 * The document's digital signatures, in its form's order, as JSON:
 *   [{"field": "Signatures.Approval", "subFilter": "adbe.pkcs7.detached",
 *     "reason": "...", "time": "D:20261021120000+02'00'",
 *     "byteRange": [0, 840, 960, 2400], "docMDP": 0, "size": 8192,
 *     "signer": {"name": "...", "issuer": "...", "serial": "0a1b...",
 *                "notBefore": "2026-01-01T00:00:00Z", "notAfter": "..."}}]
 * "docMDP" is the changes the signer allowed after it (1-3; 0 for none
 * said), "size" the contents' bytes; "signer" the certificate of the
 * signer among those the signature carries, when it can be read.
 * Each is checked against the document as it was opened: "intact" true
 * when the bytes it covers are those it signed ("digest" the algorithm)
 * and the signature is by the key of the signer's certificate, otherwise
 * false and "problem" why; "coversDocument" whether its byte range runs to
 * the end of the file, so that nothing was added after it. Whether the
 * certificate is one to trust is not checked.
 */
ZEBKIT_API char *zebkit_pdf_signatures(zebkit_pdf *pdf, char **error);

/* Signature @index's contents: the DER-encoded PKCS#7 or time stamp it holds. */
ZEBKIT_API uint8_t *zebkit_pdf_signature_contents(zebkit_pdf *pdf, int index, size_t *size, char **error);

/* ---- Navigation ------------------------------------------------------ */

/*
 * The outline (bookmarks), as JSON: [{"title": "...", "page": 4,
 * "x": 72, "y": 700, "zoom": 0, "url": "...", "open": true,
 * "children": [...]}, ...]; "page" for a place in the document, "url" for
 * a link out, either or neither.
 */
ZEBKIT_API char *zebkit_pdf_outline(zebkit_pdf *pdf, char **error);

/* Replaces the outline with an array in the format above; [] removes it.
 * Titles are required; page indices are zero based. Page and URL are mutually
 * exclusive; x/y/zoom need a page, with zoom >= 0. Unknown keys and invalid
 * values reject the whole tree before mutation. At most 64 child levels,
 * 100000 entries and 16 MiB of JSON. Save the document to persist changes.
 */
ZEBKIT_API bool zebkit_pdf_set_outline(zebkit_pdf *pdf, const char *json, char **error);

/*
 * The page's links: the link annotations, and addresses written in its
 * text ("detected": true): JSON [{"rect": [l, b, r, t], "page": 2 | "url":
 * "...", ...}, ...].
 */
ZEBKIT_API char *zebkit_pdf_links(zebkit_pdf *pdf, int page, char **error);

/* ---- Annotations ----------------------------------------------------- */

/*
 * The page's annotations, in their order on the page, as JSON:
 *   [{"index": 0, "object": 12, "type": "highlight", "rect": [l, b, r, t],
 *     "color": [r, g, b], "interiorColor": [r, g, b],
 *     "opacity": 0.5, "border": 1, "flags": 4,
 *     "contents": "...", "author": "...", "subject": "...", "name": "...",
 *     "modified": "D:2026...", "created": "D:2026...",
 *     "quads": [[x1, y1, x2, y2, x3, y3, x4, y4], ...],
 *     "ink": [[[x, y], ...], ...], "vertices": [[x, y], ...],
 *     "line": [x1, y1, x2, y2], "url": "...", "page": 3,
 *     "popup": 1, "inReplyTo": 0, "field": "name",
 *     "appearances": ["normal", "rollover", "down"],
 *     "entries": {"Subtype": "/Highlight", "BS": {"W": 1, "S": "/D"}, ...}},
 *    ...]
 * with the keys an annotation has. "entries" is the annotation's
 * dictionary but its appearance (/AP) and page (/P), as plain values: a
 * name is "/Name", a string that starts with "/" has another before it,
 * indirect objects are followed, but annotations and pages (and an object
 * already being followed), which are {"ref": their object number}.
 * "object" is the annotation's own object number, absent when it is direct.
 * Types: text, link, freeText, line,
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
 * above ("type" and "rect" at least; "index", "popup" and "appearances"
 * are not set this way), and returns its index. "entries" sets entries
 * of its dictionary as they are written there (null removes one); /AP,
 * /P and /Subtype are not set so. Text markup (highlight, underline,
 * squiggly, strikeOut) takes "quads", ink takes "ink", polygon and
 * polyline "vertices", line "line", link "url" or "page". The types made
 * this way are those of the list above up to ink, and popup and redact;
 * their appearance is generated from what is set.
 */
ZEBKIT_API int zebkit_pdf_annotation_add(zebkit_pdf *pdf, int page, const char *json, char **error);

/*
 * Changes annotation @index of the page: the keys @json has are set, null
 * removes one. Its appearance is generated again, for the types whose
 * appearance PDFium makes, when something it shows changes (not for
 * "contents", "author", "subject", "name", the dates or the flags alone);
 * "regenerate": true or false in @json says otherwise.
 */
ZEBKIT_API bool zebkit_pdf_annotation_update(zebkit_pdf *pdf, int page, int index, const char *json, char **error);

ZEBKIT_API bool zebkit_pdf_annotation_remove(zebkit_pdf *pdf, int page, int index, char **error);

/*
 * Makes an image the appearance of stamp annotation @index: @rgba, @width
 * by @height pixels, R, G, B, A in rows top to bottom, drawn over the
 * annotation's rectangle. What an image stamp ("Approved", a signature
 * drawn by hand) is.
 */
ZEBKIT_API bool zebkit_pdf_annotation_set_image(zebkit_pdf *pdf, int page, int index, const uint8_t *rgba, int width,
                                                int height, char **error);

/*
 * Makes the page's annotations and form fields part of its content, as
 * they are shown (@for_print false) or printed: they are no longer
 * annotations. Links, which draw nothing, go too.
 */
ZEBKIT_API bool zebkit_pdf_flatten(zebkit_pdf *pdf, int page, bool for_print, char **error);

/* ---- Redaction -------------------------------------------------------- */

/*
 * Takes what is under @areas, a JSON array of [l, b, r, t] (NULL for the
 * page's redaction marks, their quadrilaterals or else their rectangles),
 * out of the page for good: characters of text one by one, the rest kept
 * where it was; the pixels of images, blacked out; paths, shadings and
 * forms that reach into an area, whole; annotations that reach into one.
 * Draws a box of @fill (0xRRGGBBAA; 0 for none) over each area, and removes
 * the marks. Returns the number of areas, or -1.
 */
ZEBKIT_API int zebkit_pdf_redact(zebkit_pdf *pdf, int page, const char *areas_json, uint32_t fill, char **error);

/* ---- Forms ------------------------------------------------------------ */

/*
 * The document's form fields, as JSON:
 *   [{"name": "address.city", "type": "text" | "checkbox" | "radio" |
 *     "combobox" | "listbox" | "pushbutton" | "signature", "value": "...",
 *     "readOnly": false, "required": true, "flags": 4096,
 *     "options": [{"label": "...", "selected": true}, ...],
 *     "widgets": [{"page": 0, "annotation": 3, "rect": [l, b, r, t],
 *                  "checked": true, "exportValue": "Yes"}, ...]}, ...]
 * with the keys a field has; a field is one entry however many widgets it
 * has on however many pages.
 */
ZEBKIT_API char *zebkit_pdf_form_fields(zebkit_pdf *pdf, char **error);

/*
 * Sets field @name (its full name) to @value: the text of a text field or
 * combo box, an option's value of a list box, the export value of the
 * check box or radio button to check ("Off" for none). The fields'
 * appearances follow.
 */
ZEBKIT_API bool zebkit_pdf_form_set(zebkit_pdf *pdf, const char *name, const char *value, char **error);

/* Every field back to its default value. */
ZEBKIT_API bool zebkit_pdf_form_reset(zebkit_pdf *pdf, char **error);

/* ---- Pages, changed ------------------------------------------------- */

/* Sets the page's rotation: 0, 90, 180 or 270. */
ZEBKIT_API bool zebkit_pdf_page_rotate(zebkit_pdf *pdf, int page, int rotation, char **error);

/* Deletes @count pages, at the indexes @pages holds. */
ZEBKIT_API bool zebkit_pdf_pages_delete(zebkit_pdf *pdf, const int *pages, int count, char **error);

/* Moves @count pages, in the order given, to start at index @to of the document they leave. */
ZEBKIT_API bool zebkit_pdf_pages_move(zebkit_pdf *pdf, const int *pages, int count, int to, char **error);

/* Inserts a blank page @width by @height points at index @at. */
ZEBKIT_API bool zebkit_pdf_page_insert(zebkit_pdf *pdf, int at, double width, double height, char **error);

/*
 * Copies @count pages of @from (all when @pages is NULL) into @pdf at index
 * @at: merging documents, and taking pages from another.
 */
ZEBKIT_API bool zebkit_pdf_pages_import(zebkit_pdf *pdf, zebkit_pdf *from, const int *pages, int count, int at,
                                        char **error);

/* A new, empty document, for building one from others' pages. */
ZEBKIT_API zebkit_pdf *zebkit_pdf_new(char **error);

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
