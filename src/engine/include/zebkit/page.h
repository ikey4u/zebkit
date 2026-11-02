/*
 * zebkit: what a page asks of its window, for the application to answer.
 *
 * - JavaScript dialogs: alert(), confirm(), prompt(), a beforeunload
 *   handler's "leave this page?", and window.print(), held open and
 *   reported to the application (or to a CDP client) instead of being
 *   dismissed, as each view's dialog policy says.
 * - The page printed to PDF, as Chrome's Page.printToPDF does it (which CDP
 *   clients get as well).
 * - Drag and drop: drags the page starts, reported with what they carry,
 *   and run inside the page from the mouse events the application sends an
 *   off-screen view; drags the application brings into the page, files from
 *   the system's file manager among them.
 *
 * Everything here is for the engine's thread, as in <zebkit/zebkit.h>.
 */

#ifndef ZEBKIT_PAGE_H
#define ZEBKIT_PAGE_H

#include <zebkit/offscreen.h>
#include <zebkit/zebkit.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- JavaScript dialogs ---------------------------------------------- */

typedef enum {
    ZEBKIT_DIALOG_ALERT = 0,
    ZEBKIT_DIALOG_CONFIRM = 1,
    ZEBKIT_DIALOG_PROMPT = 2,
    /* "Leave this page?" of a beforeunload handler: accepting leaves (or
     * reloads, or closes), dismissing stays. There is no message: browsers
     * show their own text, not the page's. */
    ZEBKIT_DIALOG_BEFOREUNLOAD = 3,
    /* window.print(). The page is not held: print() returns at once, and
     * the application may print the page itself (zebkit_view_print_to_pdf).
     * Answering it only closes it. */
    ZEBKIT_DIALOG_PRINT = 4,
} zebkit_dialog_type;

/* What becomes of a view's dialogs. The policy is the view's, and may be
 * changed at any time; a dialog already open stays as it is. */
typedef enum {
    /* To zebkit_view_on_dialog()'s function when there is one, held until
     * it is answered. Otherwise held for a CDP client attached to the page
     * (Page.javascriptDialogOpening, Page.handleJavaScriptDialog), as
     * headless Chrome holds them. Otherwise what the engine does by itself: on
     * macOS a window's dialog is the system's alert, elsewhere and without
     * a window it is dismissed at once (alert returns, confirm is false,
     * prompt null) and beforeunload leaves. The default. */
    ZEBKIT_DIALOG_POLICY_DEFAULT = 0,
    /* Always held, until the application or a CDP client answers it: a page
     * that alerts waits. */
    ZEBKIT_DIALOG_POLICY_HOLD = 1,
    /* Answered at once with "OK": confirm is true, prompt its default text,
     * beforeunload leaves. Still reported, opened and closed. */
    ZEBKIT_DIALOG_POLICY_ACCEPT = 2,
    /* Answered at once with "Cancel": confirm is false, prompt null,
     * beforeunload stays on the page. Still reported, opened and closed. */
    ZEBKIT_DIALOG_POLICY_DISMISS = 3,
} zebkit_dialog_policy;

typedef struct {
    /* The view's dialogs are numbered 1, 2, 3, ... in the process. */
    uint64_t id;
    zebkit_dialog_type type;
    /* The page's text; "" for beforeunload and print. */
    const char *message;
    /* A prompt's default text, else "". */
    const char *default_prompt;
    /* The URL of the frame that opened it. */
    const char *url;
    /* A beforeunload of a reload, not of leaving. */
    bool is_reload;
    /* Whether the dialog is held for an answer: false when the policy
     * answered it already (zebkit_view_on_dialog_closed follows). */
    bool held;
} zebkit_dialog;

typedef void (*zebkit_dialog_func)(zebkit_view *view, const zebkit_dialog *dialog, void *data);
typedef void (*zebkit_dialog_closed_func)(zebkit_view *view, uint64_t id, bool accepted, const char *user_input,
                                          void *data);

