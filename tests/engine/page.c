/* What a page asks of its window (<zebkit/page.h>), in an off-screen view:
 *
 * - dialogs: alert, confirm and prompt held and answered by the
 *   application; the list of open dialogs; the accept and dismiss
 *   policies; "leave this page?" on a navigation, answered both ways;
 *   window.print() reported without holding the page; a view closed as a
 *   tab is, after its beforeunload;
 * - printing to PDF: a known page's PDF read back with <zebkit/pdf.h>, its
 *   pages, their size, landscape, page ranges, a header and footer, print
 *   styles, bad parameters refused;
 * - drag and drop: a sortable list reordered by dragging with the mouse,
 *   the drag reported with what it carries and its picture; a drag the
 *   application takes and ends; Escape; a file dropped from outside, read
 *   by the page; text and a custom type dropped; nothing dropped where the
 *   page takes nothing; a slider dragged with the pointer.
 *
 *   ZEBKIT_TEST_BASE   http://127.0.0.1:<port>, serving tests/engine/pages
 *   ZEBKIT_TEST_FILES  a directory for the file dropped
 *
 * scripts/engine-test.sh runs it with --headless. Exit 0 when every check
 * passes. */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zebkit/offscreen.h>
#include <zebkit/page.h>
#include <zebkit/pdf.h>

static int failures, checks;

static void check(int ok, const char *what, const char *format, ...) {
    ++checks;
    if (!ok)
        ++failures;
    char detail[768] = "";
    if (format) {
        va_list args;
        va_start(args, format);
        vsnprintf(detail, sizeof(detail), format, args);
        va_end(args);
    }
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, *detail ? ": " : "", detail);
    fflush(stdout);
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static zebkit_view *view;
static char base[256], page_url[512], print_url[512], file_path[512];

/* ---- Steps ---------------------------------------------------------------- */

typedef void (*step_func)(void);
static step_func steps[96];
static int step_count, current;
static char result[4096];
static int result_ready;

static void run_step(void *data) {
    (void)data;
    if (current >= step_count) {
        printf("%d of %d checks passed\n", checks - failures, checks);
        zebkit_quit();
        return;
    }
    steps[current++]();
}

static void next(uint32_t delay_ms) { zebkit_post_delayed_task(run_step, NULL, delay_ms); }

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

/* Script whose value comes later (it may wait on a dialog): result_ready. */
static void evaluated_later(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    snprintf(result, sizeof(result), "%s", json ? json : error ? error : "");
    result_ready = 1;
}
static void evaluate_later(const char *script) {
    result_ready = 0;
    result[0] = 0;
    zebkit_view_evaluate(view, script, evaluated_later, NULL);
}
static int have_result(void) { return result_ready; }

/* Script whose value the next step checks. */
static void evaluated(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)data;
    snprintf(result, sizeof(result), "%s", json ? json : error ? error : "");
    next(0);
}
static void evaluate(const char *script) { zebkit_view_evaluate(view, script, evaluated, NULL); }

static void check_result(const char *what, const char *expected) {
    check(strcmp(result, expected) == 0, what, "%s", result);
}

/* ---- Loads ---------------------------------------------------------------- */

static char loaded_url[512];
static int loads_finished;
static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *data) {
    (void)v, (void)data;
    if (event == ZEBKIT_LOAD_FINISHED) {
        snprintf(loaded_url, sizeof(loaded_url), "%s", url);
        ++loads_finished;
    }
}
static int load_wanted;
static char load_path[64];
/* The load of the page asked for: not a late one of the about:blank before. */
static int loaded(void) { return loads_finished >= load_wanted && strstr(loaded_url, load_path); }
static void load(const char *url, const char *what) {
    load_wanted = loads_finished + 1;
    snprintf(load_path, sizeof(load_path), "%s", strrchr(url, '/'));
    zebkit_view_load_url(view, url);
    wait_for(loaded, 15000, what);
}

static void mouse(zebkit_mouse_type type, float x, float y, unsigned modifiers) {
    zebkit_view_send_mouse(view, type, x, y, type == ZEBKIT_MOUSE_MOVE ? ZEBKIT_BUTTON_NONE : ZEBKIT_BUTTON_LEFT,
                           type == ZEBKIT_MOUSE_MOVE ? 0 : 1, modifiers);
}

/* ---- Dialogs -------------------------------------------------------------- */

