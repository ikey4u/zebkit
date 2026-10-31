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

#include <zebkit/canvas.h>
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
 *
 * What the appearance keys of zebkit_pdf_annotation_add() set is read back
 * under the same names when the annotation has it: "lineEndings", "leader",
 * "caption", "borderStyle", "borderEffect", "blend", "rotation", "font",
 * "fontSize", "textColor", "borderColor", "align", "richText", "defaultStyle",
 * "callout", "calloutEnding", "intent", "icon", "stamp", "stampText",
 * "caretSymbol", "replyType", "state", "stateModel", "measure"; and "replies",
 * the indexes on this page of the annotations that reply to it. An annotation
 * of a kind an application registered has its name as "type", and its "data";
 * another of a subtype not listed is "unknown" with its "subtype".
 */
ZEBKIT_API char *zebkit_pdf_annotations(zebkit_pdf *pdf, int page, char **error);

/*
 * Adds an annotation to the page from @json, an object with the keys
 * above ("type" and "rect" at least; "index" and "appearances" are not set
 * this way), and returns its index. "entries" sets entries of its
 * dictionary as they are written there (null removes one); /AP, /P and
 * /Subtype are not set so. Text markup (highlight, underline, squiggly,
 * strikeOut) takes "quads", ink takes "ink", polygon and polyline
 * "vertices", line "line", link "url" or "page". The types made this way
 * are those of the list above up to ink, and popup, fileAttachment and
 * redact; their appearance is generated from what is set. Wrong types, values
 * outside the ranges below and unknown keys fail, with nothing added.
 *
 * Appearance, for what draws a shape:
 *   "borderStyle": {"style": "solid" | "dashed" | "beveled" | "inset" |
 *                   "underline", "width": 2, "dash": [3, 2]}  (/BS)
 *   "borderEffect": {"style": "none" | "cloudy", "intensity": 0-2}  (/BE), on
 *       squares, circles, polygons and free text; the shape stands inside the
 *       rectangle by the bumps' height (/RD).
 *   "opacity": 0-1; "blend": PDF's name of a blend mode (/BM, as in PDF 2.0).
 *   "rotation": degrees counterclockwise about the middle of "rect" (/Rotate).
 *       "rect" is the unturned box: what is read back is the rectangle around
 *       the turned one. Text markup and ink are not turned.
 * Lines and polylines:
 *   "lineEndings": [start, end], each "none", "openArrow", "closedArrow",
 *       "rOpenArrow", "rClosedArrow", "circle", "square", "diamond", "butt" or
 *       "slash" (/LE), filled with "interiorColor".
 * Lines:
 *   "leader": {"length": 10, "extension": 2, "offset": 1} (/LL /LLE /LLO)
 *   "caption": {"show": true, "position": "inline" | "top", "offset": [x, y]}
 *       (/Cap /CP /CO), whose text is "contents".
 *   A line's "rect" may be left out, and any shape with points' is replaced
 *   by what covers them, endings and caption included.
 * Free text:
 *   "font": "Helvetica" and the other standard fonts, or a family given by
 *       zebkit_pdf_font_add() or zebkit_pdf_hook_font(); "fontSize"; "textColor";
 *       "borderColor"  (/DA); "align": "left" | "center" | "right" (/Q);
 *       "richText": XHTML (/RC); "defaultStyle": CSS (/DS); "color": the
 *       background; "callout": [x1, y1, x2, y2 (, x3, y3)] from its far end to
 *       the box (/CL), "calloutEnding": an ending, at its far end (/LE).
 *       "rect" is the box of the text; what is read back also covers the callout.
 * The rest made:
 *   type "stamp": "stamp": "Approved", "Experimental", "NotApproved", "AsIs",
 *       "Expired", "NotForPublicRelease", "Confidential", "Final", "Sold",
 *       "Departmental", "ForComment", "TopSecret", "Draft" or
 *       "ForPublicRelease" (/Name); or "stampText": any text, drawn the same.
 *   type "text": "icon": "Comment", "Key", "Note", "Help", "NewParagraph",
 *       "Paragraph" or "Insert" (/Name).
 *   type "fileAttachment": "icon": "Graph", "PushPin", "Paperclip", "Tag",
 *       "GraphPushPin" or "PaperclipTag"; its file by
 *       zebkit_pdf_annotation_attach_file().
 *   type "caret": "caretSymbol": "none" | "paragraph" (/Sy).
 * Discussion:
 *   "inReplyTo": the index on this page of the annotation answered (/IRT);
 *       "replyType": "reply" | "group" (/RT); "state": "marked", "unmarked",
 *       "accepted", "rejected", "cancelled", "completed" or "none" (/State),
 *       "stateModel": "marking" | "review" (/StateModel). A state is said by
 *       a reply of type "text" with them, flags 2|16, as the reference has it.
 *       A reply that would make a cycle fails.
 *   "popup": {"rect": [l, b, r, t], "open": false}: makes its popup; null
 *       removes it. A number is what reading gives, and sets nothing.
 * Measurement, on lines, polylines, polygons, squares and circles:
 *   "measure": {"unit": "mm", "scale": 0.3528, "precision": 2, "label":
 *       "1 pt = 0.3528 mm"}: units per point (/Measure, in a rectilinear
 *       viewport's /X /D /A). "intent" takes what /IT does: "LineDimension",
 *       "PolyLineDimension", "PolygonDimension", set from the type if not given.
 *       "contents" becomes the value with its unit, and the caption; reading
 *       gives "measure" with "value" and "text" too.
 * Viewers:
 *   "textRange": [start, count]: on text markup, its "quads" and "rect" from
 *       that range of the page's characters.
 * Applications:
 *   type: the name of a registered kind (zebkit_pdf_hook_kind()), and "data",
 *       an object checked against it.
 *
 * Application hooks (zebkit_pdf_hook_before(), ...) run around it.
 */
