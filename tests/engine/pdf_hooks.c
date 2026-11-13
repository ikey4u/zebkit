/* <zebkit/pdf.h>'s hooks: every one of them, the example plugin that uses them,
 * and what a hook may do to the call that runs it. Takes a font's path. */

#include "pdf_support.h"

#include "plugin.h"

static size_t font_size;
static char *font_bytes;

static int count_annotations(zebkit_pdf *pdf, int page) {
    char *json = zebkit_pdf_annotations(pdf, page, NULL);
    int count = 0;
    for (const char *at = json; at && (at = strstr(at, "\"index\":")); at += 8)
        count++;
    zebkit_free(json);
    return count;
}

static int listed(zebkit_pdf *pdf, int page, const char *what) {
    char *json = zebkit_pdf_annotations(pdf, page, NULL);
    const int found = json && strstr(json, what);
    zebkit_free(json);
    return found;
}

/* ---- The example plugin ---- */

static void test_plugin(void) {
    zebkit_pdf *pdf = blank(300, 300);
    plugin *p = plugin_install(pdf);
    char *error = NULL;

    /* The kind: checks, drawing, hit test, data. */
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":{}}", &error) < 0, &error, "a sticker needs a mood");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":{\"mood\":\"angry\"}}", &error) < 0, &error, "not one of the moods");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":{\"mood\":\"sad\",\"size\":99}}", &error) < 0, &error, "above its maximum");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":{\"mood\":\"sad\",\"x\":1}}", &error) < 0, &error, "a property it does not have");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":{\"mood\":\"\"}}", &error) < 0, &error, "the kind's own check");
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":5}", &error) < 0, &error, "data that is no object");
    check_text(count_annotations(pdf, 0) == 0, "nothing was added", NULL);
    const int sticker = zebkit_pdf_annotation_add(
        pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[50,50,150,150],\"data\":{\"mood\":\"happy\",\"size\":3},\"contents\":\"a note\"}", &error);
    check_text(sticker == 0, "a sticker", error);
    zebkit_free(error);
    error = NULL;
    shot s = render(pdf, 0);
    check_text(near_color(at(&s, 100, 130), 255, 221, 0) && white(at(&s, 53, 53)), "its own appearance: a disc", NULL);
    check_text(near_color(at(&s, 100, 75), 255, 0, 0), "and a red smile, y down in the canvas", NULL);
    done(&s);
    check_text(listed(pdf, 0, "\"type\":\"Sample_Sticker\"") && listed(pdf, 0, "\"data\":{\"mood\":\"happy\",\"size\":3}"), "its data read", NULL);
    check_text(zebkit_pdf_annotation_at(pdf, 0, 100, 100, 0, NULL) == 0 && zebkit_pdf_annotation_at(pdf, 0, 53, 53, 0, NULL) == -1,
          "its hit test: not the corners", NULL);

    /* Changes are checked against what it would have, and redrawn. */
    refused(!zebkit_pdf_annotation_update(pdf, 0, 0, "{\"data\":{\"mood\":\"angry\"}}", &error), &error, "a change is checked");
    check_text(zebkit_pdf_annotation_update(pdf, 0, 0, "{\"data\":{\"mood\":\"sad\"}}", NULL), "a change", NULL);
    check_text(listed(pdf, 0, "\"data\":{\"mood\":\"sad\",\"size\":3}"), "merged with what it had", NULL);
    s = render(pdf, 0);
    check_text(!near_color(at(&s, 100, 75), 255, 0, 0) && near_color(at(&s, 100, 85), 0, 0, 255), "redrawn: a blue frown", NULL);
    done(&s);

    /* Through save. */
    zebkit_pdf *again = reopened(pdf);
    plugin *q = plugin_install(again);
    s = render(again, 0);
    check_text(near_color(at(&s, 100, 140), 255, 221, 0), "after a save, its appearance", NULL);
    done(&s);
    check_text(listed(again, 0, "\"type\":\"Sample_Sticker\"") && listed(again, 0, "\"mood\":\"sad\""), "and its data", NULL);
    plugin_remove(q);
    /* Without the plugin too: its appearance is in the file. */
    s = render(again, 0);
    check_text(inked(&s, 50, 50, 150, 150) > 500, "and without the plugin", NULL);
    check_text(listed(again, 0, "\"subtype\":\"Sample_Sticker\"") && listed(again, 0, "\"Sample_Data\":"), "read as another's", NULL);
    done(&s);
    zebkit_pdf_close(again);

    /* Through copy. */
    zebkit_pdf *other = blank(300, 300);
    plugin *r = plugin_install(other);
    check_text(zebkit_pdf_annotation_copy(pdf, 0, 0, other, 0, NULL) == 0 && listed(other, 0, "\"mood\":\"sad\"") &&
              listed(other, 0, "\"type\":\"Sample_Sticker\""),
          "through a copy", NULL);

    /* Through XFDF and FDF: redrawn by the kind where it is read. */
    for (int round = 0; round < 2; ++round) {
        const zebkit_pdf_interchange format = round ? ZEBKIT_PDF_FDF : ZEBKIT_PDF_XFDF;
        size_t size = 0;
        uint8_t *file = zebkit_pdf_annotations_export(pdf, format, -1, -1, &size, &error);
        check_text(file != NULL, round ? "written as FDF" : "written as XFDF", error);
        zebkit_free(error);
        error = NULL;
        check_text(file && memmem(file, size, round ? "Sample_Data" : "zebkit-annot", round ? 11 : 12) != NULL, "with its kind", NULL);
        zebkit_pdf *back = blank(300, 300);
        /* Not registered there: it fails, and says so. */
        refused(zebkit_pdf_annotations_import(back, format, file, size, &error) < 0, &error, "no such kind where it is read");
        plugin *t = plugin_install(back);
        const int read = zebkit_pdf_annotations_import(back, format, file, size, &error);
        check_text(read == 1, round ? "FDF back" : "XFDF back", error);
        zebkit_free(error);
        error = NULL;
        check_text(listed(back, 0, "\"type\":\"Sample_Sticker\"") && listed(back, 0, "\"mood\":\"sad\"") && listed(back, 0, "\"size\":3"),
              "the kind and its data", NULL);
        s = render(back, 0);
        check_text(inked(&s, 50, 50, 150, 150) > 500, "its appearance", NULL);
        done(&s);
        plugin_remove(t);
        zebkit_free(file);
        zebkit_pdf_close(back);
    }

    /* Through flatten: its data stays with the page. */
    check_text(zebkit_pdf_flatten(other, 0, false, NULL) && count_annotations(other, 0) == 0, "flattened", NULL);
    char *kept = zebkit_pdf_page_data(other, 0, "Sample_", NULL);
    check_text(kept && strstr(kept, "\"Sample_Data\":{\"mood\":\"sad\"") && strstr(kept, "/Sample_Sticker"), "its data stays", kept);
    zebkit_free(kept);
    zebkit_pdf *saved = reopened(other);
    kept = zebkit_pdf_page_data(saved, 0, "Sample_", NULL);
    check_text(kept && strstr(kept, "\"mood\":\"sad\""), "after a save", NULL);
    zebkit_free(kept);
    zebkit_pdf_close(saved);
    plugin_remove(r);
    zebkit_pdf_close(other);
    refused(!zebkit_pdf_page_data(pdf, 9, "Sample_", &error), &error, "data of no page");
    plugin_remove(p);

    /* The undo history: every operation's inverse, back and forward. */
    zebkit_pdf_close(pdf);
    pdf = blank(300, 300);
    p = plugin_install(pdf);
    shot empty = render(pdf, 0);
    int steps = plugin_history(p);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,150,150],\"interiorColor\":[255,0,0],\"border\":0}", NULL);
    check_text(plugin_history(p) == steps + 1, "the history grows", NULL);
    shot one = render(pdf, 0);
    zebkit_pdf_annotation_update(pdf, 0, 0, "{\"interiorColor\":[0,0,255]}", NULL);
    shot two = render(pdf, 0);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Sample_Sticker\",\"rect\":[160,160,200,200],\"data\":{\"mood\":\"sad\"}}", NULL);
    shot three = render(pdf, 0);
    zebkit_pdf_annotation_remove(pdf, 0, 0, NULL);
    shot four = render(pdf, 0);
    check_text(!same_pixels(&three, &four) && count_annotations(pdf, 0) == 1, "removed", NULL);
    check_text(plugin_undo(p, pdf) && count_annotations(pdf, 0) == 2, "undo a removal", NULL);
    shot back = render(pdf, 0);
    check_text(same_pixels(&back, &three), "the same pixels as before it", NULL);
    done(&back);
    check_text(plugin_undo(p, pdf) && count_annotations(pdf, 0) == 1, "undo an addition", NULL);
    back = render(pdf, 0);
    check_text(same_pixels(&back, &two), "the same pixels", NULL);
    done(&back);
    check_text(plugin_undo(p, pdf), "undo a change", NULL);
    back = render(pdf, 0);
    check_text(same_pixels(&back, &one), "its old colour", NULL);
    done(&back);
    check_text(plugin_undo(p, pdf) && count_annotations(pdf, 0) == 0, "undo the first", NULL);
    back = render(pdf, 0);
    check_text(same_pixels(&back, &empty), "an empty page", NULL);
    done(&back);
    check_text(!plugin_undo(p, pdf), "nothing more to undo", NULL);
    check_text(plugin_redo(p, pdf) && count_annotations(pdf, 0) == 1, "redo", NULL);
    check_text(plugin_redo(p, pdf), "redo a change", NULL);
    back = render(pdf, 0);
    check_text(same_pixels(&back, &two), "as it was", NULL);
    done(&back);
    done(&empty);
    done(&one);
    done(&two);
    done(&three);
    done(&four);
    /* Pages and form fields are operations too. */
    int before = plugin_history(p);
    zebkit_pdf_page_insert(pdf, 1, 100, 100, NULL);
    zebkit_pdf_page_rotate(pdf, 0, 90, NULL);
    check_text(plugin_history(p) == before + 2 && zebkit_pdf_page_count(pdf) == 2, "pages in the history", NULL);
    check_text(plugin_undo(p, pdf) && plugin_undo(p, pdf) && zebkit_pdf_page_count(pdf) == 1, "undone", NULL);
    plugin_remove(p);
    zebkit_pdf_close(pdf);

    /* OCR: text for a page without any, once per page, in search too. */
    pdf = blank(300, 300);
    p = plugin_install(pdf);
    char *text = zebkit_pdf_text(pdf, 0, NULL);
    check_text(text && !strcmp(text, "read from page 0"), "OCR: its text", text);
    zebkit_free(text);
    char *lines = zebkit_pdf_text_lines(pdf, 0, NULL);
    check_text(lines && strstr(lines, "read from page 0") && strstr(lines, "\"ocr\":true"), "as lines", lines);
    zebkit_free(lines);
    char *found = zebkit_pdf_search(pdf, "PAGE", 0, 0, 0, NULL);
    check_text(found && strstr(found, "\"page\":0") && strstr(found, "\"ocr\":true"), "found by search", found);
    zebkit_free(found);
    plugin_remove(p);
    zebkit_pdf_close(pdf);
}