static zebkit_dialog last_dialog;
static char dialog_message[256], dialog_prompt[256], dialog_url[512];
static int dialogs_seen, dialogs_closed;
static uint64_t closed_id;
static int closed_accepted;
static char closed_input[256];

static void on_dialog(zebkit_view *v, const zebkit_dialog *dialog, void *data) {
    (void)v, (void)data;
    last_dialog = *dialog;
    snprintf(dialog_message, sizeof(dialog_message), "%s", dialog->message);
    snprintf(dialog_prompt, sizeof(dialog_prompt), "%s", dialog->default_prompt);
    snprintf(dialog_url, sizeof(dialog_url), "%s", dialog->url);
    ++dialogs_seen;
}
static void on_dialog_closed(zebkit_view *v, uint64_t id, bool accepted, const char *input, void *data) {
    (void)v, (void)data;
    closed_id = id, closed_accepted = accepted;
    snprintf(closed_input, sizeof(closed_input), "%s", input);
    ++dialogs_closed;
}

static int dialogs_wanted;
static int have_dialog(void) { return dialogs_seen >= dialogs_wanted; }
static void expect_dialog(const char *what) {
    wait_for(have_dialog, 10000, what);
}

static void s_load_page(void) { load(page_url, "the page loaded"); }

static void s_alert(void) {
    dialogs_wanted = dialogs_seen + 1;
    evaluate_later("alert('hello from the page'), 'after alert'");
    expect_dialog("alert() reported");
}
static void s_alert_held(void) {
    check(last_dialog.type == ZEBKIT_DIALOG_ALERT && last_dialog.held, "it is an alert, held", "type %d", last_dialog.type);
    check(strcmp(dialog_message, "hello from the page") == 0, "with its message", "%s", dialog_message);
    check(strstr(dialog_url, "/page.html") != NULL, "and the frame's URL", "%s", dialog_url);
    check(!result_ready, "the page waits for the answer", NULL);
    char *list = zebkit_view_dialogs(view);
    check(strstr(list, "\"type\":\"alert\"") && strstr(list, "hello from the page"), "zebkit_view_dialogs lists it", "%s", list);
    zebkit_free(list);
    check(!zebkit_view_dialog_respond(view, last_dialog.id + 1000, true, NULL), "no answer to a dialog that is not open", NULL);
    check(zebkit_view_dialog_respond(view, last_dialog.id, true, NULL), "zebkit_view_dialog_respond", NULL);
    wait_for(have_result, 5000, "the page goes on");
}
static void s_alert_done(void) {
    check_result("alert() returned", "\"after alert\"");
    check(dialogs_closed == 1 && closed_id == last_dialog.id && closed_accepted, "closed reported", "%d", dialogs_closed);
    char *list = zebkit_view_dialogs(view);
    check(strcmp(list, "[]") == 0, "no dialog open now", "%s", list);
    zebkit_free(list);
    next(0);
}

static void s_confirm(void) {
    dialogs_wanted = dialogs_seen + 1;
    evaluate_later("confirm('Sure?')");
    expect_dialog("confirm() reported");
}
static void s_confirm_answer(void) {
    check(last_dialog.type == ZEBKIT_DIALOG_CONFIRM && strcmp(dialog_message, "Sure?") == 0, "a confirm", "%s", dialog_message);
    zebkit_view_dialog_respond(view, last_dialog.id, false, NULL);
    wait_for(have_result, 5000, "confirm() answered");
}
static void s_confirm_done(void) { check_result("Cancel: confirm() is false", "false"); next(0); }

static void s_prompt(void) {
    dialogs_wanted = dialogs_seen + 1;
    evaluate_later("prompt('Your name?', 'nobody')");
    expect_dialog("prompt() reported");
}
static void s_prompt_answer(void) {
    check(last_dialog.type == ZEBKIT_DIALOG_PROMPT && strcmp(dialog_prompt, "nobody") == 0, "a prompt, with its default",
          "%s", dialog_prompt);
    zebkit_view_dialog_respond(view, last_dialog.id, true, "zebkit");
    wait_for(have_result, 5000, "prompt() answered");
}
static void s_prompt_done(void) {
    check_result("prompt() has the text given", "\"zebkit\"");
    check(strcmp(closed_input, "zebkit") == 0, "and the closed report too", "%s", closed_input);
    next(0);
}
static void s_prompt_default(void) {
    dialogs_wanted = dialogs_seen + 1;
    evaluate_later("prompt('Again?', 'the default')");
    expect_dialog("another prompt");
}
static void s_prompt_default_answer(void) {
    zebkit_view_dialog_respond(view, last_dialog.id, true, NULL);
    wait_for(have_result, 5000, "answered without text");
}
static void s_prompt_default_done(void) { check_result("prompt() has its default text", "\"the default\""); next(0); }