ZEBKIT_API int zebkit_pdf_annotation_add(zebkit_pdf *pdf, int page, const char *json, char **error);

/*
 * Changes annotation @index of the page: the keys @json has are set, null
 * removes one. Its appearance is generated again, for the types whose
 * appearance is generated, when something it shows changes (not for
 * "contents", but of free text and captions, "author", "subject", "name", the
 * dates or the flags alone); "regenerate": true or false in @json says
 * otherwise. true also replaces an appearance the application made (an image,
 * a canvas, a page), which is otherwise kept.
 */
ZEBKIT_API bool zebkit_pdf_annotation_update(zebkit_pdf *pdf, int page, int index, const char *json, char **error);

/* Removes annotation @index, with its popup, and the replies to it with theirs. */
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

/* The top annotation at (@x, @y) within @tolerance points, or -1 (*@error set
 * only when something was wrong). Hidden annotations and popups do not count;
 * a line counts within its width; a kind's hit test decides for it. */
ZEBKIT_API int zebkit_pdf_annotation_at(zebkit_pdf *pdf, int page, double x, double y, double tolerance, char **error);

/* Moves annotation @index to index @to in the page's order: later is on top. */
ZEBKIT_API bool zebkit_pdf_annotation_move(zebkit_pdf *pdf, int page, int index, int to, char **error);

/*
 * Copies annotation @index to page @target_page of @target (which may be @pdf),
 * with its appearance and all its entries, an application's among them, but
 * without its popup or what it replied to. Returns its index there.
 */
ZEBKIT_API int zebkit_pdf_annotation_copy(zebkit_pdf *pdf, int page, int index, zebkit_pdf *target, int target_page,
                                          char **error);

/* Makes annotation @index's appearance part of the page's content, and removes it. */
ZEBKIT_API bool zebkit_pdf_annotation_flatten(zebkit_pdf *pdf, int page, int index, char **error);