/* ---- Every event ---- */

struct told {
    int before, after;
    char last[4096];
};

static zebkit_pdf_decision count_before(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)change; (void)reason;
    struct told *t = data;
    t->before++;
    snprintf(t->last, sizeof(t->last), "%s", operation);
    return ZEBKIT_PDF_CONTINUE;
}

static void count_after(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf;
    struct told *t = data;
    t->after++;
    snprintf(t->last, sizeof(t->last), "%s", operation);
}

static zebkit_pdf_decision refuse(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)operation; (void)change; (void)data;
    *reason = zebkit_pdf_string("not today");
    return ZEBKIT_PDF_REFUSE;
}

static const char form_pdf[] =
    "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] >> >> endobj\n"
    "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
    "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [4 0 R] >> endobj\n"
    "4 0 obj << /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (old) /Rect [10 10 100 30] /P 3 0 R >> endobj\n"
    "trailer << /Root 1 0 R >>\n%%EOF\n";

/* What a document is, for telling whether it has changed: a save's bytes differ
 * in its file identifier each time. */
static char *state_of(zebkit_pdf *pdf) {
    char *info = zebkit_pdf_info(pdf);
    char *fields = zebkit_pdf_form_fields(pdf, NULL);
    size_t room = strlen(info) + strlen(fields ? fields : "") + 64;
    const int pages = zebkit_pdf_page_count(pdf);
    char **listed_pages = calloc((size_t)pages, sizeof(char *));
    for (int i = 0; i < pages; ++i) {
        char *page = zebkit_pdf_page(pdf, i, NULL);
        char *annotations = zebkit_pdf_annotations(pdf, i, NULL);
        shot s = render(pdf, i);
        unsigned long sum = 5381;
        for (int k = 0; s.rgba && k < s.width * s.height * 4; ++k)
            sum = sum * 33 + s.rgba[k];
        done(&s);
        const size_t size = strlen(page) + strlen(annotations) + 32;
        listed_pages[i] = malloc(size);
        snprintf(listed_pages[i], size, "%s%s%lu", page, annotations, sum);
        room += size;
        zebkit_free(page);
        zebkit_free(annotations);
    }
    char *state = malloc(room);
    snprintf(state, room, "%s%s", info, fields ? fields : "");
    for (int i = 0; i < pages; ++i) {
        strcat(state, listed_pages[i]);
        free(listed_pages[i]);
    }
    free(listed_pages);
    zebkit_free(info);
    zebkit_free(fields);
    return state;
}