static void s_policy_accept(void) {
    zebkit_view_set_dialog_policy(view, ZEBKIT_DIALOG_POLICY_ACCEPT);
    check(zebkit_view_get_dialog_policy(view) == ZEBKIT_DIALOG_POLICY_ACCEPT, "the policy is set", NULL);
    dialogs_wanted = dialogs_seen + 1;
    evaluate("[confirm('auto?'), prompt('auto name', 'dflt')]");
}
static void s_policy_accept_done(void) {
    check_result("accept: confirm true, prompt its default", "[true,\"dflt\"]");
    check(dialogs_seen >= dialogs_wanted + 1 && !last_dialog.held, "both reported, not held", "%d", dialogs_seen);
    zebkit_view_set_dialog_policy(view, ZEBKIT_DIALOG_POLICY_DISMISS);
    evaluate("[confirm('auto?'), prompt('auto name', 'dflt'), alert('x') === undefined]");
}
static void s_policy_dismiss_done(void) {
    check_result("dismiss: confirm false, prompt null", "[false,null,true]");
    zebkit_view_set_dialog_policy(view, ZEBKIT_DIALOG_POLICY_DEFAULT);
    /* Without a function, the default dismisses as the shell does. */
    zebkit_view_on_dialog(view, NULL, NULL);
    evaluate("confirm('nobody listens')");
}
static void s_default_without_function(void) {
    check_result("default without a function: dismissed", "false");
    zebkit_view_on_dialog(view, on_dialog, NULL);
    next(0);
}

static void s_print_dialog(void) {
    dialogs_wanted = dialogs_seen + 1;
    evaluate_later("window.print(), 'print returned'");
    expect_dialog("window.print() reported");
}
static void s_print_dialog_done(void) {
    check(last_dialog.type == ZEBKIT_DIALOG_PRINT, "as a print dialog", "type %d", last_dialog.type);
    wait_for(have_result, 5000, "and the page goes on without an answer");
}
static void s_print_dialog_answer(void) {
    check_result("print() returned", "\"print returned\"");
    check(zebkit_view_dialog_respond(view, last_dialog.id, false, NULL), "the print dialog answered", NULL);
    next(0);
}

/* beforeunload needs the page to have had a user's gesture. */
static void s_beforeunload(void) {
    mouse(ZEBKIT_MOUSE_MOVE, 300, 250, 0);
    mouse(ZEBKIT_MOUSE_DOWN, 300, 250, 0);
    mouse(ZEBKIT_MOUSE_UP, 300, 250, 0);
    zebkit_view_evaluate(view, "guard()", NULL, NULL);
    next(300);
}
static void s_beforeunload_navigate(void) {
    dialogs_wanted = dialogs_seen + 1;
    zebkit_view_load_url(view, print_url);
    expect_dialog("leaving asks first");
}
static void s_beforeunload_stay(void) {
    check(last_dialog.type == ZEBKIT_DIALOG_BEFOREUNLOAD && last_dialog.held, "a beforeunload dialog, held", "type %d",
          last_dialog.type);
    zebkit_view_dialog_respond(view, last_dialog.id, false, NULL);
    next(800);
}
static void s_beforeunload_stayed(void) {
    char *url = zebkit_view_get_url(view);
    check(strstr(url, "/page.html") != NULL, "dismissed: the page stays", "%s", url);
    zebkit_free(url);
    dialogs_wanted = dialogs_seen + 1;
    load_wanted = loads_finished + 1;
    snprintf(load_path, sizeof(load_path), "/print.html");
    zebkit_view_load_url(view, print_url);
    expect_dialog("asks again");
}
static void s_beforeunload_leave(void) {
    zebkit_view_dialog_respond(view, last_dialog.id, true, NULL);
    wait_for(loaded, 10000, "accepted: the navigation goes on");
}
static void s_beforeunload_left(void) {
    check(strstr(loaded_url, "/print.html") != NULL, "on the next page", "%s", loaded_url);
    next(0);
}