/* Makes page @source_page of @source, as vectors, the appearance of stamp @index. */
ZEBKIT_API bool zebkit_pdf_annotation_stamp_page(zebkit_pdf *pdf, int page, int index, zebkit_pdf *source,
                                                 int source_page, char **error);

/* Embeds @size bytes as the file of file attachment annotation @index, named @name. */
ZEBKIT_API bool zebkit_pdf_annotation_attach_file(zebkit_pdf *pdf, int page, int index, const char *name,
                                                  const void *data, size_t size, char **error);
/* That file's bytes; *@name, when not NULL, its name to free. Sets *@size. */
ZEBKIT_API uint8_t *zebkit_pdf_annotation_file(zebkit_pdf *pdf, int page, int index, char **name, size_t *size,
                                               char **error);

/*
 * Draws @ops (zebkit_canvas_run()) as the appearance of annotation @index: in
 * points, over its box, the origin its top left. What a binding without a
 * direct canvas uses; it is kept as zebkit_pdf_annotation_set_image()'s is.
 */
ZEBKIT_API bool zebkit_pdf_annotation_draw(zebkit_pdf *pdf, int page, int index, const char *ops, char **error);

/*
 * What activating annotation @index means, without doing it: JSON
 * {"kind": "uri", "url": ...} | {"kind": "goto", "page": ...} | {"kind": "named",
 * "name": ...} | {"kind": "launch", "file": ...} | {"kind": "javascript",
 * "script": ...} | {"kind": "other", "action": name} | {"kind": "none"}, with
 * "handled": whether a zebkit_pdf_hook_action() took it.
 */
ZEBKIT_API char *zebkit_pdf_annotation_action(zebkit_pdf *pdf, int page, int index, char **error);

/* ---- Interchange ------------------------------------------------------ */

typedef enum { ZEBKIT_PDF_XFDF, ZEBKIT_PDF_FDF } zebkit_pdf_interchange;

/*
 * The annotations as an XFDF or FDF file: of page @page, or all pages with -1;
 * only annotation @index of that page unless it is -1. Links and widgets are
 * not annotations in these formats. Appearances are not written: whatever
 * reads the file draws them. An application's kind and data travel in elements
 * or entries of zebkit's own, which other readers skip. Sets *@size.
 */
ZEBKIT_API uint8_t *zebkit_pdf_annotations_export(zebkit_pdf *pdf, zebkit_pdf_interchange format, int page, int index,
                                                  size_t *size, char **error);

/*
 * Adds the annotations of an XFDF or FDF file, of at most 64 MiB and 100000
 * annotations, and returns how many. XML is read without the network, without
 * entities and without a DTD; actions in FDF are not taken over. Everything
 * is checked before the first is added, and those added are removed again when
 * a later one fails or a hook refuses.
 */
ZEBKIT_API int zebkit_pdf_annotations_import(zebkit_pdf *pdf, zebkit_pdf_interchange format, const void *data,
                                             size_t size, char **error);

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

/* ---- Hooks ------------------------------------------------------------ */

/*
 * The library is the application's to extend. Every zebkit_pdf_hook_*() takes
 * the document it is for, or NULL for all, @data for the callback, and @destroy
 * (or NULL), called on @data once the hook is gone and no call of it is running.
 * It returns the hook's id for zebkit_pdf_hook_remove(), above 0, or -1. A
 * document's hooks go with it.
 *
 * Callbacks run on the thread that made the call that needs them, outside the
 * lock around PDFium, so they may call this API, which runs its hooks in turn
 * (to a depth of 16; deeper fails). A hook may remove itself or others: one
 * removed during a round is not called again. A hook may close the document:
 * it closes when the outermost call returns, which then fails or says what it
 * did so far; after closing it the hook does not use it again.
 *
 * Strings and bytes a callback gives are from zebkit_pdf_string() and
 * zebkit_pdf_bytes(), and the library frees them.
 */
ZEBKIT_API char *zebkit_pdf_string(const char *text);
ZEBKIT_API uint8_t *zebkit_pdf_bytes(const void *data, size_t size);