/* Runs an action: told before and after; refused, nothing changes. */
static void check_event(zebkit_pdf *pdf, const char *event, int (*act)(zebkit_pdf *, char **)) {
    struct told t = {0};
    int a = zebkit_pdf_hook_before(pdf, event, count_before, &t, NULL);
    int b = zebkit_pdf_hook_after(pdf, event, count_after, &t, NULL);
    check_text(a > 0 && b > 0, "hooks registered", event);
    char *state_before = NULL;
    /* Its own hooks are told of this render, and of none in the refused call. */
    zebkit_pdf_hook_remove(a);
    zebkit_pdf_hook_remove(b);
    state_before = state_of(pdf);
    a = zebkit_pdf_hook_before(pdf, event, count_before, &t, NULL);
    b = zebkit_pdf_hook_after(pdf, event, count_after, &t, NULL);
    int refusal = zebkit_pdf_hook_before(pdf, event, refuse, NULL, NULL);
    char *error = NULL;
    const int result = act(pdf, &error);
    check_text(!result && error && !strcmp(error, "not today"), event, error ? error : "refused");
    zebkit_free(error);
    error = NULL;
    check_text(t.before == 1 && t.after == 0, "told before, not after", event);
    zebkit_pdf_hook_remove(refusal);
    zebkit_pdf_hook_remove(a);
    zebkit_pdf_hook_remove(b);
    char *state_after = state_of(pdf);
    check_text(!strcmp(state_before, state_after), "refused: the document is as it was, in pixels too", event);
    free(state_before);
    free(state_after);
    a = zebkit_pdf_hook_before(pdf, event, count_before, &t, NULL);
    b = zebkit_pdf_hook_after(pdf, event, count_after, &t, NULL);
    check_text(act(pdf, &error) != 0, event, error ? error : "done");
    zebkit_free(error);
    check_text(t.before == 2 && t.after == 1, "told before and after", event);
    check_text(strstr(t.last, event) != NULL, "its name in the operation", t.last);
    zebkit_pdf_hook_remove(a);
    zebkit_pdf_hook_remove(b);
}

static int act_add(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,50,50]}", error) >= 0;
}
static int act_update(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_annotation_update(pdf, 0, 1, "{\"contents\":\"x\"}", error);
}
static int act_move(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_annotation_move(pdf, 0, 1, 2, error);
}
static int act_remove(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_annotation_remove(pdf, 0, 2, error);
}
static int act_form(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_form_set(pdf, "name", "new", error);
}
static int act_reset(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_form_reset(pdf, error);
}
static int act_redact(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_redact(pdf, 0, "[[150,150,160,160]]", 0, error) >= 0;
}
static int act_insert(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_page_insert(pdf, 1, 100, 100, error);
}
static int act_page_move(zebkit_pdf *pdf, char **error) {
    const int pages[] = {1};
    return zebkit_pdf_pages_move(pdf, pages, 1, 0, error);
}
static int act_page_delete(zebkit_pdf *pdf, char **error) {
    const int pages[] = {0};
    return zebkit_pdf_pages_delete(pdf, pages, 1, error);
}
static int act_rotate(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_page_rotate(pdf, 0, 90, error);
}
static int act_flatten_one(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_annotation_flatten(pdf, 0, 0, error);
}
static int act_flatten(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_flatten(pdf, 0, false, error);
}
static int act_render(zebkit_pdf *pdf, char **error) {
    int w, h;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, NULL, &w, &h, error);
    zebkit_free(pixels);
    return pixels != NULL;
}
static int act_save(zebkit_pdf *pdf, char **error) {
    size_t size;
    uint8_t *file = zebkit_pdf_save(pdf, 0, &size, error);
    zebkit_free(file);
    return file != NULL;
}
static zebkit_pdf *import_source;
static int act_import(zebkit_pdf *pdf, char **error) {
    return zebkit_pdf_pages_import(pdf, import_source, NULL, 0, 0, error);
}

static void test_events(void) {
    zebkit_pdf *pdf = zebkit_pdf_open(form_pdf, sizeof(form_pdf) - 1, NULL, NULL);
    check_event(pdf, "annotation.add", act_add);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[60,60,70,70],\"interiorColor\":[1,2,3]}", NULL);
    check_event(pdf, "annotation.update", act_update);
    check_event(pdf, "annotation.move", act_move);
    check_event(pdf, "annotation.remove", act_remove);
    check_event(pdf, "form.set", act_form);
    {
        struct told t = {0};
        int after = zebkit_pdf_hook_after(pdf, "form.set", count_after, &t, NULL);
        zebkit_pdf_form_set(pdf, "name", "newer", NULL);
        check_text(strstr(t.last, "\"previous\":\"new\"") && strstr(t.last, "\"inverse\":{"), "a field's change undoes", t.last);
        zebkit_pdf_hook_remove(after);
    }
    check_event(pdf, "form.reset", act_reset);
    check_event(pdf, "redact", act_redact);
    check_event(pdf, "page.insert", act_insert);
    check_event(pdf, "page.move", act_page_move);
    check_event(pdf, "page.rotate", act_rotate);
    check_event(pdf, "render", act_render);
    {
        /* Flattening one: the first has an appearance. */
        int hook = zebkit_pdf_hook_before(pdf, "annotation.add", NULL, NULL, NULL);
        check_text(hook < 0, "no hook without a function", NULL);
        zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,50,50]}", NULL);
        zebkit_pdf_annotation_move(pdf, 0, 1, 0, NULL);
        check_event(pdf, "annotation.flatten", act_flatten_one);
    }
    check_event(pdf, "flatten", act_flatten);
    {
        /* Saving, unlike the rest: the file itself shows it was called. */
        struct told t = {0};
        int a = zebkit_pdf_hook_before(pdf, "save", count_before, &t, NULL);
        int b = zebkit_pdf_hook_after(pdf, "save", count_after, &t, NULL);
        check_text(act_save(pdf, NULL) && t.before == 1 && t.after == 1 && strstr(t.last, "\"size\":"), "save told", t.last);
        int c = zebkit_pdf_hook_before(pdf, "save", refuse, NULL, NULL);
        char *error = NULL;
        refused(!act_save(pdf, &error), &error, "save refused");
        zebkit_pdf_hook_remove(a);
        zebkit_pdf_hook_remove(b);
        zebkit_pdf_hook_remove(c);
    }
    import_source = blank(50, 50);
    check_event(pdf, "page.import", act_import);
    zebkit_pdf_close(import_source);
    check_text(zebkit_pdf_page_count(pdf) > 1, "pages there to delete", NULL);
    check_event(pdf, "page.delete", act_page_delete);
    zebkit_pdf_close(pdf);

    /* Opening: for all documents, before there is one. */
    struct told t = {0};
    int a = zebkit_pdf_hook_before(NULL, "open", count_before, &t, NULL);
    int b = zebkit_pdf_hook_after(NULL, "open", count_after, &t, NULL);
    pdf = zebkit_pdf_open(form_pdf, sizeof(form_pdf) - 1, NULL, NULL);
    check_text(pdf && t.before == 1 && t.after == 1, "opening told", t.last);
    zebkit_pdf_close(pdf);
    int c = zebkit_pdf_hook_before(NULL, "open", refuse, NULL, NULL);
    char *error = NULL;
    refused(!zebkit_pdf_open(form_pdf, sizeof(form_pdf) - 1, NULL, &error), &error, "opening refused");
    zebkit_pdf_hook_remove(a);
    zebkit_pdf_hook_remove(b);
    zebkit_pdf_hook_remove(c);
}

/* ---- Before may change ---- */

static zebkit_pdf_decision to_blue(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)operation; (void)reason; (void)data;
    *change = zebkit_pdf_string("{\"page\":0,\"annotation\":{\"type\":\"square\",\"rect\":[10,10,50,50],\"interiorColor\":[0,0,255],\"border\":0}}");
    return ZEBKIT_PDF_CONTINUE;
}

static zebkit_pdf_decision bad_change(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)operation; (void)reason; (void)data;
    *change = zebkit_pdf_string("not json");
    return ZEBKIT_PDF_CONTINUE;
}