/* ---- PDF ------------------------------------------------------------------ */

static uint8_t *pdf_bytes;
static size_t pdf_size;
static char pdf_error[512];
static int pdf_done;

static void on_pdf(zebkit_view *v, const uint8_t *pdf, size_t size, const char *error, void *data) {
    (void)v, (void)data;
    free(pdf_bytes);
    pdf_bytes = NULL, pdf_size = 0, pdf_error[0] = 0;
    if (pdf) {
        pdf_bytes = malloc(size);
        memcpy(pdf_bytes, pdf, size);
        pdf_size = size;
    } else {
        snprintf(pdf_error, sizeof(pdf_error), "%s", error ? error : "?");
    }
    pdf_done = 1;
}
static int have_pdf(void) { return pdf_done; }
static void print_pdf(const char *options, const char *what) {
    pdf_done = 0;
    zebkit_view_print_to_pdf(view, options, on_pdf, NULL);
    wait_for(have_pdf, 20000, what);
}

/* The document's pages and the first one's size, and its text. */
static int pdf_pages;
static double pdf_width, pdf_height;
static char pdf_text[2048];
static int read_pdf(void) {
    pdf_pages = 0, pdf_width = pdf_height = 0, pdf_text[0] = 0;
    if (!pdf_bytes)
        return 0;
    char *error = NULL;
    zebkit_pdf *pdf = zebkit_pdf_open(pdf_bytes, pdf_size, NULL, &error);
    if (!pdf) {
        check(0, "the PDF opens", "%s", error ? error : "");
        zebkit_free(error);
        return 0;
    }
    pdf_pages = zebkit_pdf_page_count(pdf);
    char *page = zebkit_pdf_page(pdf, 0, &error);
    if (page) {
        const char *w = strstr(page, "\"width\":"), *h = strstr(page, "\"height\":");
        if (w)
            pdf_width = atof(w + 8);
        if (h)
            pdf_height = atof(h + 9);
        zebkit_free(page);
    }
    for (int i = 0; i < pdf_pages; ++i) {
        char *text = zebkit_pdf_text(pdf, i, &error);
        if (text) {
            strncat(pdf_text, text, sizeof(pdf_text) - strlen(pdf_text) - 2);
            strcat(pdf_text, "|");
            zebkit_free(text);
        }
    }
    zebkit_pdf_close(pdf);
    return 1;
}

static void s_pdf(void) { print_pdf(NULL, "the page printed to PDF"); }
static void s_pdf_check(void) {
    check(pdf_bytes && pdf_size > 1000 && memcmp(pdf_bytes, "%PDF-", 5) == 0, "it is a PDF", "%zu bytes %s", pdf_size,
          pdf_error);
    read_pdf();
    check(pdf_pages == 3, "of three pages", "%d", pdf_pages);
    check(pdf_width > 611 && pdf_width < 613 && pdf_height > 791 && pdf_height < 793, "Letter by default", "%gx%g",
          pdf_width, pdf_height);
    check(strstr(pdf_text, "Second page of three") != NULL, "with the page's text", "%.120s", pdf_text);
    check(strstr(pdf_text, "not printed") == NULL, "in its print styles", NULL);
    print_pdf("{\"landscape\": true, \"paperWidth\": 8.27, \"paperHeight\": 11.69, \"pageRanges\": \"2-3\"}",
              "landscape A4, pages 2 and 3");
}
static void s_pdf_landscape(void) {
    read_pdf();
    check(pdf_pages == 2, "two pages", "%d", pdf_pages);
    check(pdf_width > 840 && pdf_width < 843 && pdf_height > 594 && pdf_height < 597, "A4, landscape", "%gx%g", pdf_width,
          pdf_height);
    check(strstr(pdf_text, "First page") == NULL && strstr(pdf_text, "Third page"), "the pages asked for", "%.120s",
          pdf_text);
    print_pdf("{\"displayHeaderFooter\": true, \"marginTop\": 1, \"marginBottom\": 1, "
              "\"headerTemplate\": \"<div style='font-size:10px'>HEAD <span class=title></span></div>\", "
              "\"footerTemplate\": \"<div style='font-size:10px'>FOOT <span class=pageNumber></span>/<span "
              "class=totalPages></span></div>\"}",
              "with a header and footer");
}
static void s_pdf_header(void) {
    read_pdf();
    check(strstr(pdf_text, "HEAD Printed by zebkit") != NULL, "the header, with the title", "%.200s", pdf_text);
    check(strstr(pdf_text, "FOOT 2/3") != NULL, "the footer, with page numbers", "%.300s", pdf_text);
    print_pdf("{\"pageRanges\": \"7-9\"}", "a range past the end");
}
static void s_pdf_bad_range(void) {
    check(!pdf_bytes && *pdf_error, "refused", "%s", pdf_error);
    print_pdf("{\"scale\": 9}", "a scale out of range");
}
static void s_pdf_bad_scale(void) {
    check(!pdf_bytes && *pdf_error, "refused", "%s", pdf_error);
    print_pdf("not json", "options that are not JSON");
}
static void s_pdf_bad_json(void) {
    check(!pdf_bytes && *pdf_error, "refused", "%s", pdf_error);
    next(0);
}