/* False when there is no such hook. */
ZEBKIT_API bool zebkit_pdf_hook_remove(int hook);

/*
 * Events. @event is one of these, or "*":
 *   "open"              {"size": bytes | "path": file, "password": ...} (before,
 *                       with @pdf NULL: only hooks for all documents see it)
 *   "save"              {"flags": n}, after also {"size": bytes}
 *   "render"            {"page": 0, "options": {scale, rotation, flags,
 *                       background, x, y, width, height}}, after {"page",
 *                       "width", "height"}
 *   "annotation.add"    {"page": 0, "annotation": {...}}, after also {"index": 3}
 *                       and "annotation" as read
 *   "annotation.update" {"page": 0, "index": 3, "annotation": {changes}}
 *   "annotation.remove" {"page": 0, "index": 3}, after also {"removed": [...]}
 *   "form.set"          {"name": "...", "value": "..."}, after also {"previous"}
 *   "form.reset"        {}
 *   "redact"            {"page": 0, "areas": [...] | null, "fill": rgba}, after
 *                       also {"count": n}
 *   "page.insert"       {"at": 0, "width": 612, "height": 792}
 *   "page.move"         {"pages": [...], "to": 0}
 *   "page.delete"       {"pages": [...]}
 *   "page.rotate"       {"page": 0, "rotation": 90}
 *   "page.import"       {"pages": [...] | null, "at": 0}
 *   "flatten"           {"page": 0, "print": false}
 *   "annotation.flatten" {"page": 0, "index": 3}
 *   "annotation.move"   {"page": 0, "index": 3, "to": 0}
 * @operation is that as JSON with "event" in it. A "before" hook returns
 * ZEBKIT_PDF_CONTINUE or ZEBKIT_PDF_REFUSE, with *@reason (optional) the call's
 * error, and nothing has changed. To change what is about to happen it sets
 * *@change to another @operation, which the next hooks see, and which is checked
 * as a caller's arguments are.
 *
 * An "after" hook sees what happened, as an operation: the arguments as they
 * were used, and when it can be undone "inverse", another operation, which
 * zebkit_pdf_apply() runs: an undo history keeps those, and collaborators are
 * sent the operations themselves.
 */
typedef enum { ZEBKIT_PDF_CONTINUE = 0, ZEBKIT_PDF_REFUSE = 1 } zebkit_pdf_decision;

typedef zebkit_pdf_decision (*zebkit_pdf_before_func)(zebkit_pdf *pdf, const char *operation, char **change,
                                                      char **reason, void *data);
typedef void (*zebkit_pdf_after_func)(zebkit_pdf *pdf, const char *operation, void *data);

ZEBKIT_API int zebkit_pdf_hook_before(zebkit_pdf *pdf, const char *event, zebkit_pdf_before_func func, void *data,
                                      void (*destroy)(void *));
ZEBKIT_API int zebkit_pdf_hook_after(zebkit_pdf *pdf, const char *event, zebkit_pdf_after_func func, void *data,
                                     void (*destroy)(void *));

/*
 * Runs @operation: one that an "after" hook saw, or its "inverse". Events:
 * annotation.add, annotation.update, annotation.remove, annotation.restore,
 * annotation.move, form.set, page.insert, page.delete, page.move, page.rotate.
 * Its own hooks run, so that a redo is one more operation to keep.
 */
ZEBKIT_API bool zebkit_pdf_apply(zebkit_pdf *pdf, const char *operation, char **error);

/*
 * A drawing of an annotation: on @canvas, which writes PDF operators into the
 * appearance stream (<zebkit/canvas.h>, its comment says why), in points, its
 * origin the top left of the annotation's box of @width by @height; @annotation
 * is the annotation as zebkit_pdf_annotations() has it. False when it draws
 * nothing, and the generator goes on.
 */
typedef bool (*zebkit_pdf_appearance_func)(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index,
                                           const char *annotation, double width, double height, void *data);