static zebkit_pdf_decision wrong_change(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)operation; (void)reason; (void)data;
    *change = zebkit_pdf_string("{\"page\":0,\"annotation\":{\"type\":\"square\",\"rect\":\"nope\"}}");
    return ZEBKIT_PDF_CONTINUE;
}

static zebkit_pdf_decision twice_as_big(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)reason; (void)data; (void)operation;
    *change = zebkit_pdf_string("{\"page\":0,\"options\":{\"scale\":2,\"rotation\":0,\"flags\":1,\"background\":4294967295,\"x\":0,\"y\":0,\"width\":0,\"height\":0}}");
    return ZEBKIT_PDF_CONTINUE;
}

static void test_changes(void) {
    zebkit_pdf *pdf = blank(100, 100);
    char *error = NULL;
    int hook = zebkit_pdf_hook_before(pdf, "annotation.add", to_blue, NULL, NULL);
    check_text(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[60,60,70,70],\"interiorColor\":[255,0,0]}", NULL) == 0, "added", NULL);
    shot s = render(pdf, 0);
    check_text(near_color(at(&s, 30, 30), 0, 0, 255) && white(at(&s, 65, 65)), "as the hook changed it", NULL);
    done(&s);
    zebkit_pdf_hook_remove(hook);
    hook = zebkit_pdf_hook_before(pdf, "annotation.add", bad_change, NULL, NULL);
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", &error) < 0, &error, "a change that is not JSON");
    zebkit_pdf_hook_remove(hook);
    hook = zebkit_pdf_hook_before(pdf, "annotation.add", wrong_change, NULL, NULL);
    refused(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", &error) < 0, &error, "a change is checked like an argument");
    zebkit_pdf_hook_remove(hook);
    check_text(count_annotations(pdf, 0) == 1, "neither added anything", NULL);
    hook = zebkit_pdf_hook_before(pdf, "render", twice_as_big, NULL, NULL);
    int w = 0, h = 0;
    uint8_t *pixels = zebkit_pdf_render(pdf, 0, NULL, &w, &h, NULL);
    check_text(pixels && w == 200, "a render changed by a hook", NULL);
    zebkit_free(pixels);
    zebkit_pdf_hook_remove(hook);
    zebkit_pdf_close(pdf);
}

/* ---- Re-entrancy ---- */

static int destroyed;
static void count_destroyed(void *data) {
    (void)data;
    destroyed++;
}

static int self_removing_id;
static int self_removing_calls;
static void removes_itself(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf; (void)operation; (void)data;
    self_removing_calls++;
    check_text(destroyed == 0, "not yet destroyed while it runs", NULL);
    check_text(zebkit_pdf_hook_remove(self_removing_id), "it removes itself", NULL);
    check_text(!zebkit_pdf_hook_remove(self_removing_id), "only once", NULL);
    check_text(destroyed == 0, "nor just after, inside its own call", NULL);
}

static int second_id, second_calls;
static void removes_next(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf; (void)operation; (void)data;
    zebkit_pdf_hook_remove(second_id);
}
static void second_hook(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf; (void)operation; (void)data;
    second_calls++;
}

static zebkit_pdf_decision closes_it(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)operation; (void)change; (void)reason; (void)data;
    zebkit_pdf_close(pdf);
    return ZEBKIT_PDF_CONTINUE;
}

static int after_close_calls;
static void closes_after(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)operation; (void)data;
    after_close_calls++;
    zebkit_pdf_close(pdf);
}
static void not_after_close(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf; (void)operation; (void)data;
    check_text(0, "a hook after one that closed the document", NULL);
}

static int nested_calls;
static zebkit_pdf_decision reads_inside(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)operation; (void)change; (void)reason; (void)data;
    /* Not deadlocked: the API from inside a hook, while a call is running. */
    char *json = zebkit_pdf_annotations(pdf, 0, NULL);
    check_text(json != NULL, "the API inside a hook", NULL);
    zebkit_free(json);
    nested_calls++;
    return ZEBKIT_PDF_CONTINUE;
}

static zebkit_pdf_decision recurses(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)operation; (void)change; (void)reason; (void)data;
    /* Adds, whose hook adds, and so on: stopped at a depth, with an error. */
    nested_calls++;
    if (nested_calls < 1000) {
        char *error = NULL;
        if (zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", &error) < 0)
            zebkit_free(error);
    }
    return ZEBKIT_PDF_CONTINUE;
}

static zebkit_pdf_decision closes_other(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)operation; (void)change; (void)reason;
    zebkit_pdf_close(data);
    return ZEBKIT_PDF_CONTINUE;
}

static int layer_removed_id;
static void layer_removes_itself(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data) {
    (void)canvas; (void)pdf; (void)page; (void)info; (void)data;
    zebkit_pdf_hook_remove(layer_removed_id);
}
static void destroy_calls_in(void *data) {
    destroyed += zebkit_pdf_page_count(data);
}

static char *kept_inverse;
static void keeps_inverse(zebkit_pdf *pdf, const char *operation, void *data) {
    (void)pdf; (void)data;
    const char *from = strstr(operation, "\"inverse\":");
    const char *to = from;
    for (const char *at = from; at && (at = strstr(at + 1, ",\"page\":"));)
        to = at;
    free(kept_inverse);
    kept_inverse = strndup(from + 10, (size_t)(to - from - 10));
}

static char *input_changes(zebkit_pdf *pdf, const char *tool, const char *points, void *data) {
    (void)pdf; (void)tool; (void)points; (void)data;
    return zebkit_pdf_string("[[7,7]]");
}

static zebkit_pdf_decision goes_on(zebkit_pdf *pdf, const char *operation, char **change, char **reason, void *data) {
    (void)pdf; (void)operation; (void)change; (void)reason; (void)data;
    return ZEBKIT_PDF_CONTINUE;
}