/* Calls @func for every dialog the view's pages open, held or not; NULL
 * stops. With a function set, ZEBKIT_DIALOG_POLICY_DEFAULT holds them for
 * the application. */
ZEBKIT_API void zebkit_view_on_dialog(zebkit_view *view, zebkit_dialog_func func, void *data);

/* Calls @func when a dialog closes, however it was answered: by the
 * application, by a CDP client, by the policy, or because the page went
 * (accepted false). @user_input is the prompt's text, else "". */
ZEBKIT_API void zebkit_view_on_dialog_closed(zebkit_view *view, zebkit_dialog_closed_func func, void *data);

ZEBKIT_API void zebkit_view_set_dialog_policy(zebkit_view *view, zebkit_dialog_policy policy);
ZEBKIT_API zebkit_dialog_policy zebkit_view_get_dialog_policy(zebkit_view *view);

/* Answers dialog @id: OK (@accept) or Cancel, a prompt with @prompt_text
 * (NULL for its default text). False when the view has no such dialog
 * open. */
ZEBKIT_API bool zebkit_view_dialog_respond(zebkit_view *view, uint64_t id, bool accept, const char *prompt_text);

/* The view's open dialogs as a JSON array, oldest first:
 * [{"id": 1, "type": "confirm", "message": "...", "defaultPrompt": "",
 *   "url": "https://...", "isReload": false}]. The caller frees it. */
ZEBKIT_API char *zebkit_view_dialogs(zebkit_view *view);

/* Closes the view as a browser closes a tab: the page's beforeunload
 * handlers run first, and a "leave this page?" dialog it asks for is
 * reported as any other; the view closes when the page lets it, and stays
 * otherwise. zebkit_view_close() closes without asking. */
ZEBKIT_API void zebkit_view_request_close(zebkit_view *view);

/* ---- Printing to PDF ------------------------------------------------- */

/* @pdf is the document, @size bytes, the engine's for the call; or NULL
 * with @error. */
typedef void (*zebkit_pdf_func)(zebkit_view *view, const uint8_t *pdf, size_t size, const char *error, void *data);

/*
 * Prints the view's page to PDF, as Chrome prints it, with the parameters
 * of CDP's Page.printToPDF as a JSON object (NULL or "{}" for the
 * defaults): landscape, displayHeaderFooter, printBackground, scale (0.1
 * to 2), paperWidth and paperHeight (inches; Letter by default),
 * marginTop, marginBottom, marginLeft, marginRight (inches; 0.4 by
 * default), pageRanges ("1-5, 8"), headerTemplate and footerTemplate
 * (HTML, with the classes date, title, url, pageNumber, totalPages),
 * preferCSSPageSize, generateTaggedPDF, generateDocumentOutline.
 * The page is printed in the browser's print mode (@media print); frames
 * of other sites inside it are not printed.
 */
ZEBKIT_API void zebkit_view_print_to_pdf(zebkit_view *view, const char *options_json, zebkit_pdf_func func,
                                         void *data);

/* ---- Drag and drop --------------------------------------------------- */

/* Drag operations, as masks of what is allowed and as what was done. */
enum {
    ZEBKIT_DRAG_NONE = 0,
    ZEBKIT_DRAG_COPY = 1,
    ZEBKIT_DRAG_LINK = 2,
    ZEBKIT_DRAG_MOVE = 16,
    ZEBKIT_DRAG_ALL = ZEBKIT_DRAG_COPY | ZEBKIT_DRAG_LINK | ZEBKIT_DRAG_MOVE,
};

/*
 * What a drag carries, as JSON, both ways:
 *
 *     {"text": "...", "html": "...", "htmlBaseUrl": "https://...",
 *      "urls": [{"url": "https://...", "title": "..."}],
 *      "files": ["/path/to/file", ...],
 *      "custom": {"application/x-mine": "...", ...},
 *      "fileContents": {"name": "image.png", "url": "https://...",
 *                       "base64": "..."}}
 *
 * Every member may be missing. "files" are paths the page may read once
 * they are dropped on it; from the page they are files it dragged out of
 * an <input type=file> or a drop before. "fileContents" is an image the
 * page drags (an <img>), with its bytes.
 */