/*
 * Any type's appearance drawn by the application instead of the generator, for
 * @type as "type" names it ("stamp"), or "*". The last registered goes first.
 */
ZEBKIT_API int zebkit_pdf_hook_appearance(zebkit_pdf *pdf, const char *type, zebkit_pdf_appearance_func func,
                                          void *data, void (*destroy)(void *));

/*
 * A kind of annotation of the application's own.
 *   name:    "type" in JSON, e.g. "MyApp_Sticker".
 *   prefix:  e.g. "MyApp_": its data is kept as the annotation's entry
 *            /MyApp_Data, and every entry of that prefix is carried through save,
 *            copy, XFDF and FDF, and through flatten into the page's /PieceInfo
 *            (zebkit_pdf_page_data()).
 *   subtype: NULL: /Subtype is @name, which other readers draw by its appearance
 *            and do not otherwise know. Or a standard one ("Stamp"), and /MyApp_Kind
 *            says which it is.
 *   schema:  JSON or NULL, what "data" may hold: {"mood": {"type": "string",
 *            "enum": ["a", "b"], "required": true}, "size": {"type": "number",
 *            "min": 1, "max": 9}}. Types: string (also "maxLength"), number, integer,
 *            boolean, array, object. Keys it does not name fail.
 *   check:   or NULL: returns NULL, or what is wrong from zebkit_pdf_string().
 *   draw:    or NULL for no appearance: its appearance (run for every change).
 *   hit_test: or NULL for its rectangle: whether (@x, @y) is on it.
 */
typedef struct {
    const char *name;
    const char *prefix;
    const char *subtype;
    const char *schema;
    char *(*check)(const char *data_json, void *data);
    zebkit_pdf_appearance_func draw;
    bool (*hit_test)(zebkit_pdf *pdf, int page, int index, const char *annotation, double x, double y, void *data);
    void *data;
    void (*destroy)(void *data);
} zebkit_pdf_kind;

ZEBKIT_API int zebkit_pdf_hook_kind(zebkit_pdf *pdf, const zebkit_pdf_kind *kind, char **error);

/*
 * What flatten kept of annotations with entries of @prefix, as JSON
 * [{"Subtype": "/MyApp_Sticker", "Rect": [...], "MyApp_Data": {...}}, ...].
 */
ZEBKIT_API char *zebkit_pdf_page_data(zebkit_pdf *pdf, int page, const char *prefix, char **error);

/*
 * Drawing above or below a page's content when it is rendered, on the page's
 * bitmap, for selections, search results, a cursor or a watermark. @canvas is
 * over that bitmap with page space as its first space: points, y up (text and
 * images stand upright in it). @info says how it maps to pixels, for what is
 * drawn in pixels with zebkit_canvas_set_matrix().
 */
typedef enum { ZEBKIT_PDF_LAYER_BELOW = 0, ZEBKIT_PDF_LAYER_ABOVE = 1 } zebkit_pdf_layer_position;

typedef struct {
    int width, height;   /* of the bitmap, a tile's if a tile is rendered */
    double scale;        /* pixels per point */
    int rotation;        /* the options' */
    double matrix[6];    /* a, b, c, d, e, f: page space to the bitmap's pixels */
} zebkit_pdf_layer_info;

typedef void (*zebkit_pdf_layer_func)(zebkit_canvas *canvas, zebkit_pdf *pdf, int page,
                                      const zebkit_pdf_layer_info *info, void *data);

ZEBKIT_API int zebkit_pdf_hook_layer(zebkit_pdf *pdf, zebkit_pdf_layer_position position, zebkit_pdf_layer_func func,
                                     void *data, void (*destroy)(void *));

/*
 * Text for scanned pages: asked when a page has no text of its own, once per
 * page and document, for zebkit_pdf_text(), zebkit_pdf_text_lines() and
 * zebkit_pdf_search(). Given the page rendered at @scale pixels per point; returns
 * JSON from zebkit_pdf_string(), [{"text": "...", "box": [l, b, r, t]}, ...] in
 * page space, or NULL for none.
 */