static void test_more_reentrancy(void) {
    char *error = NULL;
    /* An "after open" hook closes it: the caller is not handed what has gone. */
    int global = zebkit_pdf_hook_after(NULL, "open", closes_after, NULL, NULL);
    after_close_calls = 0;
    zebkit_pdf *pdf = zebkit_pdf_open(form_pdf, sizeof(form_pdf) - 1, NULL, &error);
    check_text(!pdf && after_close_calls == 1, "opened and closed by a hook: nothing comes back", NULL);
    refused(!pdf, &error, "said so");
    zebkit_pdf_hook_remove(global);

    /* Closing while a file of annotations is read. */
    static const char xfdf[] = "<xfdf><annots><text page=\"0\" rect=\"1,1,2,2\"/><text page=\"0\" rect=\"3,3,4,4\"/></annots></xfdf>";
    pdf = blank(100, 100);
    zebkit_pdf_hook_after(pdf, "annotation.add", closes_after, NULL, NULL);
    after_close_calls = 0;
    check_text(zebkit_pdf_annotations_import(pdf, ZEBKIT_PDF_XFDF, xfdf, sizeof(xfdf) - 1, &error) < 0 && after_close_calls == 1,
          "an import stops where a hook closed the document", NULL);
    refused(after_close_calls == 1, &error, "said so");

    /* Closing the document pages come from. */
    pdf = blank(100, 100);
    zebkit_pdf *source = blank(50, 50);
    int hook = zebkit_pdf_hook_before(pdf, "page.import", closes_other, source, NULL);
    check_text(zebkit_pdf_pages_import(pdf, source, NULL, 0, 0, NULL) && zebkit_pdf_page_count(pdf) == 2, "the source stays for the import", NULL);
    zebkit_pdf_hook_remove(hook);

    /* A layer that removes itself, whose destroy function calls in. */
    destroyed = 0;
    layer_removed_id = zebkit_pdf_hook_layer(pdf, ZEBKIT_PDF_LAYER_ABOVE, layer_removes_itself, pdf, destroy_calls_in);
    shot s = render(pdf, 0);
    done(&s);
    check_text(destroyed == 2, "destroyed when the render is over, outside its lock", NULL);

    /* A redaction whose areas cannot be read is not of the page's marks. */
    hook = zebkit_pdf_hook_before(pdf, "redact", goes_on, NULL, NULL);
    refused(zebkit_pdf_redact(pdf, 0, "not json", 0xff, &error) < 0, &error, "areas that are not JSON, with a hook");
    zebkit_pdf_hook_remove(hook);

    /* No tool is no tool's filter. */
    hook = zebkit_pdf_hook_input(pdf, "ink", input_changes, NULL, NULL);
    char *points = zebkit_pdf_filter_input(pdf, "", "[[1,1]]", NULL, NULL);
    check_text(points && !strstr(points, "7"), "a filter of another tool is not run", points);
    zebkit_free(points);
    zebkit_pdf_hook_remove(hook);

    /* An undo leaves what it does not change: a file. */
    const int attached = zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"fileAttachment\",\"rect\":[1,1,9,9]}", NULL);
    check_text(zebkit_pdf_annotation_attach_file(pdf, 0, attached, "a.txt", "file", 4, NULL), "attached", NULL);
    hook = zebkit_pdf_hook_after(pdf, "annotation.update", keeps_inverse, NULL, NULL);
    check_text(zebkit_pdf_annotation_update(pdf, 0, attached, "{\"contents\":\"changed\"}", NULL) && kept_inverse, "changed", NULL);
    zebkit_pdf_hook_remove(hook);
    check_text(zebkit_pdf_apply(pdf, kept_inverse, &error), "undone", error);
    zebkit_free(error);
    error = NULL;
    size_t got = 0;
    uint8_t *bytes = zebkit_pdf_annotation_file(pdf, 0, attached, NULL, &got, NULL);
    check_text(bytes && got == 4 && !memcmp(bytes, "file", 4) && !listed(pdf, 0, "changed"), "the file stays after an undo", NULL);
    zebkit_free(bytes);
    free(kept_inverse);
    kept_inverse = NULL;
    zebkit_pdf_close(pdf);
}