/* ---- Drags ---------------------------------------------------------------- */

static int drags_started, drags_ended;
static unsigned drag_operations, drag_end_operation;
static char drag_data[2048];
static int drag_image_width, drag_image_height;
static float drag_x, drag_y;
static int take_next_drag;

static bool on_drag_start(zebkit_view *v, const zebkit_drag *drag, void *data) {
    (void)v, (void)data;
    ++drags_started;
    drag_operations = drag->operations;
    snprintf(drag_data, sizeof(drag_data), "%s", drag->data_json);
    drag_image_width = drag->image ? drag->image_width : 0;
    drag_image_height = drag->image ? drag->image_height : 0;
    drag_x = drag->x, drag_y = drag->y;
    return take_next_drag;
}
static void on_drag_end(zebkit_view *v, unsigned operation, void *data) {
    (void)v, (void)data;
    ++drags_ended;
    drag_end_operation = operation;
}

static int drags_wanted;
static int drag_started(void) { return drags_started >= drags_wanted; }
static int drag_ended(void) { return drags_ended >= drags_wanted; }

static void s_load_drag_page(void) { load(page_url, "the page again"); }
/* Its first frame drawn, so that the mouse's events have a widget to reach. */
static void s_settle(void) {
    mouse(ZEBKIT_MOUSE_MOVE, 390, 410, 0);
    next(500);
}

/* "a" dragged with the mouse onto "c": the list is b c a d e. */
static void s_drag_list(void) {
    drags_wanted = drags_started + 1;
    mouse(ZEBKIT_MOUSE_MOVE, 50, 20, 0);
    mouse(ZEBKIT_MOUSE_DOWN, 50, 20, 0);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 26, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 34, ZEBKIT_MODIFIER_LEFT_BUTTON);
    wait_for(drag_started, 5000, "the page's drag reported");
}
static void s_drag_list_reported(void) {
    check(zebkit_view_is_dragging(view), "zebkit_view_is_dragging", NULL);
    check(strstr(drag_data, "\"text\":\"item a\"") != NULL, "with its text", "%s", drag_data);
    check(strstr(drag_data, "\"application/x-zebkit-item\":\"a\"") != NULL, "and its custom type", "%s", drag_data);
    check(drag_operations == ZEBKIT_DRAG_MOVE, "allowing a move", "%u", drag_operations);
    check(drag_image_width > 0 && drag_image_height > 0, "with a picture", "%dx%d", drag_image_width,
          drag_image_height);
    check(drag_x > 40 && drag_x < 60 && drag_y > 15 && drag_y < 40, "from where it started", "%g,%g", drag_x, drag_y);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 60, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 95, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 100, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_UP, 50, 100, 0);
    wait_for(drag_ended, 5000, "the drag ended at the mouse's release");
}
static void s_drag_list_ended(void) {
    check(drag_end_operation == ZEBKIT_DRAG_MOVE, "moved", "%u", drag_end_operation);
    check(!zebkit_view_is_dragging(view), "no drag now", NULL);
    next(200);
}
static void s_drag_list_order(void) { evaluate("[order(), events.join(' ')]"); }
static void s_drag_list_order_check(void) {
    check_result("the list reordered: dragstart, drop, dragend", "[\"bcade\",\"dragstart drop:a dragend:move\"]");
    evaluate("events = []; 1");
}