typedef char *(*zebkit_pdf_ocr_func)(zebkit_pdf *pdf, int page, const uint8_t *rgba, int width, int height,
                                     double scale, void *data);
ZEBKIT_API int zebkit_pdf_hook_ocr(zebkit_pdf *pdf, zebkit_pdf_ocr_func func, void *data, void (*destroy)(void *));

/*
 * Fonts for free text: asked for a family that is not a standard font, or for
 * "" when text has what the standard fonts do not. Returns a TrueType or OpenType
 * file from zebkit_pdf_bytes(), setting *@size, or NULL.
 */
typedef uint8_t *(*zebkit_pdf_font_func)(zebkit_pdf *pdf, const char *family, size_t *size, void *data);
ZEBKIT_API int zebkit_pdf_hook_font(zebkit_pdf *pdf, zebkit_pdf_font_func func, void *data, void (*destroy)(void *));

/* The same given outright: family @family ("" for what the rest lack) is this file. */
ZEBKIT_API bool zebkit_pdf_font_add(zebkit_pdf *pdf, const char *family, const void *font, size_t size, char **error);

/*
 * Trust in a signer: asked for each of zebkit_pdf_signatures(), with its entry
 * and its PKCS#7. Returns ZEBKIT_PDF_TRUSTED or ZEBKIT_PDF_UNTRUSTED, which that
 * says as "trusted": true or false; ZEBKIT_PDF_TRUST_UNKNOWN says nothing.
 */
typedef enum {
    ZEBKIT_PDF_UNTRUSTED = -1,
    ZEBKIT_PDF_TRUST_UNKNOWN = 0,
    ZEBKIT_PDF_TRUSTED = 1,
} zebkit_pdf_trust;
typedef zebkit_pdf_trust (*zebkit_pdf_trust_func)(zebkit_pdf *pdf, const char *signature, const uint8_t *contents,
                                                  size_t size, void *data);
ZEBKIT_API int zebkit_pdf_hook_trust(zebkit_pdf *pdf, zebkit_pdf_trust_func func, void *data, void (*destroy)(void *));

/* Actions (zebkit_pdf_annotation_action()): true when the application has done it. */
typedef bool (*zebkit_pdf_action_func)(zebkit_pdf *pdf, const char *action, void *data);
ZEBKIT_API int zebkit_pdf_hook_action(zebkit_pdf *pdf, zebkit_pdf_action_func func, void *data,
                                      void (*destroy)(void *));

/*
 * Tools for viewers: what a drawing tool's points go through.
 * zebkit_pdf_filter_input() takes @points, JSON [[x, y], ...], runs the built-in
 * filters @options names, then the hooks for @tool, and returns the points as
 * JSON. Options, a JSON object or NULL, at most 1000000 points:
 *   "smooth": n (1-8): rounds corners, n passes of Chaikin's; for ink.
 *   "simplify": tolerance in points: drops points closer than it to the line.
 *   "snapGrid": spacing: every point to the nearest of a grid.
 *   "snapAngle": degrees: from the first point, the line's angle to a multiple,
 *       its length kept; for two points, a line.
 *   "constrain": "square" | "circle": two corners of a box, made equal sided.
 * @pdf may be NULL. A hook returns new points from zebkit_pdf_string(), or NULL
 * for no change.
 */
typedef char *(*zebkit_pdf_input_func)(zebkit_pdf *pdf, const char *tool, const char *points, void *data);
ZEBKIT_API int zebkit_pdf_hook_input(zebkit_pdf *pdf, const char *tool, zebkit_pdf_input_func func, void *data,
                                     void (*destroy)(void *));
ZEBKIT_API char *zebkit_pdf_filter_input(zebkit_pdf *pdf, const char *tool, const char *points, const char *options,
                                         char **error);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_PDF_H */