static void test_reentrancy(void) {
    zebkit_pdf *pdf = blank(100, 100);
    destroyed = 0;
    self_removing_id = zebkit_pdf_hook_after(pdf, "annotation.add", removes_itself, NULL, count_destroyed);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", NULL);
    check_text(self_removing_calls == 1 && destroyed == 1, "destroyed once the call is over", NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", NULL);
    check_text(self_removing_calls == 1, "not called again", NULL);

    /* One that removes the next in the same round. */
    int first = zebkit_pdf_hook_after(pdf, "annotation.add", removes_next, NULL, NULL);
    second_id = zebkit_pdf_hook_after(pdf, "annotation.add", second_hook, NULL, NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", NULL);
    check_text(second_calls == 0, "one removed in the round is not called", NULL);
    zebkit_pdf_hook_remove(first);

    /* The API inside a hook. */
    int inside = zebkit_pdf_hook_before(pdf, "annotation.add", reads_inside, NULL, NULL);
    check_text(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", NULL) >= 0 && nested_calls == 1, "went on", NULL);
    zebkit_pdf_hook_remove(inside);

    /* Recursion. */
    nested_calls = 0;
    int recursive = zebkit_pdf_hook_before(pdf, "annotation.add", recurses, NULL, NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", NULL);
    check_text(nested_calls > 1 && nested_calls <= 17, "a hook that calls itself is stopped", NULL);
    zebkit_pdf_hook_remove(recursive);

    /* Closing from a "before" hook: the call fails, nothing crashes. */
    destroyed = 0;
    zebkit_pdf_hook_before(pdf, "annotation.update", closes_it, NULL, count_destroyed);
    char *error = NULL;
    refused(!zebkit_pdf_annotation_update(pdf, 0, 0, "{\"contents\":\"x\"}", &error), &error, "a hook closed the document");
    check_text(destroyed == 1, "its hooks are gone with it", NULL);

    /* From an "after" hook: the call has happened, later hooks are skipped. */
    pdf = blank(100, 100);
    zebkit_pdf_hook_after(pdf, "annotation.add", closes_after, NULL, NULL);
    zebkit_pdf_hook_after(pdf, "annotation.add", not_after_close, NULL, NULL);
    check_text(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[1,1,2,2]}", NULL) == 0 && after_close_calls == 1,
          "closed after the fact", NULL);

    /* Closing during a save, a render and a removal. */
    const char *events[] = {"save", "render", "annotation.remove", "form.set", "page.insert", "flatten"};
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); ++i) {
        pdf = zebkit_pdf_open(form_pdf, sizeof(form_pdf) - 1, NULL, NULL);
        zebkit_pdf_hook_before(pdf, events[i], closes_it, NULL, NULL);
        int ok = 0;
        switch (i) {
            case 0: { size_t size; uint8_t *f = zebkit_pdf_save(pdf, 0, &size, &error); ok = f != NULL; zebkit_free(f); break; }
            case 1: { uint8_t *p = zebkit_pdf_render(pdf, 0, NULL, NULL, NULL, &error); ok = p != NULL; zebkit_free(p); break; }
            case 2: ok = zebkit_pdf_annotation_remove(pdf, 0, 0, &error); break;
            case 3: ok = zebkit_pdf_form_set(pdf, "name", "x", &error); break;
            case 4: ok = zebkit_pdf_page_insert(pdf, 0, 10, 10, &error); break;
            case 5: ok = zebkit_pdf_flatten(pdf, 0, false, &error); break;
        }
        refused(!ok, &error, events[i]);
    }

    /* A global hook goes on after documents go; its removal is by id. */
    destroyed = 0;
    int global = zebkit_pdf_hook_after(NULL, "*", second_hook, NULL, count_destroyed);
    pdf = blank(10, 10);
    zebkit_pdf_close(pdf);
    check_text(destroyed == 0, "a hook for all stays", NULL);
    check_text(zebkit_pdf_hook_remove(global) && destroyed == 1 && !zebkit_pdf_hook_remove(global), "until removed", NULL);
    check_text(!zebkit_pdf_hook_remove(-1) && !zebkit_pdf_hook_remove(999999), "no such hook", NULL);
    check_text(zebkit_pdf_hook_before(NULL, NULL, count_before, NULL, NULL) < 0 && zebkit_pdf_hook_before(NULL, "x", NULL, NULL, NULL) < 0,
          "a hook of nothing fails", NULL);
}

/* ---- Appearance providers, layers, kinds' rules ---- */

static bool draw_red(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, int index, const char *annotation, double width,
                     double height, void *data) {
    (void)pdf; (void)page; (void)index; (void)annotation;
    if (data)
        return false;
    /* With everything a canvas has: state, gradient, image, text as outlines, blend. */
    zebkit_canvas_set_fill_color(canvas, 0xff0000ff);
    zebkit_canvas_rect(canvas, 0, 0, width, height);
    zebkit_canvas_fill(canvas, false);
    zebkit_canvas_save(canvas);
    zebkit_canvas_begin_path(canvas);
    zebkit_canvas_rect(canvas, 0, 0, width / 2, height / 2);
    zebkit_canvas_clip(canvas, false);
    zebkit_canvas_set_fill_color(canvas, 0x00ff00ff);
    zebkit_canvas_begin_path(canvas);
    zebkit_canvas_rect(canvas, 0, 0, width, height);
    zebkit_canvas_fill(canvas, false);
    zebkit_canvas_restore(canvas);
    const double offsets[] = {0, 1};
    const uint32_t colors[] = {0x000000ff, 0xffffffff};
    zebkit_canvas_set_linear_gradient(canvas, 0, height / 2, width / 2, height / 2, offsets, colors, 2);
    zebkit_canvas_begin_path(canvas);
    zebkit_canvas_rect(canvas, 0, height / 2, width / 2, height / 4);
    zebkit_canvas_fill(canvas, false);
    const uint8_t blue[] = {0, 0, 255, 255};
    zebkit_canvas_draw_image(canvas, blue, 1, 1, width / 2, 0, width / 2, height / 2);
    zebkit_canvas_font *font = zebkit_canvas_font_new(font_bytes, font_size, NULL);
    zebkit_canvas_set_font(canvas, font, height / 5);
    zebkit_canvas_font_close(font);
    zebkit_canvas_set_fill_color(canvas, 0xffffffff);
    zebkit_canvas_fill_text(canvas, "X", width / 2 + 2, height - 4, NULL);
    /* A failure inside a canvas of PDF: pixels are not there. */
    char *error = NULL;
    check_text(!zebkit_canvas_pixels(canvas, &error) && error, "a canvas of PDF has no pixels", NULL);
    zebkit_free(error);
    return true;
}

static void test_providers(void) {
    /* For a type, on a document. */
    zebkit_pdf *pdf = blank(200, 200);
    int hook = zebkit_pdf_hook_appearance(pdf, "stamp", draw_red, NULL, NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"stamp\",\"rect\":[0,0,200,200],\"stamp\":\"Draft\"}", NULL);
    shot s = render(pdf, 0);
    /* y down in the canvas: its top left is the page's upper left. */
    check_text(near_color(at(&s, 50, 150), 0, 255, 0), "provider: a clip, top left", NULL);
    check_text(near_color(at(&s, 150, 150), 0, 0, 255), "an image, top right", NULL);
    check_text(near_color(at(&s, 150, 50), 255, 0, 0), "a fill, bottom right", NULL);
    check_text(at(&s, 5, 75)[0] < 60 && at(&s, 95, 75)[0] > 190, "a gradient, as a shading", NULL);
    check_text(white(at(&s, 110, 20)), "text as outlines, upright", NULL);
    check_text(!white(at(&s, 145, 20)), "and only there", NULL);
    done(&s);
    /* Kept through a save. */
    zebkit_pdf *again = reopened(pdf);
    s = render(again, 0);
    check_text(near_color(at(&s, 50, 150), 0, 255, 0) && near_color(at(&s, 150, 150), 0, 0, 255), "after a save", NULL);
    done(&s);
    zebkit_pdf_close(again);
    zebkit_pdf_hook_remove(hook);
    zebkit_pdf_close(pdf);

    /* It may decline, and the generator goes on; "*" takes every type. */
    pdf = blank(200, 200);
    hook = zebkit_pdf_hook_appearance(pdf, "*", draw_red, (void *)1, NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"square\",\"rect\":[50,50,150,150],\"interiorColor\":[0,0,255],\"border\":0}", NULL);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 100, 100), 0, 0, 255), "declined: the generator's", NULL);
    done(&s);
    zebkit_pdf_hook_remove(hook);
    /* For all documents. */
    hook = zebkit_pdf_hook_appearance(NULL, "circle", draw_red, NULL, NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"circle\",\"rect\":[0,0,40,40]}", NULL);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 30, 10), 255, 0, 0), "a provider for all documents", NULL);
    done(&s);
    zebkit_pdf_hook_remove(hook);
    /* An operation list as an appearance. */
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"stamp\",\"rect\":[100,100,200,200]}", NULL);
    check_text(zebkit_pdf_annotation_draw(pdf, 0, 2, "[[\"fillColor\",4278190335],[\"rect\",0,0,100,50],[\"fill\",false]]", NULL), "operations as an appearance", NULL);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 150, 175), 255, 0, 0) && white(at(&s, 150, 125)), "from its top", NULL);
    done(&s);
    zebkit_pdf_close(pdf);
}

static void layer_above(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data) {
    (void)pdf; (void)page; (void)data;
    check_text(info->width > 0 && info->scale > 0, "told the bitmap's size", NULL);
    /* Page space: (50, 50) up from the bottom left. */
    zebkit_canvas_set_fill_color(canvas, 0xff000080);
    zebkit_canvas_rect(canvas, 50, 50, 40, 40);
    zebkit_canvas_fill(canvas, false);
    /* Text stands upright there. */
    zebkit_canvas_font *font = zebkit_canvas_font_new(font_bytes, font_size, NULL);
    zebkit_canvas_set_font(canvas, font, 20);
    zebkit_canvas_font_close(font);
    zebkit_canvas_set_fill_color(canvas, 0x000000ff);
    zebkit_canvas_fill_text(canvas, "X", 100, 100, NULL);
}

static void layer_below(zebkit_canvas *canvas, zebkit_pdf *pdf, int page, const zebkit_pdf_layer_info *info, void *data) {
    (void)pdf; (void)page; (void)info; (void)data;
    zebkit_canvas_set_fill_color(canvas, 0x00ff00ff);
    zebkit_canvas_rect(canvas, 0, 0, 300, 200);
    zebkit_canvas_fill(canvas, false);
}