typedef struct {
    const char *data_json;
    /* What the page allows: ZEBKIT_DRAG_* combined. */
    unsigned operations;
    /* Where the drag started in the view, in CSS pixels. */
    float x, y;
    /* The picture the page gives the drag, BGRA premultiplied, @stride
     * bytes a row, the engine's for the call; NULL when there is none. */
    const uint8_t *image;
    int image_width, image_height, image_stride;
    /* Device pixels per CSS pixel of the image. */
    float image_scale;
    /* Where the cursor is in the image, in CSS pixels from its top left. */
    float image_offset_x, image_offset_y;
} zebkit_drag;

/*
 * The page started dragging. Return false to let the engine run the drag
 * inside the page: in an off-screen view, the mouse moves the application
 * sends from now on are the drag's, over the page (dragenter, dragover,
 * drop), until the button is released, Escape is pressed or the mouse
 * leaves and comes back without the button. Return true to run it
 * yourself (the system's drag and drop, say): send it back over the page
 * with zebkit_view_drag_enter() and the rest if it comes back, and end it
 * with zebkit_view_drag_source_end().
 *
 * Without a function, an off-screen view's drags are run by the engine,
 * and a window's by the system, as before.
 */
typedef bool (*zebkit_drag_func)(zebkit_view *view, const zebkit_drag *drag, void *data);
/* The page's drag ended, with what was done (ZEBKIT_DRAG_NONE if it was
 * not dropped, or dropped where nothing took it). */
typedef void (*zebkit_drag_end_func)(zebkit_view *view, unsigned operation, void *data);
/* The operation the page chose for a drag over it or dropped on it. */
typedef void (*zebkit_drag_operation_func)(zebkit_view *view, unsigned operation, void *data);

ZEBKIT_API void zebkit_view_on_drag_start(zebkit_view *view, zebkit_drag_func func, void *data);
ZEBKIT_API void zebkit_view_on_drag_end(zebkit_view *view, zebkit_drag_end_func func, void *data);

/* Whether a drag the page started is under way. */
ZEBKIT_API bool zebkit_view_is_dragging(zebkit_view *view);

/*
 * A drag into the page from outside it: @data_json (as above) enters at
 * (@x, @y), CSS pixels in the view, allowing @operations
 * (ZEBKIT_DRAG_*), with @modifiers (ZEBKIT_MODIFIER_*, <zebkit/offscreen.h>).
 * Then it moves (zebkit_view_drag_over, whose function gets the operation
 * the page chose), and leaves or is dropped. A dropped file is the page's
 * to read, as from the system's drag and drop, and zebkit.pathForFile()
 * knows its path. Any view, with a window or without.
 */
ZEBKIT_API void zebkit_view_drag_enter(zebkit_view *view, const char *data_json, float x, float y,
                                       unsigned operations, unsigned modifiers);
ZEBKIT_API void zebkit_view_drag_over(zebkit_view *view, float x, float y, unsigned operations, unsigned modifiers,
                                      zebkit_drag_operation_func func, void *data);
ZEBKIT_API void zebkit_view_drag_leave(zebkit_view *view);
/* Drops it at (@x, @y); @func gets the operation done, as the last
 * dragover chose it. */
ZEBKIT_API void zebkit_view_drop(zebkit_view *view, float x, float y, unsigned modifiers,
                                 zebkit_drag_operation_func func, void *data);

/* Ends the drag the page started, at (@x, @y), with what was done with it
 * (ZEBKIT_DRAG_NONE for nothing): the page gets its dragend. */
ZEBKIT_API void zebkit_view_drag_source_end(zebkit_view *view, float x, float y, unsigned operation);

#ifdef __cplusplus
}
#endif

#endif /* ZEBKIT_PAGE_H */