/* Escape ends a drag without a drop. */
static void s_drag_escape(void) {
    drags_wanted = drags_started + 1;
    mouse(ZEBKIT_MOUSE_MOVE, 50, 140, 0);
    mouse(ZEBKIT_MOUSE_DOWN, 50, 140, 0);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 146, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_MOVE, 50, 154, ZEBKIT_MODIFIER_LEFT_BUTTON);
    wait_for(drag_started, 5000, "another drag");
}
static void s_drag_escape_press(void) {
    mouse(ZEBKIT_MOUSE_MOVE, 50, 20, ZEBKIT_MODIFIER_LEFT_BUTTON);
    zebkit_key_event escape = {ZEBKIT_KEY_DOWN, 0, 0x1B, 0, "Escape", "Escape", NULL, NULL};
    zebkit_view_send_key(view, &escape);
    escape.type = ZEBKIT_KEY_UP;
    zebkit_view_send_key(view, &escape);
    wait_for(drag_ended, 5000, "Escape ends it");
}
static void s_drag_escape_ended(void) {
    check(drag_end_operation == ZEBKIT_DRAG_NONE, "with nothing done", "%u", drag_end_operation);
    mouse(ZEBKIT_MOUSE_UP, 50, 20, 0);
    next(200);
}
static void s_drag_escape_order(void) { evaluate("[order(), events.join(' ')]"); }
static void s_drag_escape_check(void) {
    check_result("the list as it was, dragend with none", "[\"bcade\",\"dragstart dragend:none\"]");
    evaluate("events = []; 1");
}

/* A drag the application takes: links, HTML, text; ended by it. */
static void s_drag_taken(void) {
    take_next_drag = 1;
    drags_wanted = drags_started + 1;
    mouse(ZEBKIT_MOUSE_MOVE, 300, 20, 0);
    mouse(ZEBKIT_MOUSE_DOWN, 300, 20, 0);
    mouse(ZEBKIT_MOUSE_MOVE, 306, 20, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_MOVE, 314, 20, ZEBKIT_MODIFIER_LEFT_BUTTON);
    wait_for(drag_started, 5000, "a drag the application takes");
}
static void s_drag_taken_check(void) {
    take_next_drag = 0;
    check(strstr(drag_data, "\"url\":\"https://example.com/out\"") && strstr(drag_data, "\"html\":\"\\u003Cb>out") &&
              strstr(drag_data, "\"text\":\"out text\""),
          "what it carries: a link, HTML and text", "%s", drag_data);
    check(drag_operations == (ZEBKIT_DRAG_COPY | ZEBKIT_DRAG_LINK), "copy or link", "%u", drag_operations);
    check(zebkit_view_is_dragging(view), "it is under way", NULL);
    zebkit_view_drag_source_end(view, 500, 500, ZEBKIT_DRAG_COPY);
    mouse(ZEBKIT_MOUSE_UP, 314, 20, 0);
    next(300);
}
static void s_drag_taken_events(void) { evaluate("events.join(' ')"); }
static void s_drag_taken_check_events(void) {
    check_result("the page's dragend has what was done", "\"out-dragstart out-dragend:copy\"");
    check(!zebkit_view_is_dragging(view), "and it is over", NULL);
    next(0);
}

/* A file from outside, dropped on the page. */
static unsigned over_operation = 99, drop_operation = 99;
static int drop_done;
static void on_over(zebkit_view *v, unsigned operation, void *data) {
    (void)v, (void)data;
    over_operation = operation;
}
static void on_drop(zebkit_view *v, unsigned operation, void *data) {
    (void)v, (void)data;
    drop_operation = operation;
    drop_done = 1;
}
static int have_drop(void) { return drop_done; }
static int have_over(void) { return over_operation != 99; }