static void test_layers(void) {
    static const char page[] =
        "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R >> endobj\n"
        "4 0 obj << >> stream\n0 0 1 rg 60 60 100 100 re f\nendstream endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    zebkit_pdf *pdf = zebkit_pdf_open(page, sizeof(page) - 1, NULL, NULL);
    int above = zebkit_pdf_hook_layer(pdf, ZEBKIT_PDF_LAYER_ABOVE, layer_above, NULL, NULL);
    shot s = render(pdf, 0);
    /* Half red over white, half red over the page's blue. */
    check_text(near_color(at(&s, 55, 55), 255, 128, 128), "a layer above, on white", NULL);
    check_text(near_color(at(&s, 75, 75), 128, 0, 128), "and over the content", NULL);
    check_text(near_color(at(&s, 110, 105), 0, 0, 0) && !near_color(at(&s, 110, 125), 0, 0, 0), "text upright, its baseline in page space", NULL);
    done(&s);
    int below = zebkit_pdf_hook_layer(pdf, ZEBKIT_PDF_LAYER_BELOW, layer_below, NULL, NULL);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 10, 10), 0, 255, 0), "a layer below", NULL);
    check_text(near_color(at(&s, 120, 120), 0, 0, 255) || near_color(at(&s, 150, 150), 0, 0, 255), "under the content", NULL);
    done(&s);
    /* At a scale, turned, as a tile: still in page space. */
    zebkit_pdf_hook_remove(below);
    shot big = render_at(pdf, 0, 2);
    {
        /* Page (55, 55) at 2 pixels a point. */
        const uint8_t *p = big.rgba + (size_t)(big.height - 111) * big.width * 4 + 110 * 4;
        check_text(near_color(p, 255, 128, 128), "at scale 2", NULL);
    }
    done(&big);
    zebkit_pdf_render_options options = {0};
    options.scale = 1;
    options.rotation = 90;
    options.background = 0xffffffff;
    int w = 0, h = 0;
    uint8_t *turned = zebkit_pdf_render(pdf, 0, &options, &w, &h, NULL);
    check_text(turned && w == 200 && h == 300, "turned", NULL);
    /* Clockwise: the page's lower left goes to the upper left. */
    if (turned) {
        const uint8_t *p = turned + (size_t)55 * w * 4 + 55 * 4;
        check_text(near_color(p, 255, 128, 128), "a turned render's layer stays on the page", NULL);
    }
    zebkit_free(turned);
    options = (zebkit_pdf_render_options){0};
    options.scale = 1;
    options.x = 40;
    options.y = 100;
    options.width = 60;
    options.height = 60;
    options.background = 0xffffffff;
    uint8_t *tile = zebkit_pdf_render(pdf, 0, &options, &w, &h, NULL);
    check_text(tile && w == 60, "a tile", NULL);
    if (tile) {
        /* Page (55, 55): row 200 - 55 = 145 of the page, 45 of the tile. */
        const uint8_t *p = tile + (size_t)44 * w * 4 + 15 * 4;
        check_text(near_color(p, 255, 128, 128), "a tile's layer stays on the page", NULL);
    }
    zebkit_free(tile);
    zebkit_pdf_hook_remove(above);
    s = render(pdf, 0);
    check_text(white(at(&s, 55, 55)), "the hook removed, gone", NULL);
    done(&s);
    zebkit_pdf_close(pdf);
}

static zebkit_pdf_trust trust_it(zebkit_pdf *pdf, const char *signature, const uint8_t *contents, size_t size, void *data) {
    (void)pdf;
    check_text(signature && strstr(signature, "\"field\":") && contents && size > 0, "a signature and its PKCS#7", NULL);
    return (zebkit_pdf_trust)(intptr_t)data;
}

static uint8_t *give_font(zebkit_pdf *pdf, const char *family, size_t *size, void *data) {
    (void)pdf;
    int *asked = data;
    if (strcmp(family, "Boxes") && *family)
        return NULL;
    (*asked)++;
    *size = font_size;
    return zebkit_pdf_bytes(font_bytes, font_size);
}

static bool take_action(zebkit_pdf *pdf, const char *action, void *data) {
    (void)pdf;
    snprintf(data, 256, "%s", action);
    return strstr(action, "\"javascript\"") != NULL;
}

static char *double_points(zebkit_pdf *pdf, const char *tool, const char *points, void *data) {
    (void)pdf; (void)tool; (void)points;
    int *asked = data;
    (*asked)++;
    return zebkit_pdf_string("[[1,2],[3,4]]");
}

static char *wrong_points(zebkit_pdf *pdf, const char *tool, const char *points, void *data) {
    (void)pdf; (void)tool; (void)points; (void)data;
    return zebkit_pdf_string("{\"not\":\"points\"}");
}