static void s_drop_file(void) {
    FILE *f = fopen(file_path, "w");
    fputs("dropped contents", f);
    fclose(f);
    char json[1024];
    snprintf(json, sizeof(json), "{\"files\": [\"%s\"]}", file_path);
    zebkit_view_drag_enter(view, json, 100, 240, ZEBKIT_DRAG_COPY | ZEBKIT_DRAG_LINK, 0);
    zebkit_view_drag_over(view, 110, 250, ZEBKIT_DRAG_COPY | ZEBKIT_DRAG_LINK, 0, on_over, NULL);
    wait_for(have_over, 5000, "a file dragged over the drop zone");
}
static void s_drop_file_over(void) {
    check(over_operation == ZEBKIT_DRAG_COPY, "the page will copy it", "%u", over_operation);
    drop_done = 0;
    zebkit_view_drop(view, 110, 250, ZEBKIT_MODIFIER_SHIFT, on_drop, NULL);
    wait_for(have_drop, 5000, "dropped");
}
static void s_drop_file_dropped(void) {
    check(drop_operation == ZEBKIT_DRAG_COPY, "copied", "%u", drop_operation);
    next(300);
}
static void s_drop_file_page(void) { evaluate("JSON.stringify(dropped)"); }
static void s_drop_file_check(void) {
    check(strstr(result, "dropped.txt") && strstr(result, "dropped contents") && strstr(result, "Files"),
          "the page has the file and reads it", "%s", result);
    check(strstr(result, "\\\"shiftKey\\\":true") != NULL, "with the modifiers", "%s", result);
    over_operation = 99;
    zebkit_view_drag_enter(view, "{\"text\": \"plain words\", \"custom\": {\"application/x-mine\": \"mine\"}}", 50, 250,
                           ZEBKIT_DRAG_COPY, 0);
    zebkit_view_drag_over(view, 60, 260, ZEBKIT_DRAG_COPY, 0, on_over, NULL);
    drop_done = 0;
    zebkit_view_drop(view, 60, 260, 0, on_drop, NULL);
    wait_for(have_drop, 5000, "text and a custom type dropped");
}
static void s_drop_text(void) { next(300); }
static void s_drop_text_page(void) { evaluate("JSON.stringify(dropped)"); }
static void s_drop_text_check(void) {
    check(strstr(result, "plain words") && strstr(result, "\\\"custom\\\":\\\"mine\\\""), "the page has both",
          "%s", result);
    /* Onto <input type=file>: it takes the file. */
    char json[1024];
    snprintf(json, sizeof(json), "{\"files\": [\"%s\"]}", file_path);
    zebkit_view_drag_enter(view, json, 100, 340, ZEBKIT_DRAG_COPY, 0);
    zebkit_view_drag_over(view, 100, 340, ZEBKIT_DRAG_COPY, 0, NULL, NULL);
    drop_done = 0;
    zebkit_view_drop(view, 100, 340, 0, on_drop, NULL);
    wait_for(have_drop, 5000, "a file dropped on <input type=file>");
}
static void s_drop_input(void) { next(300); }
static void s_drop_input_page(void) { evaluate("[document.getElementById('file').files.length, String(window.chosen)]"); }
static void s_drop_input_check(void) {
    check_result("the input has the file", "[1,\"dropped.txt\"]");
    /* Where the page takes nothing (the slider's margin): nothing done. */
    zebkit_view_drag_enter(view, "{\"text\": \"nowhere\"}", 390, 395, ZEBKIT_DRAG_COPY, 0);
    zebkit_view_drag_over(view, 390, 395, ZEBKIT_DRAG_COPY, 0, NULL, NULL);
    drop_done = 0;
    zebkit_view_drop(view, 390, 395, 0, on_drop, NULL);
    wait_for(have_drop, 5000, "a drop where nothing takes it");
}
static void s_drop_nowhere(void) {
    check(drop_operation == ZEBKIT_DRAG_NONE, "nothing done", "%u", drop_operation);
    next(0);
}

/* A slider dragged with the pointer: plain mouse events, no drag. */
static void s_slider(void) {
    mouse(ZEBKIT_MOUSE_MOVE, 4, 390, 0);
    mouse(ZEBKIT_MOUSE_DOWN, 4, 390, 0);
    for (int x = 10; x <= 150; x += 20)
        mouse(ZEBKIT_MOUSE_MOVE, (float)x, 390, ZEBKIT_MODIFIER_LEFT_BUTTON);
    mouse(ZEBKIT_MOUSE_UP, 150, 390, 0);
    next(300);
}
static void s_slider_value(void) { evaluate("Number(document.getElementById('slider').value)"); }
static void s_slider_check(void) {
    int value = atoi(result);
    check(value > 60 && value < 85, "the slider followed the pointer", "%s", result);
    next(0);
}

/* ---- Closing as a tab ---------------------------------------------------- */

static int closed;
static void on_close(zebkit_view *v, void *data) {
    (void)v, (void)data;
    closed = 1;
}
static int is_closed(void) { return closed; }

static void s_close_guarded(void) {
    mouse(ZEBKIT_MOUSE_DOWN, 300, 250, 0);
    mouse(ZEBKIT_MOUSE_UP, 300, 250, 0);
    zebkit_view_evaluate(view, "guard()", NULL, NULL);
    next(300);
}
static void s_request_close(void) {
    dialogs_wanted = dialogs_seen + 1;
    zebkit_view_request_close(view);
    expect_dialog("closing asks the page");
}
static void s_request_close_stay(void) {
    check(last_dialog.type == ZEBKIT_DIALOG_BEFOREUNLOAD, "beforeunload", "type %d", last_dialog.type);
    zebkit_view_dialog_respond(view, last_dialog.id, false, NULL);
    next(800);
}
static void s_request_close_kept(void) {
    check(!closed, "dismissed: the view stays open", NULL);
    dialogs_wanted = dialogs_seen + 1;
    zebkit_view_request_close(view);
    expect_dialog("asked again");
}
static void s_request_close_leave(void) {
    zebkit_view_dialog_respond(view, last_dialog.id, true, NULL);
    wait_for(is_closed, 5000, "accepted: the view closes");
}

static void ready(void *data) {
    (void)data;
    const char *env = getenv("ZEBKIT_TEST_BASE");
    snprintf(base, sizeof(base), "%s", env);
    snprintf(page_url, sizeof(page_url), "%s/page.html", base);
    snprintf(print_url, sizeof(print_url), "%s/print.html", base);
    const char *files = getenv("ZEBKIT_TEST_FILES");
    snprintf(file_path, sizeof(file_path), "%s/dropped.txt", files ? files : "/tmp");
    view = zebkit_view_new_offscreen(NULL, 400, 420, 1);
    check(view != NULL, "an off-screen view", NULL);
    if (!view) {
        zebkit_quit();
        return;
    }
    zebkit_view_on_load(view, on_load, NULL);
    zebkit_view_on_close(view, on_close, NULL);
    zebkit_view_on_dialog(view, on_dialog, NULL);
    zebkit_view_on_dialog_closed(view, on_dialog_closed, NULL);
    zebkit_view_on_drag_start(view, on_drag_start, NULL);
    zebkit_view_on_drag_end(view, on_drag_end, NULL);
    zebkit_view_set_focus(view, true);
    step_func all[] = {
        s_load_page,
        s_alert, s_alert_held, s_alert_done,
        s_confirm, s_confirm_answer, s_confirm_done,
        s_prompt, s_prompt_answer, s_prompt_done, s_prompt_default, s_prompt_default_answer, s_prompt_default_done,
        s_policy_accept, s_policy_accept_done, s_policy_dismiss_done, s_default_without_function,
        s_print_dialog, s_print_dialog_done, s_print_dialog_answer,
        s_beforeunload, s_beforeunload_navigate, s_beforeunload_stay, s_beforeunload_stayed, s_beforeunload_leave,
        s_beforeunload_left,
        s_pdf, s_pdf_check, s_pdf_landscape, s_pdf_header, s_pdf_bad_range, s_pdf_bad_scale, s_pdf_bad_json,
        s_load_drag_page, s_settle,
        s_drag_list, s_drag_list_reported, s_drag_list_ended, s_drag_list_order, s_drag_list_order_check,
        s_drag_escape, s_drag_escape_press, s_drag_escape_ended, s_drag_escape_order, s_drag_escape_check,
        s_drag_taken, s_drag_taken_check, s_drag_taken_events, s_drag_taken_check_events,
        s_drop_file, s_drop_file_over, s_drop_file_dropped, s_drop_file_page, s_drop_file_check,
        s_drop_text, s_drop_text_page, s_drop_text_check, s_drop_input, s_drop_input_page, s_drop_input_check,
        s_drop_nowhere,
        s_slider, s_slider_value, s_slider_check,
        s_close_guarded, s_request_close, s_request_close_stay, s_request_close_kept, s_request_close_leave,
    };
    step_count = (int)(sizeof(all) / sizeof(all[0]));
    memcpy(steps, all, sizeof(all));
    next(0);
}

int main(int argc, char **argv) {
    if (!getenv("ZEBKIT_TEST_BASE")) {
        fprintf(stderr, "set ZEBKIT_TEST_BASE: http://127.0.0.1:<port>, serving tests/engine/pages\n");
        return 2;
    }
    int code = zebkit_main(argc, argv, ready, NULL);
    if (code)
        return code;
    return failures || checks < 60 ? 1 : 0;
}