static void test_other_providers(void) {
    /* Fonts for free text: asked for a family, and for what standard fonts lack. */
    zebkit_pdf *pdf = blank(300, 200);
    int asked = 0;
    int hook = zebkit_pdf_hook_font(pdf, give_font, &asked, NULL);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[10,100,290,190],\"contents\":\"XX\",\"font\":\"Boxes\",\"fontSize\":20,\"textColor\":[255,0,0]}", NULL);
    check_text(asked == 1, "a font asked for", NULL);
    shot s = render(pdf, 0);
    check_text(near_color(at(&s, 30, 170), 255, 0, 0), "and used", NULL);
    done(&s);
    zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"freeText\",\"rect\":[10,10,290,90],\"contents\":\"\xe4\xb8\xad\xe6\x96\x87\",\"fontSize\":20,\"textColor\":[0,0,255]}", NULL);
    check_text(asked == 2, "and for what the standard fonts lack", NULL);
    s = render(pdf, 0);
    check_text(near_color(at(&s, 25, 70), 0, 0, 255), "CJK, in that font", NULL);
    done(&s);
    zebkit_pdf_hook_remove(hook);
    zebkit_pdf_close(pdf);

    /* Trust in a signer. */
    static const char *const signed_form =
        "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [5 0 R] /SigFlags 3 >> >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots [5 0 R] >> endobj\n"
        "5 0 obj << /Type /Annot /Subtype /Widget /FT /Sig /T (S) /Rect [0 0 0 0] /P 3 0 R /V 7 0 R >> endobj\n"
        "7 0 obj << /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 1 2 3] /Contents <3082> >> endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    pdf = zebkit_pdf_open(signed_form, strlen(signed_form), NULL, NULL);
    char *signatures = zebkit_pdf_signatures(pdf, NULL);
    check_text(signatures && !strstr(signatures, "\"trusted\""), "says nothing without a hook", signatures);
    zebkit_free(signatures);
    hook = zebkit_pdf_hook_trust(pdf, trust_it, (void *)(intptr_t)ZEBKIT_PDF_TRUSTED, NULL);
    signatures = zebkit_pdf_signatures(pdf, NULL);
    check_text(signatures && strstr(signatures, "\"trusted\":true"), "trusted by a hook", signatures);
    zebkit_free(signatures);
    zebkit_pdf_hook_remove(hook);
    hook = zebkit_pdf_hook_trust(pdf, trust_it, (void *)(intptr_t)ZEBKIT_PDF_UNTRUSTED, NULL);
    signatures = zebkit_pdf_signatures(pdf, NULL);
    check_text(signatures && strstr(signatures, "\"trusted\":false"), "not trusted by a hook", signatures);
    zebkit_free(signatures);
    zebkit_pdf_hook_remove(hook);
    hook = zebkit_pdf_hook_trust(pdf, trust_it, (void *)(intptr_t)ZEBKIT_PDF_TRUST_UNKNOWN, NULL);
    signatures = zebkit_pdf_signatures(pdf, NULL);
    check_text(signatures && !strstr(signatures, "\"trusted\""), "no opinion", signatures);
    zebkit_free(signatures);
    zebkit_pdf_hook_remove(hook);
    zebkit_pdf_close(pdf);

    /* Actions. */
    static const char *const actions =
        "%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
        "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
        "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Annots ["
        "<< /Type /Annot /Subtype /Link /Rect [0 0 9 9] /A << /S /Named /N /NextPage >> >> "
        "<< /Type /Annot /Subtype /Link /Rect [0 0 9 9] /A << /S /Launch /F (run.exe) >> >> "
        "<< /Type /Annot /Subtype /Link /Rect [0 0 9 9] /A << /S /JavaScript /JS (app.alert(1)) >> >> "
        "<< /Type /Annot /Subtype /Link /Rect [0 0 9 9] /A << /S /SubmitForm >> >> "
        "<< /Type /Annot /Subtype /Link /Rect [0 0 9 9] /Dest [3 0 R /Fit] >> "
        "<< /Type /Annot /Subtype /Square /Rect [0 0 9 9] >> ] >> endobj\n"
        "trailer << /Root 1 0 R >>\n%%EOF\n";
    pdf = zebkit_pdf_open(actions, strlen(actions), NULL, NULL);
    char seen[256] = "";
    hook = zebkit_pdf_hook_action(pdf, take_action, seen, NULL);
    static const struct {
        int index;
        const char *kind, *value, *handled;
    } expected[] = {
        {0, "named", "NextPage", "false"}, {1, "launch", "run.exe", "false"}, {2, "javascript", "app.alert(1)", "true"},
        {3, "other", "SubmitForm", "false"}, {4, "goto", "\"page\":0", "false"}, {5, "none", "", "false"},
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        char *action = zebkit_pdf_annotation_action(pdf, 0, expected[i].index, NULL);
        char kind[64], handled[64];
        snprintf(kind, sizeof(kind), "\"kind\":\"%s\"", expected[i].kind);
        snprintf(handled, sizeof(handled), "\"handled\":%s", expected[i].handled);
        check_text(action && strstr(action, kind) && strstr(action, expected[i].value) && strstr(action, handled), "an action", action);
        check_text(strstr(seen, expected[i].kind) != NULL, "shown to the hook", seen);
        zebkit_free(action);
    }
    zebkit_pdf_hook_remove(hook);
    zebkit_pdf_close(pdf);

    /* Input filters. */
    char *error = NULL;
    char *out = zebkit_pdf_filter_input(NULL, "ink", "[[0,0],[10,0],[10,10]]", "{\"smooth\":1}", &error);
    check_text(out && strstr(out, "[7.5,0.0]") && strstr(out, "[10.0,2.5]") && strstr(out, "[10.0,10.0]") && strstr(out, "[0.0,0.0]"), "smoothing keeps its ends", out);
    zebkit_free(out);
    out = zebkit_pdf_filter_input(NULL, "ink", "[[0,0],[5,0.1],[10,0]]", "{\"simplify\":1}", NULL);
    check_text(out && !strcmp(out, "[[0.0,0.0],[10.0,0.0]]"), "simplified", out);
    zebkit_free(out);
    out = zebkit_pdf_filter_input(NULL, "line", "[[12,18],[31,44]]", "{\"snapGrid\":10}", NULL);
    check_text(out && !strcmp(out, "[[10.0,20.0],[30.0,40.0]]"), "snapped to a grid", out);
    zebkit_free(out);
    out = zebkit_pdf_filter_input(NULL, "line", "[[0,0],[10,1]]", "{\"snapAngle\":45}", NULL);
    check_text(out && strstr(out, "[[0.0,0.0],[10.04"), "snapped to an angle", out);
    zebkit_free(out);
    out = zebkit_pdf_filter_input(NULL, "square", "[[0,0],[10,-3]]", "{\"constrain\":\"square\"}", NULL);
    check_text(out && !strcmp(out, "[[0.0,0.0],[10.0,-10.0]]"), "constrained to a square", out);
    zebkit_free(out);
    static const struct {
        const char *points, *options;
    } bad[] = {
        {"not json", NULL},
        {"{}", NULL},
        {"[[1]]", NULL},
        {"[[\"a\",\"b\"]]", NULL},
        {"[[1,2]]", "not json"},
        {"[[1,2]]", "{\"nothing\":1}"},
        {"[[1,2]]", "{\"smooth\":99}"},
        {"[[1,2]]", "{\"smooth\":\"x\"}"},
        {"[[1,2]]", "{\"snapGrid\":-1}"},
        {"[[1,2]]", "{\"constrain\":\"round\"}"},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        out = zebkit_pdf_filter_input(NULL, "ink", bad[i].points, bad[i].options, &error);
        refused(!out, &error, bad[i].points);
    }
    int filtered = 0;
    pdf = blank(10, 10);
    hook = zebkit_pdf_hook_input(pdf, "ink", double_points, &filtered, NULL);
    out = zebkit_pdf_filter_input(pdf, "ink", "[[9,9]]", NULL, NULL);
    check_text(out && !strcmp(out, "[[1.0,2.0],[3.0,4.0]]") && filtered == 1, "a filter of the application's", out);
    zebkit_free(out);
    out = zebkit_pdf_filter_input(pdf, "pen", "[[9,9]]", NULL, NULL);
    check_text(out && filtered == 1, "only for its tool", out);
    zebkit_free(out);
    zebkit_pdf_hook_remove(hook);
    hook = zebkit_pdf_hook_input(pdf, "ink", wrong_points, NULL, NULL);
    out = zebkit_pdf_filter_input(pdf, "ink", "[[9,9]]", NULL, &error);
    refused(!out, &error, "a filter's own points are checked");
    zebkit_pdf_hook_remove(hook);
    zebkit_pdf_close(pdf);

    /* A kind's rules. */
    zebkit_pdf_kind kind = {0};
    error = NULL;
    refused(zebkit_pdf_hook_kind(NULL, &kind, &error) < 0, &error, "a kind of no name");
    kind.name = "App_K";
    kind.prefix = "App_";
    kind.schema = "not json";
    refused(zebkit_pdf_hook_kind(NULL, &kind, &error) < 0, &error, "a schema that is not JSON");
    kind.schema = NULL;
    kind.name = "Bad Name/";
    refused(zebkit_pdf_hook_kind(NULL, &kind, &error) < 0, &error, "a name that cannot be in a file");
    kind.name = "Other_K";
    refused(zebkit_pdf_hook_kind(NULL, &kind, &error) < 0, &error, "a name not of its prefix");
    /* A standard subtype beneath it: other readers know what it is. */
    pdf = blank(100, 100);
    kind.name = "Seal";
    kind.prefix = "App_";
    kind.subtype = "Stamp";
    int id = zebkit_pdf_hook_kind(pdf, &kind, &error);
    check_text(id > 0, "a kind on a standard subtype", error);
    zebkit_free(error);
    check_text(zebkit_pdf_annotation_add(pdf, 0, "{\"type\":\"Seal\",\"rect\":[1,1,9,9],\"data\":{\"a\":1}}", NULL) == 0 &&
              listed(pdf, 0, "\"type\":\"Seal\"") && listed(pdf, 0, "\"Subtype\":\"/Stamp\"") && listed(pdf, 0, "\"App_Kind\":\"/Seal\"") && listed(pdf, 0, "\"data\":{\"a\":1}"),
          "kept as a stamp", NULL);
    zebkit_pdf_close(pdf);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: pdf_hooks <font.ttf>\n");
        return 2;
    }
    font_bytes = read_file(argv[1], &font_size);
    check_text(font_bytes != NULL, "the font is there", argv[1]);
    test_plugin();
    test_events();
    test_changes();
    test_reentrancy();
    test_more_reentrancy();
    test_providers();
    test_layers();
    test_other_providers();
    free(font_bytes);
    return summary();
}
