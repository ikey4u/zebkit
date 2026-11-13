/* The hooks of <zebkit/hooks.h> through the C API; run by hook_policies.py:
 * the modes of ZEBKIT_HOOK_RUNS one after the other in one engine, each on a
 * view of its own, or the one of ZEBKIT_HOOK_MODE. The first that fails ends
 * the program (FAIL on standard error, exit 1). */
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <zebkit/hooks.h>

static const char *MODE, *BASE, *TLS, *FILES, *DOWNLOADS;
static char *CERT, *KEY, *OTHER_KEY;
static zebkit_view *view;
static int closed, finished, failed, told, asked;
static char last_told[131072];

#define FAIL(...) do { fprintf(stderr, "FAIL %s line %d: ", MODE, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define CHECK(c) do { if (!(c)) FAIL("%s", #c); } while (0)
#define IS(name) (!strcmp(MODE, name))
#define WAIT(c) do { int t_ = 0; while (!(c)) { if (++t_ > 15000) FAIL("timed out: %s", #c); pump_once(); } } while (0)

static void pump_once(void) {
    CHECK(zebkit_pump());
    struct timespec nap = {0, 1000000};
    nanosleep(&nap, NULL);
}
static void pump_for(int ms) { for (int i = 0; i < ms; ++i) pump_once(); }
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) FAIL("cannot read %s", path);
    size_t size = 0, used = 0;
    char *buffer = NULL;
    do {
        if (used + 1 >= size) buffer = realloc(buffer, size = size ? size * 2 : 65536);
        used += fread(buffer + used, 1, size - used - 1, f);
    } while (!feof(f) && !ferror(f));
    fclose(f);
    buffer[used] = 0;
    return buffer;
}
static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}
struct eval { int done; char *json; char *error; };
static void eval_done(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v;
    struct eval *e = data;
    e->json = json ? strdup(json) : NULL;
    e->error = error ? strdup(error) : NULL;
    e->done = 1;
}
static char *ev_in(zebkit_view *in, const char *script) {
    struct eval e = {0};
    zebkit_view_evaluate(in, script, eval_done, &e);
    WAIT(e.done);
    if (e.error) FAIL("evaluate %s: %s", script, e.error);
    return e.json;
}
static char *ev(const char *script) { return ev_in(view, script); }
static int ev_in_is(zebkit_view *in, const char *script, const char *value) {
    char *json = ev_in(in, script);
    int same = json && !strcmp(json, value);
    free(json);
    return same;
}
static struct eval pending_eval;
static void ev_later(const char *script) {
    memset(&pending_eval, 0, sizeof pending_eval);
    zebkit_view_evaluate(view, script, eval_done, &pending_eval);
}
static int ev_is(const char *script, const char *value) {
    char *json = ev(script);
    int same = json && !strcmp(json, value);
    free(json);
    return same;
}
static void on_close(zebkit_view *v, void *d) { (void)v; (void)d; closed = 1; }
static void on_load(zebkit_view *v, zebkit_load_event event, const char *url, void *d) {
    (void)v; (void)url; (void)d;
    if (event == ZEBKIT_LOAD_FINISHED) finished = 1;
    if (event == ZEBKIT_LOAD_FAILED) failed = 1;
}
static void load(const char *base, const char *path) {
    static char url[4096];
    snprintf(url, sizeof url, "%s%s", base, path);
    finished = failed = 0;
    zebkit_view_load_url(view, url);
}
static void open_page(void) {
    load(BASE, "/page");
    for (int t = 0;; ++t) {
        char *url = zebkit_view_get_url(view);
        int here = url && strstr(url, "/page") != NULL;
        zebkit_free(url);
        if (here && finished) {
            /* The first document's own "finished" may be what was seen. */
            char *state = ev("location.pathname + ' ' + document.readyState");
            int ready = strstr(state, "/page complete") != NULL;
            free(state);
            if (ready) return;
        }
        if (t > 15000) FAIL("page did not load");
        pump_once();
    }
}
static void note(zebkit_view *v, const char *json, void *d) {
    (void)v; (void)d;
    ++told;
    snprintf(last_told, sizeof last_told, "%s", json);
}
static int has(const char *text) { return strstr(last_told, text) != NULL; }
static void wait_title(const char *title) {
    for (int t = 0;; ++t) {
        char *now = zebkit_view_get_title(view);
        int same = now && !strcmp(now, title);
        zebkit_free(now);
        if (same) return;
        if (t > 15000) FAIL("title never %s", title);
        pump_once();
    }
}

/* ---- links to other programs */
static void external_told(zebkit_view *v, const char *json, void *d) {
    note(v, json, d);
    if (IS("external-close")) zebkit_view_close(v);
}
static void external(void) {
    open_page();
    if (!IS("external-none")) zebkit_view_on_external_protocol(view, external_told, NULL);
    free(ev("location.href = 'zoommtg://join?x=1'; 1"));
    if (IS("external-none")) {
        pump_for(300);
        CHECK(ev_is("location.pathname", "\"/page\""));
        zebkit_view_close(view);
        return;
    }
    WAIT(told);
    CHECK(has("\"scheme\":\"zoommtg\"") && has("\"mainFrame\":true"));
    if (IS("external-close")) { WAIT(closed); return; }
    CHECK(ev_is("location.pathname", "\"/page\""));
    zebkit_view_on_external_protocol(view, NULL, NULL);
    free(ev("location.href = 'mailto:a@example.com'; 1"));
    pump_for(300);
    CHECK(told == 1);
    zebkit_view_close(view);
}

/* ---- downloads */
static zebkit_download_request *download_request;
static char target[4096];
static int download_complete, download_cancelled, download_saved, download_timeout, download_replaced, download_in_profile;
static void download_progress(zebkit_view *v, const char *json, void *d) {
    (void)v; (void)d;
    if (strstr(json, "\"state\":\"complete\"")) {
        download_complete = 1;
        if (strstr(json, "Downloads/report.bin") || strstr(json, "Downloads\\\\report.bin")) download_in_profile = 1;
    }
    if (strstr(json, "\"state\":\"cancelled\"") && strstr(json, "\"done\":true")) download_cancelled = 1;
}
/* What the function decided, or why it did not: the outcome every asked hook has. */
static void download_result(zebkit_view *v, const char *json, void *d) {
    (void)v; (void)d;
    if (strstr(json, "\"state\":\"saved\"") && strstr(json, "\"reason\":\"answered\"")) download_saved = 1;
    if (strstr(json, "\"state\":\"cancelled\"") && strstr(json, "\"reason\":\"timeout\"")) download_timeout = 1;
    if (strstr(json, "\"state\":\"cancelled\"") && strstr(json, "\"reason\":\"handler-changed\"")) download_replaced = 1;
}
static void download_asked(zebkit_view *v, zebkit_download_request *request, const char *json, void *d) {
    (void)d;
    download_request = request;
    ++asked;
    CHECK(strstr(json, "\"suggestedName\":\"report.bin\"") && strstr(json, "\"totalBytes\":65536"));
    if (IS("download-save")) {
        char *error = NULL;
        CHECK(!zebkit_download_respond(request, ZEBKIT_DOWNLOAD_SAVE, "relative.bin", &error) && error);
        zebkit_free(error);
        CHECK(!zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, target, NULL));
        CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_SAVE, target, NULL));
        CHECK(!zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, NULL, NULL));
        CHECK(!zebkit_download_request_info(request));
    } else if (IS("download-cancel")) {
        CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, NULL, NULL));
    } else if (IS("download-close")) {
        zebkit_view_close(v);
    }
}
static void download(void) {
    snprintf(target, sizeof target, "%s/saved.bin", DOWNLOADS);
    FILE *f = fopen(target, "wb"); CHECK(f); fputs("old", f); fclose(f);
    open_page();
    zebkit_view_on_download_progress(view, download_progress, NULL);
    zebkit_view_on_download_result(view, download_result, NULL);
    if (!IS("download-default")) zebkit_view_on_download(view, download_asked, NULL);
    if (IS("download-timeout")) {
        CHECK(!zebkit_view_set_download_timeout(view, 0) && !zebkit_view_set_download_timeout(view, 300001));
        CHECK(zebkit_view_set_download_timeout(view, 50));
    }
    free(ev("location.href = '/download.bin'; 1"));
    if (IS("download-default")) {
        WAIT(download_complete);
        CHECK(download_in_profile);
        zebkit_view_close(view);
        return;
    }
    WAIT(asked);
    if (IS("download-save")) {
        WAIT(download_complete);
        WAIT(download_saved);
        CHECK(file_size(target) == 65536);
    } else if (IS("download-cancel")) {
        WAIT(download_cancelled);
        CHECK(file_size(target) == 3);
    } else if (IS("download-timeout")) {
        WAIT(download_timeout);
        CHECK(!zebkit_download_respond(download_request, ZEBKIT_DOWNLOAD_SAVE, target, NULL));
        CHECK(file_size(target) == 3);
    } else if (IS("download-close")) {
        WAIT(closed);
        CHECK(!zebkit_download_request_info(download_request));
        return;
    } else if (IS("download-pending-close")) {
        zebkit_view_close(view);
        WAIT(closed);
        CHECK(!zebkit_download_respond(download_request, ZEBKIT_DOWNLOAD_SAVE, target, NULL));
        CHECK(file_size(target) == 3);
        return;
    } else if (IS("download-replace")) {
        zebkit_view_on_download(view, NULL, NULL);
        CHECK(!zebkit_download_request_info(download_request));
        WAIT(download_replaced);
    }
    zebkit_view_close(view);
}

/* ---- client certificates */
static zebkit_client_certificate_request *cert_request;
static void server_certificate(zebkit_view *v, zebkit_certificate_request *request, const char *json, void *d) {
    (void)v; (void)json; (void)d;
    CHECK(zebkit_certificate_respond(request, true));
}
static void cert_asked(zebkit_view *v, zebkit_client_certificate_request *request, const char *json, void *d) {
    (void)d;
    cert_request = request;
    ++asked;
    CHECK(strstr(json, "\"host\":\"localhost\""));
    if (IS("cert-provide")) {
        char *error = NULL;
        CHECK(!zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_PROVIDE, "junk", KEY, &error));
        zebkit_free(error); error = NULL;
        CHECK(!zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_PROVIDE, CERT, "junk", &error));
        zebkit_free(error); error = NULL;
        CHECK(!zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_PROVIDE, CERT, OTHER_KEY, &error) && error && strstr(error, "not the certificate"));
        zebkit_free(error);
        CHECK(zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_PROVIDE, CERT, KEY, NULL));
        CHECK(!zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_NONE, NULL, NULL, NULL));
    } else if (IS("cert-none")) {
        CHECK(zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_NONE, NULL, NULL, NULL));
    } else if (IS("cert-cancel")) {
        CHECK(zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_CANCEL, NULL, NULL, NULL));
    } else if (IS("cert-close")) {
        zebkit_view_close(v);
    } else if (IS("cert-store")) {
        /* The system's store's identity, chosen; its key stays there. */
        const char *found = strstr(json, "\"subject\":\"zebkit-client\"");
        CHECK(found);
        const char *object = found;
        while (object > json && *object != '{') --object;
        const char *index = strstr(object, "\"index\":");
        CHECK(index && index < found);
        size_t chosen = (size_t)strtoul(index + 8, NULL, 10);
        CHECK(strstr(object, "\"certificate\":\"-----BEGIN CERTIFICATE-----"));
        char *error = NULL;
        CHECK(!zebkit_client_certificate_choose(request, 1000, &error) && error);
        zebkit_free(error);
        CHECK(zebkit_client_certificate_choose(request, chosen, NULL));
    } else if (IS("cert-store-off")) {
        /* Without the engine setting, the store is not offered. */
        CHECK(strstr(json, "\"identities\":[]"));
        CHECK(zebkit_client_certificate_respond(request, ZEBKIT_CLIENT_CERTIFICATE_NONE, NULL, NULL, NULL));
    }
}
static void certificate(void) {
    zebkit_view_on_certificate(view, server_certificate, NULL);
    zebkit_view_on_client_certificate_result(view, note, NULL);
    if (IS("cert-store")) CHECK(zebkit_set_engine_settings("{\"clientCertificateStores\":true}", NULL));
    if (!IS("cert-default")) zebkit_view_on_client_certificate(view, cert_asked, NULL);
    if (IS("cert-timeout")) CHECK(zebkit_view_set_client_certificate_timeout(view, 50));
    load(TLS, "/secure");
    if (IS("cert-provide") || IS("cert-store")) wait_title("cert:zebkit-client");
    else if (IS("cert-none") || IS("cert-default") || IS("cert-store-off")) wait_title("cert:none");
    else if (IS("cert-cancel") || IS("cert-timeout")) { WAIT(failed); WAIT(told); CHECK(has("\"state\":\"cancelled\"")); }
    else if (IS("cert-close")) { WAIT(closed); CHECK(!zebkit_client_certificate_request_info(cert_request)); return; }
    else if (IS("cert-pending-close")) {
        WAIT(asked);
        zebkit_view_close(view);
        WAIT(closed);
        CHECK(!zebkit_client_certificate_respond(cert_request, ZEBKIT_CLIENT_CERTIFICATE_NONE, NULL, NULL, NULL));
        return;
    } else if (IS("cert-replace")) {
        WAIT(asked);
        zebkit_view_on_client_certificate(view, NULL, NULL);
        CHECK(!zebkit_client_certificate_request_info(cert_request));
        WAIT(failed); WAIT(told); CHECK(has("\"reason\":\"handler-changed\""));
    }
    zebkit_view_close(view);
}

/* ---- isolated worlds */
static void world(void) {
    char *error = NULL;
    CHECK(zebkit_view_add_world_script(view, "zebkit-test", "var worldVar = 1; addEventListener('zkping', () => document.documentElement.setAttribute('data-all', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));", false, NULL));
    CHECK(zebkit_view_add_world_script(view, "zebkit-main", "addEventListener('zkping', () => document.documentElement.setAttribute('data-main', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));", true, NULL));
    CHECK(zebkit_view_add_world_script(view, NULL, "window.mainWorldVar = 1;", true, NULL));
    static char lengthy[258];
    memset(lengthy, 'x', 257);
    CHECK(!zebkit_view_add_world_script(view, lengthy, "1", false, &error) && error);
    zebkit_free(error);
    load(BASE, "/frames");
    WAIT(finished);
    for (int t = 0; !ev_is("window.reports && reports.length", "2"); ++t) {
        if (t > 3000) FAIL("frames did not report");
        pump_for(5);
    }
    CHECK(ev_is("JSON.stringify(report)", "\"{\\\"all\\\":\\\"isolated\\\",\\\"main\\\":\\\"isolated\\\",\\\"global\\\":\\\"undefined\\\",\\\"mainWorld\\\":\\\"number\\\"}\""));
    CHECK(ev_is("reports.every(r => r.all === 'isolated' && r.main === null && r.global === 'undefined' && r.mainWorld === 'undefined')", "true"));
    zebkit_view_close(view);
}

/* ---- file choosers */
static zebkit_file_chooser_request *file_request;
static void file_asked(zebkit_view *v, zebkit_file_chooser_request *request, const char *json, void *d) {
    (void)d;
    file_request = request;
    ++asked;
    static char one[4096], two[4096], folder[4096];
    snprintf(one, sizeof one, "%s/one.txt", FILES);
    snprintf(two, sizeof two, "%s/two.txt", FILES);
    snprintf(folder, sizeof folder, "%s/folder", FILES);
    const char *both[] = {one, two};
    const char *relative[] = {"relative.txt"};
    if (IS("file-open")) {
        CHECK(strstr(json, "\"mode\":\"open\""));
        CHECK(!zebkit_file_chooser_respond(request, relative, 1, NULL));
        CHECK(!zebkit_file_chooser_respond(request, both, 2, NULL));
        CHECK(zebkit_file_chooser_respond(request, both, 1, NULL));
        CHECK(!zebkit_file_chooser_respond(request, NULL, 0, NULL));
    } else if (IS("file-multiple")) {
        CHECK(strstr(json, "\"mode\":\"open-multiple\""));
        CHECK(zebkit_file_chooser_respond(request, both, 2, NULL));
    } else if (IS("file-folder")) {
        const char *dir[] = {folder};
        CHECK(strstr(json, "\"mode\":\"upload-folder\""));
        CHECK(zebkit_file_chooser_respond(request, dir, 1, NULL));
    } else if (IS("file-cancel")) {
        CHECK(zebkit_file_chooser_respond(request, NULL, 0, NULL));
    } else if (IS("file-close")) {
        zebkit_view_close(v);
    }
}
static void chooser(void) {
    open_page();
    zebkit_view_on_file_chooser_result(view, note, NULL);
    zebkit_view_on_file_chooser(view, file_asked, NULL);
    if (IS("file-timeout")) CHECK(zebkit_view_set_file_chooser_timeout(view, 50));
    const char *element = IS("file-multiple") ? "m" : IS("file-folder") ? "d" : "i";
    char script[256];
    snprintf(script, sizeof script, "%s.addEventListener('cancel', () => window.cancelled = 1); %s.click(); 1", element, element);
    if (IS("file-close")) {
        /* The handler's close can come before the reply to the click, which then fails as
         * the page goes, as CDP fails a detached target's commands. */
        ev_later(script);
        WAIT(pending_eval.done);
        /* What a call still waiting for its reply is told then (src/engine/view.cc). */
        if (pending_eval.error && strcmp(pending_eval.error, "the page target was detached")
            && strcmp(pending_eval.error, "the page was closed") && strcmp(pending_eval.error, "the view was closed"))
            FAIL("evaluate %s: %s", script, pending_eval.error);
        free(pending_eval.json);
        free(pending_eval.error);
    } else {
        free(ev(script));
    }
    WAIT(asked);
    int t = 0;
    if (IS("file-open")) {
        while (!ev_is("i.files.length", "1")) if (++t > 3000) FAIL("no file"); else pump_for(2);
        CHECK(ev_is("i.files[0].text()", "\"first file\""));
    } else if (IS("file-multiple")) {
        while (!ev_is("m.files.length", "2")) if (++t > 3000) FAIL("no files"); else pump_for(2);
        CHECK(ev_is("m.files[1].text()", "\"second file\""));
    } else if (IS("file-folder")) {
        while (!ev_is("d.files.length", "2")) if (++t > 3000) FAIL("no folder"); else pump_for(2);
        CHECK(ev_is("[...d.files].map(f => f.webkitRelativePath).sort().join()", "\"folder/a.txt,folder/sub/b.txt\""));
    } else if (IS("file-cancel") || IS("file-timeout")) {
        WAIT(told);
        CHECK(has("\"state\":\"cancelled\"") && has(IS("file-timeout") ? "\"reason\":\"timeout\"" : "\"reason\":\"answered\""));
        while (!ev_is("window.cancelled === 1", "true")) if (++t > 3000) FAIL("page not told"); else pump_for(2);
    } else if (IS("file-close")) {
        WAIT(closed);
        CHECK(!zebkit_file_chooser_request_info(file_request));
        return;
    } else if (IS("file-pending-close")) {
        zebkit_view_close(view);
        WAIT(closed);
        CHECK(!zebkit_file_chooser_respond(file_request, NULL, 0, NULL));
        return;
    } else if (IS("file-replace")) {
        zebkit_view_on_file_chooser(view, NULL, NULL);
        CHECK(!zebkit_file_chooser_request_info(file_request));
        WAIT(told); CHECK(has("\"reason\":\"handler-changed\""));
    }
    zebkit_view_close(view);
}

/* ---- camera and microphone */
static zebkit_media_access_request *media_request;
static void media_asked(zebkit_view *v, zebkit_media_access_request *request, const char *json, void *d) {
    (void)d;
    media_request = request;
    ++asked;
    CHECK(strstr(json, "\"video\":true") && strstr(json, "\"audio\":false"));
    if (IS("media-allow")) {
        CHECK(zebkit_media_access_respond(request, true, NULL, NULL));
        CHECK(!zebkit_media_access_respond(request, false, NULL, NULL));
    } else if (IS("media-deny")) {
        CHECK(!zebkit_media_access_respond(request, false, NULL, "device"));
        CHECK(zebkit_media_access_respond(request, false, NULL, NULL));
    } else if (IS("media-close")) {
        zebkit_view_close(v);
    }
}
static void media(void) {
    open_page();
    zebkit_view_on_media_access_result(view, note, NULL);
    zebkit_view_on_media_access(view, media_asked, NULL);
    if (IS("media-timeout")) CHECK(zebkit_view_set_media_access_timeout(view, 50));
    ev_later("navigator.mediaDevices.getUserMedia({video: true}).then(s => { const n = s.getTracks().length; s.getTracks().forEach(t => t.stop()); return 'ok' + n; }, e => e.name)");
    WAIT(asked);
    if (IS("media-allow")) {
        WAIT(pending_eval.done);
        CHECK(pending_eval.json && !strcmp(pending_eval.json, "\"ok1\""));
    } else if (IS("media-deny") || IS("media-timeout")) {
        WAIT(pending_eval.done);
        CHECK(pending_eval.json && !strcmp(pending_eval.json, "\"NotAllowedError\""));
        WAIT(told); CHECK(has("\"state\":\"denied\""));
    } else if (IS("media-close")) {
        WAIT(closed);
        CHECK(!zebkit_media_access_request_info(media_request));
        return;
    } else if (IS("media-pending-close")) {
        zebkit_view_close(view);
        WAIT(closed);
        CHECK(!zebkit_media_access_respond(media_request, true, NULL, NULL));
        return;
    } else if (IS("media-replace")) {
        zebkit_view_on_media_access(view, NULL, NULL);
        CHECK(!zebkit_media_access_request_info(media_request));
        WAIT(pending_eval.done);
        CHECK(pending_eval.json && !strcmp(pending_eval.json, "\"NotAllowedError\""));
    }
    zebkit_view_close(view);
}

/* ---- keys, menu, fullscreen */
static void keys(void) {
    char *error = NULL;
    CHECK(!zebkit_view_set_reserved_keys(view, "[\"Ctrl+Nonsense\"]", &error) && error);
    zebkit_free(error); error = NULL;
    CHECK(!zebkit_view_set_reserved_keys(view, "{}", &error) && error);
    zebkit_free(error);
    CHECK(zebkit_view_set_reserved_keys(view, "[\"Ctrl+KeyK\"]", NULL));
    zebkit_view_on_reserved_key(view, note, NULL);
    load(BASE, "/input");
    WAIT(told);
    CHECK(has("\"combo\":\"Ctrl+KeyK\"") && has("\"code\":\"KeyK\""));
    int t = 0;
    while (!ev_is("keys.length > 0", "true")) if (++t > 3000) FAIL("no other key"); else pump_for(2);
    CHECK(ev_is("keys.every(k => k === 'KeyB')", "true"));
    zebkit_view_close(view);
}
static void menu(void) {
    zebkit_view_on_context_menu(view, note, NULL);
    load(BASE, "/input");
    WAIT(told);
    CHECK(has("\"x\":20") && has("\"y\":30") && has("/page\""));
    CHECK(ev_is("window.menu", "1"));
    zebkit_view_close(view);
}
/* A gated call, once the evaluation's user activation has reached the
   browser: it goes there on a pipe of its own, and the call consumes it. */
#define ACTIVATED(script) "new Promise(r => setTimeout(r, 200)).then(() => " script ")"
#define FULL ACTIVATED("document.documentElement.requestFullscreen()") ".then(() => 'in', e => e.name)"
static void fullscreen(void) {
    open_page();
    zebkit_view_on_fullscreen(view, note, NULL);
    /* Since Chromium 157 only a view with focus goes fullscreen, as when a
       person clicks the page. */
    zebkit_view_focus(view);
    CHECK(!zebkit_view_set_fullscreen_policy(view, (zebkit_fullscreen_policy)7));
    if (IS("fullscreen-deny")) {
        CHECK(zebkit_view_set_fullscreen_policy(view, ZEBKIT_FULLSCREEN_DENY));
        CHECK(ev_is(FULL, "\"TypeError\""));
        pump_for(200);
        CHECK(!told);
        CHECK(zebkit_view_set_fullscreen_policy(view, ZEBKIT_FULLSCREEN_ALLOW));
    }
    CHECK(ev_is(FULL, "\"in\""));
    WAIT(told && has("\"entered\":true"));
    if (IS("fullscreen-deny")) CHECK(zebkit_view_set_fullscreen_policy(view, ZEBKIT_FULLSCREEN_DENY));
    else free(ev("document.exitFullscreen().then(() => 1)"));
    WAIT(has("\"entered\":false"));
    zebkit_view_close(view);
}

/* ---- network */
static zebkit_network_request *net_request;
static int net_timeout, net_replaced, net_request_told, net_response_told, net_finished_told, net_failed_told;
static int body_ends, body_truncated, body_second, body_end_late;
static long long body_bytes;
static char net_response_json[65536], body_id[64];
static void net_event(zebkit_view *v, const char *json, void *d) {
    (void)v; (void)d;
    if (strstr(json, "\"reason\":\"timeout\"")) net_timeout = 1;
    if (strstr(json, "\"reason\":\"handler-changed\"")) net_replaced = 1;
    if (strstr(json, "\"kind\":\"request\"")) net_request_told = 1;
    if (strstr(json, "\"kind\":\"response\"")) { net_response_told = 1; snprintf(net_response_json, sizeof net_response_json, "%s", json); }
    if (strstr(json, "\"kind\":\"finished\"")) { net_finished_told = 1; if (!body_ends) body_end_late = 1; }
    if (strstr(json, "\"kind\":\"failed\"")) { net_failed_told = 1; if (!body_ends) body_end_late = 1; }
}
#define REPLY(json) zebkit_network_respond(request, json, NULL, 0, NULL)
static void net_asked(zebkit_view *v, zebkit_network_request *request, const char *json, void *d) {
    (void)d;
    net_request = request;
    ++asked;
    char *info = zebkit_network_request_info(request);
    CHECK(info && !strcmp(info, json));
    zebkit_free(info);
    if (IS("net-block")) {
        CHECK(strstr(json, "\"stage\":\"request\"") && strstr(json, "\"resourceType\":\"fetch\""));
        CHECK(!REPLY("{\"action\":\"nonsense\"}") && !REPLY("not json"));
        CHECK(!zebkit_network_respond(request, "{\"action\":\"continue\"}", "x", 1, NULL));
        CHECK(!REPLY("{\"action\":\"redirect\",\"url\":\"file:///etc/passwd\"}"));
        CHECK(REPLY("{\"action\":\"block\"}") && !REPLY("{\"action\":\"continue\"}"));
    } else if (IS("net-headers")) {
        CHECK(!REPLY("{\"action\":\"continue\",\"headers\":[{\"name\":\"Trailer\",\"value\":\"x\"}]}"));
        CHECK(REPLY("{\"action\":\"continue\",\"headers\":[{\"name\":\"X-Zebkit\",\"value\":\"yes\"}]}"));
    } else if (IS("net-redirect")) {
        char reply[4096];
        snprintf(reply, sizeof reply, "{\"action\":\"redirect\",\"url\":\"%s/echo?redirected\"}", BASE);
        CHECK(REPLY(reply));
    } else if (IS("net-fulfill")) {
        unsigned char body[768];
        for (int i = 0; i < 768; ++i) body[i] = (unsigned char)i;
        CHECK(!REPLY("{\"action\":\"fulfill\",\"status\":302}"));
        CHECK(zebkit_network_respond(request, "{\"action\":\"fulfill\",\"status\":201,\"headers\":[{\"name\":\"Content-Type\",\"value\":\"application/octet-stream\"},{\"name\":\"X-Added\",\"value\":\"by-host\"}]}", body, sizeof body, NULL));
    } else if (IS("net-navigation")) {
        const char *page = "<title>from-host</title>";
        CHECK(strstr(json, "\"navigation\":true") && strstr(json, "\"mainFrame\":true"));
        CHECK(zebkit_network_respond(request, "{\"action\":\"fulfill\",\"headers\":[{\"name\":\"Content-Type\",\"value\":\"text/html\"}]}", page, strlen(page), NULL));
    } else if (IS("net-response")) {
        CHECK(strstr(json, "\"stage\":\"response\"") && strstr(json, "\"status\":200"));
        CHECK(REPLY("{\"action\":\"continue\",\"headers\":[{\"name\":\"Content-Type\",\"value\":\"application/json\"},{\"name\":\"X-Added\",\"value\":\"late\"}]}"));
    } else if (IS("net-response-block")) {
        CHECK(REPLY("{\"action\":\"block\",\"error\":\"failed\"}"));
    } else if (IS("net-close")) {
        zebkit_view_close(v);
    } else if (IS("net-server-redirect")) {
        CHECK(strstr(json, "/echo\""));
        CHECK(REPLY("{\"action\":\"block\"}"));
    } else if (IS("net-worker")) {
        CHECK(strstr(json, "\"worker\":true"));
        CHECK(zebkit_network_respond(request, "{\"action\":\"fulfill\",\"headers\":[{\"name\":\"Content-Type\",\"value\":\"text/plain\"}]}", "from-host", 9, NULL));
    } else if (IS("net-post")) {
        CHECK(strstr(json, "\"method\":\"POST\"") && strstr(json, "\"bodySize\":6") && strstr(json, "\"body\":\"cG9zdGVk\""));
        CHECK(REPLY("{\"action\":\"continue\"}"));
    } else if (IS("net-redirect-manual") || IS("net-redirect-error")) {
        char reply[4096];
        snprintf(reply, sizeof reply, "{\"action\":\"redirect\",\"url\":\"%s/echo?redirected\"}", BASE);
        CHECK(REPLY(reply));
    }
}

/* ---- network: WebSockets, workers, bodies, files, streams */
#define WS_SCRIPT "new Promise(ok => { const s = window.s = new WebSocket(location.origin.replace('http', 'ws') + '/ws'); s.onmessage = e => { ok(e.data); s.close(); }; s.onerror = () => ok('failed'); })"
#define SW_SCRIPT(what) "navigator.serviceWorker.register('/net-sw.js').then(r => new Promise(ok => { const w = r.installing || r.waiting || r.active; if (w.state === 'activated') return ok(w); w.addEventListener('statechange', () => { if (w.state === 'activated') ok(w); }); })).then(w => new Promise(ok => { navigator.serviceWorker.onmessage = e => ok(e.data); w.postMessage('" what "'); }))"
#define SHARED_SCRIPT(what) "new Promise(ok => { const w = window.w = window.w || new SharedWorker('/net-shared.js'); w.port.onmessage = e => ok(e.data); w.port.postMessage('" what "'); })"
#define PAGE_READS(url) "fetch('" url "').then(r => r.arrayBuffer()).then(b => b.byteLength, () => 'failed')"
#define TEXT_HEADER "{\"name\":\"Content-Type\",\"value\":\"text/plain\"}"
static zebkit_network_stream *net_stream;
static long stream_left;
static int stream_blocked;
/* net-stream-full: blocks of 1 MiB, the most one write takes, 16 MiB in all.
 * The engine buffers 1 MiB before its pipe; with smaller blocks a reader as
 * fast as the writer drains it between writes and no write is ever refused,
 * which is what the mode is about. */
static char stream_block[1 << 20];
static void stream_pump(zebkit_network_stream *stream, void *d) {
    (void)d;
    while (stream_left > 0) {
        if (!zebkit_network_stream_write(stream, stream_block, sizeof stream_block)) { stream_blocked = 1; return; }
        stream_left -= (long)sizeof stream_block;
    }
    zebkit_network_stream_end(stream, false);
    CHECK(!zebkit_network_stream_write(stream, "x", 1));
}
static void more_asked(zebkit_view *v, zebkit_network_request *request, const char *json, void *d) {
    (void)d;
    net_request = request;
    ++asked;
    if (IS("net-ws-block")) {
        CHECK(strstr(json, "\"resourceType\":\"websocket\"") && strstr(json, "\"url\":\"ws://") && strstr(json, "\"stage\":\"request\""));
        CHECK(!REPLY("{\"action\":\"redirect\",\"url\":\"http://127.0.0.1/\"}") && !REPLY("{\"action\":\"fulfill\"}"));
        CHECK(!zebkit_network_respond_stream(request, "{}", NULL) && !zebkit_network_respond_file(request, "{}", "/nothing", NULL));
        CHECK(!REPLY("{\"action\":\"continue\",\"headers\":[{\"name\":\"Origin\",\"value\":\"http://x\"}]}"));
        CHECK(REPLY("{\"action\":\"block\"}"));
    } else if (IS("net-ws-continue")) {
        CHECK(REPLY("{\"action\":\"continue\",\"headers\":[{\"name\":\"X-Zebkit\",\"value\":\"yes\"}]}"));
    } else if (IS("net-ws-close")) {
        zebkit_view_close(v);
    } else if (IS("net-sw-block") || IS("net-sw-running")) {
        CHECK(strstr(json, "\"worker\":true") && strstr(json, "\"workerType\":\"service\"") && strstr(json, "\"frameId\":\"\""));
        CHECK(REPLY("{\"action\":\"block\"}"));
    } else if (IS("net-sw-ws")) {
        CHECK(strstr(json, "\"workerType\":\"service\"") && strstr(json, "\"resourceType\":\"websocket\""));
        CHECK(REPLY("{\"action\":\"block\"}"));
    } else if (IS("net-shared-block") || IS("net-shared-running")) {
        CHECK(strstr(json, "\"worker\":true") && strstr(json, "\"workerType\":\"shared\""));
        CHECK(REPLY("{\"action\":\"block\"}"));
    } else if (IS("net-file") || IS("net-file-close")) {
        char path[4096], *error = NULL;
        snprintf(path, sizeof path, "%s/big.bin", FILES);
        CHECK(!zebkit_network_respond_file(request, "{}", "relative.bin", &error) && error);
        zebkit_free(error);
        CHECK(!zebkit_network_respond_file(request, "{\"status\":302}", path, NULL));
        CHECK(zebkit_network_respond_file(request, "{\"status\":201,\"headers\":[{\"name\":\"Content-Type\",\"value\":\"application/x-zebkit\"},{\"name\":\"X-Added\",\"value\":\"by-host\"}]}", path, NULL));
        if (IS("net-file-close")) zebkit_view_close(v);
    } else if (IS("net-file-missing")) {
        char path[4096];
        snprintf(path, sizeof path, "%s/nothing.bin", FILES);
        CHECK(zebkit_network_respond_file(request, "{}", path, NULL));
    } else if (IS("net-stream") || IS("net-stream-full") || IS("net-stream-fail") || IS("net-stream-close")) {
        CHECK(!zebkit_network_respond_stream(request, "{\"status\":302}", NULL));
        net_stream = zebkit_network_respond_stream(request, "{\"status\":201,\"headers\":[" TEXT_HEADER ",{\"name\":\"X-Added\",\"value\":\"by-host\"}]}", NULL);
        CHECK(net_stream && zebkit_network_stream_is_open(net_stream) && !zebkit_network_request_info(request));
        if (IS("net-stream")) {
            CHECK(zebkit_network_stream_write(net_stream, "ab", 2) && zebkit_network_stream_write(net_stream, "cd", 2));
            zebkit_network_stream_end(net_stream, false);
            CHECK(!zebkit_network_stream_is_open(net_stream));
            zebkit_network_stream_end(net_stream, false);
        } else if (IS("net-stream-fail")) {
            CHECK(zebkit_network_stream_write(net_stream, "ab", 2));
            zebkit_network_stream_end(net_stream, true);
        } else if (IS("net-stream-full")) {
            static char too_much[(1 << 20) + 1];
            CHECK(!zebkit_network_stream_write(net_stream, too_much, sizeof too_much));
            stream_left = 16 << 20;
            memset(stream_block, 'f', sizeof stream_block);
            zebkit_network_stream_on_writable(net_stream, stream_pump, NULL);
            stream_pump(net_stream, NULL);
        } else {
            zebkit_view_close(v);
        }
    }
}
static void more_answer(const char *script, const char *expected) {
    char *answer = ev(script);
    if (strcmp(answer, expected)) FAIL("%s: %s", script, answer);
    free(answer);
}
static void network_ws(void) {
    open_page();
    const char *rules = IS("net-ws-none") ? "[]"
        : IS("net-ws-filter") ? "[{\"url\":\"**/ws\",\"resourceTypes\":[\"fetch\"]}]"
        : IS("net-ws-continue") ? "[{\"url\":\"**/ws\",\"resourceTypes\":[\"websocket\"]}]"
        : "[{\"url\":\"**/ws\"}]";
    CHECK(zebkit_view_set_network_filter(view, rules, NULL));
    zebkit_view_on_network_event(view, net_event, NULL);
    if (!IS("net-ws-nofunc")) zebkit_view_on_network(view, more_asked, NULL);
    if (IS("net-ws-timeout")) CHECK(zebkit_view_set_network_timeout(view, 50));
    if (IS("net-ws-block")) { more_answer(WS_SCRIPT, "\"failed\""); CHECK(asked == 1); zebkit_view_close(view); return; }
    ev_later(WS_SCRIPT);
    if (IS("net-ws-close")) { WAIT(closed); CHECK(!zebkit_network_request_info(net_request)); return; }
    if (IS("net-ws-abort")) {
        WAIT(asked);
        free(ev("s.close()"));
        WAIT(!zebkit_network_request_info(net_request));
        WAIT(pending_eval.done);
        zebkit_view_close(view);
        return;
    }
    if (IS("net-ws-replace")) {
        WAIT(asked);
        zebkit_view_on_network(view, NULL, NULL);
        CHECK(!zebkit_network_request_info(net_request));
        WAIT(net_replaced);
    }
    WAIT(pending_eval.done);
    CHECK(pending_eval.json && !strcmp(pending_eval.json, "\"hello\""));
    if (IS("net-ws-timeout")) WAIT(net_timeout);
    if (IS("net-ws-none") || IS("net-ws-nofunc") || IS("net-ws-filter")) CHECK(!asked);
    zebkit_view_close(view);
}
static void network_workers(void) {
    open_page();
    int shared = !strncmp(MODE, "net-shared", 10);
    const char *rules = IS("net-sw-ws") ? "[{\"url\":\"**/ws\"}]" : "[{\"url\":\"**/worker-held\"}]";
    CHECK(!zebkit_view_set_network_filter_for_workers(view, true));
    CHECK(zebkit_view_set_network_filter(view, rules, NULL));
    zebkit_view_on_network(view, more_asked, NULL);
    if (IS("net-sw-block") || IS("net-sw-ws") || IS("net-shared-block")) CHECK(zebkit_view_set_network_filter_for_workers(view, true));
    if (IS("net-sw-ws")) { more_answer(SW_SCRIPT("socket"), "\"failed\""); CHECK(asked == 1); zebkit_view_close(view); return; }
    const char *script = shared ? SHARED_SCRIPT("fetch") : SW_SCRIPT("fetch");
    if (IS("net-sw-running") || IS("net-shared-running")) {
        /* It runs before there is a policy; then the same worker is asked about. */
        more_answer(script, "\"through\"");
        CHECK(!asked);
        CHECK(zebkit_view_set_network_filter_for_workers(view, true));
    }
    if (IS("net-sw-none") || IS("net-shared-none")) { more_answer(script, "\"through\""); CHECK(!asked); }
    else { more_answer(script, "\"failed\""); CHECK(asked == 1); }
    CHECK(zebkit_view_set_network_filter_for_workers(view, false));
    zebkit_view_close(view);
}
static void body_second_told(zebkit_view *v, const char *id, const void *bytes, size_t size, bool end, bool truncated, void *d) {
    (void)v; (void)id; (void)bytes; (void)size; (void)end; (void)truncated; (void)d;
    ++body_second;
}
static void body_told(zebkit_view *v, const char *id, const void *bytes, size_t size, bool end, bool truncated, void *d) {
    (void)d;
    CHECK(!body_ends);
    snprintf(body_id, sizeof body_id, "\"requestId\":\"%s\"", id);
    if (size && IS("net-body")) CHECK(((const char *)bytes)[0] == 'z');
    body_bytes += (long long)size;
    if (end) { ++body_ends; body_truncated += truncated; }
    if (size && IS("net-body-close")) zebkit_view_close(v);
    if (size && IS("net-body-replace")) zebkit_view_on_network_body(v, body_second_told, NULL);
}
static void network_body(void) {
    open_page();
    char *error = NULL;
    CHECK(!zebkit_view_set_network_filter(view, "[{\"body\":true}]", &error) && error);
    zebkit_free(error);
    CHECK(!zebkit_view_set_network_filter(view, "[{\"stage\":\"response\",\"mode\":\"hold\",\"body\":true}]", NULL));
    CHECK(zebkit_view_set_network_filter(view, "[{\"url\":\"**/download.bin\",\"stage\":\"response\",\"body\":true},{\"url\":\"**/big\",\"stage\":\"response\",\"body\":true},{\"url\":\"**/cut\",\"stage\":\"response\",\"body\":true},{\"url\":\"**/slow-text\",\"stage\":\"response\",\"body\":true}]", NULL));
    zebkit_view_on_network_event(view, net_event, NULL);
    if (!IS("net-body-none")) zebkit_view_on_network_body(view, body_told, NULL);
    if (IS("net-body") || IS("net-body-none")) {
        more_answer(PAGE_READS("/download.bin"), "65536");
        WAIT(net_finished_told);
        if (IS("net-body")) {
            CHECK(body_bytes == 65536 && body_ends == 1 && !body_truncated && !body_end_late);
            CHECK(strstr(net_response_json, body_id));
        } else CHECK(!body_ends && net_response_told);
    } else if (IS("net-body-big")) {
        more_answer(PAGE_READS("/big"), "68157440");
        CHECK(body_bytes == 67108864 && body_ends == 1 && body_truncated == 1);
    } else if (IS("net-body-backpressure")) {
        more_answer("fetch('/big').then(r => { window.r = r; return 1; })", "1");
        /* Blink's fetch reads ahead itself, so no bound can be seen from here. */
        pump_for(1000);
        CHECK(!body_ends);
        more_answer("r.arrayBuffer().then(b => b.byteLength)", "68157440");
        CHECK(body_bytes == 67108864 && body_truncated == 1);
    } else if (IS("net-body-error")) {
        more_answer(PAGE_READS("/cut"), "\"failed\"");
        WAIT(net_failed_told);
        CHECK(body_ends == 1 && !body_truncated && !body_end_late && body_bytes == 1000);
    } else if (IS("net-body-abort")) {
        ev_later("(window.a = new AbortController(), fetch('/slow-text', {signal: a.signal}).then(r => r.arrayBuffer()).then(b => b.byteLength, () => 'failed'))");
        WAIT(body_bytes > 0);
        free(ev("a.abort()"));
        WAIT(pending_eval.done);
        CHECK(!strcmp(pending_eval.json, "\"failed\""));
        WAIT(net_failed_told);
        CHECK(body_ends == 1 && !body_truncated && !body_end_late);
    } else if (IS("net-body-close")) {
        ev_later(PAGE_READS("/slow-text"));
        WAIT(closed);
        long long at = body_bytes;
        pump_for(200);
        CHECK(body_bytes == at);
        return;
    } else if (IS("net-body-replace")) {
        more_answer(PAGE_READS("/big"), "68157440");
        CHECK(!body_second && !body_ends);
        more_answer(PAGE_READS("/download.bin"), "65536");
        CHECK(body_second >= 2);
    }
    zebkit_view_close(view);
}
static void network_answers(void) {
    open_page();
    CHECK(zebkit_view_set_network_filter(view, "[{\"url\":\"**/held\"}]", NULL));
    zebkit_view_on_network_event(view, net_event, NULL);
    zebkit_view_on_network(view, more_asked, NULL);
    if (IS("net-file")) {
        more_answer("fetch('/held').then(async r => [r.status, r.headers.get('x-added'), r.headers.get('content-type'), (await r.arrayBuffer()).byteLength].join('|'))", "\"201|by-host|application/x-zebkit|3145728\"");
    } else if (IS("net-file-missing")) {
        more_answer(PAGE_READS("/held"), "\"failed\"");
        WAIT(net_failed_told);
    } else if (IS("net-file-close") || IS("net-stream-close")) {
        ev_later(PAGE_READS("/held"));
        WAIT(closed);
        if (net_stream) {
            WAIT(!zebkit_network_stream_write(net_stream, "x", 1));
            CHECK(!zebkit_network_stream_is_open(net_stream));
            zebkit_network_stream_end(net_stream, false);
        }
        return;
    } else if (IS("net-stream")) {
        more_answer("fetch('/held').then(async r => [r.status, r.headers.get('x-added'), await r.text()].join('|'))", "\"201|by-host|abcd\"");
    } else if (IS("net-stream-full")) {
        more_answer(PAGE_READS("/held"), "16777216");
        CHECK(stream_blocked && stream_left <= 0);
    } else if (IS("net-stream-fail")) {
        more_answer(PAGE_READS("/held"), "\"failed\"");
        WAIT(net_failed_told);
    }
    zebkit_view_close(view);
}
static void network_redirect_modes(void) {
    open_page();
    CHECK(zebkit_view_set_network_filter(view, "[{\"url\":\"**/held\"}]", NULL));
    zebkit_view_on_network(view, net_asked, NULL);
    if (IS("net-redirect-manual")) more_answer("fetch('/held', {redirect: 'manual'}).then(r => r.type, () => 'failed')", "\"opaqueredirect\"");
    else more_answer("fetch('/held', {redirect: 'error'}).then(r => r.type, () => 'failed')", "\"failed\"");
    CHECK(asked == 1);
    zebkit_view_close(view);
}
static void network(void) {
    if (!strncmp(MODE, "net-ws-", 7)) { network_ws(); return; }
    if (!strncmp(MODE, "net-sw-", 7) || !strncmp(MODE, "net-shared-", 11)) { network_workers(); return; }
    if (!strncmp(MODE, "net-body", 8)) { network_body(); return; }
    if (!strncmp(MODE, "net-file", 8) || !strncmp(MODE, "net-stream", 10)) { network_answers(); return; }
    if (IS("net-redirect-manual") || IS("net-redirect-error")) { network_redirect_modes(); return; }
    open_page();
    const char *rules = "[{\"url\":\"**/held\"}]";
    if (IS("net-response") || IS("net-response-block")) rules = "[{\"url\":\"**/echo\",\"stage\":\"response\"}]";
    else if (IS("net-observe")) rules = "[{\"url\":\"**/echo\",\"mode\":\"observe\"},{\"url\":\"**/echo\",\"mode\":\"observe\",\"stage\":\"response\"}]";
    else if (IS("net-none")) rules = "[]";
    else if (IS("net-filter")) rules = "[{\"url\":\"**/echo\",\"resourceTypes\":[\"image\"]},{\"url\":\"**/echo\",\"frames\":\"sub\"}]";
    else if (IS("net-server-redirect") || IS("net-post") || IS("net-headers")) rules = "[{\"url\":\"**/echo\"}]";
    else if (IS("net-worker")) rules = "[{\"url\":\"**/echo?worker\"}]";
    else if (IS("net-navigation")) rules = "[{\"url\":\"**/held\",\"frames\":\"main\",\"resourceTypes\":[\"document\"]}]";
    else if (IS("net-flood")) rules = "[{\"url\":\"**/held*\"}]";
    char *error = NULL;
    CHECK(!zebkit_view_set_network_filter(view, "[{\"mode\":\"sometimes\"}]", &error) && error);
    zebkit_free(error);
    CHECK(zebkit_view_set_network_filter(view, rules, NULL));
    zebkit_view_on_network_event(view, net_event, NULL);
    zebkit_view_on_network(view, net_asked, NULL);
    if (IS("net-timeout")) CHECK(zebkit_view_set_network_timeout(view, 50));
    if (IS("net-navigation")) {
        load(BASE, "/held");
        wait_title("from-host");
        zebkit_view_close(view);
        return;
    }
    const char *url = (IS("net-none") || IS("net-observe") || IS("net-response") || IS("net-response-block") || IS("net-filter") || IS("net-headers") || IS("net-post")) ? "/echo" : IS("net-server-redirect") ? "/server-redirect" : "/held";
    char script[512];
    if (IS("net-worker")) snprintf(script, sizeof script, "new Promise(ok => { const w = new Worker('/net-worker'); w.onmessage = e => ok(e.data); })");
    /* What is past the queue is refused, not let through: nothing here is answered. */
    else if (IS("net-flood")) snprintf(script, sizeof script, "Promise.race(Array.from({length: 1100}, (_, i) => fetch('/held?' + i).then(() => 'through', () => 'failed')))");
    else snprintf(script, sizeof script, "fetch('%s'%s).then(async r => [r.status, r.redirected, r.headers.get('x-added'), await r.text()].join('|'), e => 'failed')", url, IS("net-post") ? ", {method: 'POST', body: 'posted'}" : "");
    ev_later(script);
    if (IS("net-close")) { WAIT(closed); CHECK(!zebkit_network_request_info(net_request)); return; }
    if (IS("net-pending-close")) {
        WAIT(asked);
        zebkit_view_close(view);
        WAIT(closed);
        CHECK(!zebkit_network_request_info(net_request));
        CHECK(!zebkit_network_respond(net_request, "{\"action\":\"continue\"}", NULL, 0, NULL));
        return;
    }
    if (IS("net-replace")) {
        WAIT(asked);
        zebkit_view_on_network(view, NULL, NULL);
        CHECK(!zebkit_network_request_info(net_request));
        WAIT(net_replaced);
    }
    WAIT(pending_eval.done);
    const char *answer = pending_eval.json ? pending_eval.json : "";
    if (IS("net-none") || IS("net-filter")) { CHECK(strstr(answer, "\"200|") && !asked); }
    else if (IS("net-block") || IS("net-response-block") || IS("net-server-redirect")) { CHECK(!strcmp(answer, "\"failed\"") && asked == 1); }
    else if (IS("net-headers")) { CHECK(strstr(answer, "x-zebkit")); }
    else if (IS("net-redirect")) { CHECK(strstr(answer, "\"200|true|")); }
    else if (IS("net-fulfill")) { CHECK(strstr(answer, "\"201|false|by-host|")); }
    else if (IS("net-response")) { CHECK(strstr(answer, "\"200|false|late|")); }
    else if (IS("net-worker")) { CHECK(!strcmp(answer, "\"from-host\"")); }
    else if (IS("net-observe")) { CHECK(!asked); WAIT(net_request_told && net_response_told && net_finished_told); }
    else if (IS("net-timeout")) { WAIT(net_timeout); CHECK(!zebkit_network_request_info(net_request)); }
    else if (IS("net-flood")) { CHECK(!strcmp(answer, "\"failed\"") && asked >= 1024); }
    else if (IS("net-post")) { CHECK(strstr(answer, "\"200|")); }
    /* Its checks are above: the request went stale, the handler-changed event came. */
    else if (IS("net-replace")) {}
    else FAIL("a network mode with nothing to check");
    zebkit_view_close(view);
}

/* ---- popups, two views, a running download, a CDP client beside the hook, the workers' latch */
/* The other views of a mode (popups, a second view), closed with it. */
static zebkit_view *others[4];
static int others_closed[4], other_count;
static void other_closed(zebkit_view *v, void *d) { (void)v; others_closed[(intptr_t)d] = 1; }
static int add_other(zebkit_view *v) {
    if (other_count == 4) FAIL("too many views");
    others[other_count] = v;
    others_closed[other_count] = 0;
    zebkit_view_on_close(v, other_closed, (void *)(intptr_t)other_count);
    return other_count++;
}
static int other_finished[4];
static void other_load(zebkit_view *v, zebkit_load_event event, const char *url, void *d) {
    (void)v; (void)url;
    if (event == ZEBKIT_LOAD_FINISHED) other_finished[(intptr_t)d] = 1;
}
/* Another view at BASE/page, loaded: its index in others. */
static int another_page(void) {
    int i = add_other(zebkit_view_new(NULL, 640, 480));
    other_finished[i] = 0;
    zebkit_view_on_load(others[i], other_load, (void *)(intptr_t)i);
    char url[4096];
    snprintf(url, sizeof url, "%s/page", BASE);
    zebkit_view_load_url(others[i], url);
    WAIT(other_finished[i]);
    for (int t = 0; !ev_in_is(others[i], "location.pathname + ' ' + document.readyState", "\"/page complete\""); ++t) {
        if (t > 3000) FAIL("the other view did not load");
        pump_for(5);
    }
    return i;
}
static void close_others(void) {
    for (int i = 0; i < other_count; ++i) if (!others_closed[i]) zebkit_view_close(others[i]);
    for (int i = 0; i < other_count; ++i) WAIT(others_closed[i]);
}
#define PASSES "fetch('/held').then(() => 'passed', () => 'failed')"
static zebkit_view *popup_child;
static int popup_asked_download, popup_asked_network, popup_own_asked, popup_cancelled, popup_name_ok;
static void popup_opened(zebkit_view *opener, zebkit_view *opened, const char *url, void *d) {
    (void)opener; (void)url; (void)d;
    if (!popup_child) popup_child = opened;
}
static void popup_download(zebkit_view *v, zebkit_download_request *request, const char *json, void *d) {
    (void)v; (void)d;
    ++popup_asked_download;
    if (strstr(json, "\"suggestedName\":\"report.bin\"")) popup_name_ok = 1;
    CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, NULL, NULL));
}
static void popup_own_download(zebkit_view *v, zebkit_download_request *request, const char *json, void *d) {
    (void)v; (void)json; (void)d;
    ++popup_own_asked;
    CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, NULL, NULL));
}
static void popup_network(zebkit_view *v, zebkit_network_request *request, const char *json, void *d) {
    (void)v; (void)json; (void)d;
    ++popup_asked_network;
    CHECK(zebkit_network_respond(request, "{\"action\":\"block\"}", NULL, 0, NULL));
}
static void popup_progress(zebkit_view *v, const char *json, void *d) {
    (void)v; (void)d;
    if (strstr(json, "\"state\":\"cancelled\"") && strstr(json, "\"done\":true")) popup_cancelled = 1;
}
static void popup(void) {
    popup_child = NULL;
    popup_asked_download = popup_asked_network = popup_own_asked = popup_cancelled = popup_name_ok = 0;
    open_page();
    zebkit_view_on_window_open(view, popup_opened, NULL);
    zebkit_view_on_download(view, popup_download, NULL);
    CHECK(zebkit_view_set_network_filter(view, "[{\"url\":\"**/held\"}]", NULL));
    zebkit_view_on_network(view, popup_network, NULL);
    free(ev("window.open('/page'); 1"));
    WAIT(popup_child);
    int child = add_other(popup_child);
    zebkit_view_on_download_progress(popup_child, popup_progress, NULL);
    /* The URL is there before the document: wait for that. */
    for (int t = 0; !ev_in_is(popup_child, "document.title", "\"page\""); ++t) {
        if (t > 3000) FAIL("the popup did not load");
        pump_for(5);
    }
    if (IS("popup-inherit")) {
        /* The opener's functions decide for the popup. */
        free(ev_in(popup_child, "location.href = '/download.bin'; 1"));
        WAIT(popup_asked_download);
        CHECK(popup_name_ok);
        WAIT(popup_cancelled);
        CHECK(ev_in_is(popup_child, PASSES, "\"failed\""));
        CHECK(popup_asked_network);
    } else if (IS("popup-own")) {
        /* A function of its own replaces its opener's. */
        zebkit_view_on_download(popup_child, popup_own_download, NULL);
        free(ev_in(popup_child, "location.href = '/download.bin'; 1"));
        WAIT(popup_own_asked);
        CHECK(!popup_asked_download);
    } else if (IS("popup-orphan")) {
        /* With its opener gone, a popup without functions of its own has no policy left. */
        zebkit_view_close(view);
        WAIT(closed);
        CHECK(ev_in_is(popup_child, PASSES, "\"failed\""));
        CHECK(!popup_asked_network && !popup_asked_download);
        /* Its navigations are refused too; a link with download attribute is not one. */
        free(ev_in(popup_child, "const a = document.createElement('a'); a.href = '/download.bin'; a.download = ''; a.click(); 1"));
        WAIT(popup_cancelled);
        CHECK(ev_in_is(popup_child, FULL, "\"TypeError\""));
    } else FAIL("a popup mode with nothing to check");
    zebkit_view_close(popup_child);
    WAIT(others_closed[child]);
    zebkit_view_close(view);
}
static int running, running_cancelled;
static char running_id[64];
static void running_progress(zebkit_view *v, const char *json, void *d) {
    (void)v; (void)d;
    const char *id = strstr(json, "\"downloadId\":\"");
    if (strstr(json, "\"state\":\"in-progress\"") && id && !strstr(json, "\"receivedBytes\":0,") && !running_id[0]) {
        snprintf(running_id, sizeof running_id, "%s", id + 14);
        *strchr(running_id, '"') = 0;
        running = 1;
    }
    if (strstr(json, "\"state\":\"cancelled\"") && strstr(json, "\"done\":true")) running_cancelled = 1;
}
static void running_asked(zebkit_view *v, zebkit_download_request *request, const char *json, void *d) {
    (void)v; (void)json; (void)d;
    CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_SAVE, target, NULL));
}
static void cancel_running(void) {
    running = running_cancelled = 0;
    running_id[0] = 0;
    snprintf(target, sizeof target, "%s/running.bin", DOWNLOADS);
    open_page();
    zebkit_view_on_download(view, running_asked, NULL);
    zebkit_view_on_download_progress(view, running_progress, NULL);
    free(ev("location.href = '/slow.bin'; 1"));
    WAIT(running);
    CHECK(!zebkit_view_cancel_download(view, "d999999"));
    CHECK(zebkit_view_cancel_download(view, running_id));
    WAIT(running_cancelled);
    CHECK(!zebkit_view_cancel_download(view, running_id));
    CHECK(file_size(target) < 0);
    zebkit_view_close(view);
}
/* The request's own headers and X-Hook: first, continued. */
static void order_asked(zebkit_view *v, zebkit_network_request *request, const char *json, void *d) {
    (void)v; (void)d;
    const char *headers = strstr(json, "\"headers\":[");
    CHECK(headers);
    headers += 10;
    const char *end = headers;
    int depth = 0, quoted = 0;
    for (; *end; ++end) {
        if (quoted) { if (*end == '\\') ++end; else if (*end == '"') quoted = 0; continue; }
        if (*end == '"') quoted = 1;
        else if (*end == '[') ++depth;
        else if (*end == ']' && !--depth) break;
    }
    CHECK(*end == ']');
    size_t size = (size_t)(end - headers) + 128;
    char *reply = malloc(size);
    snprintf(reply, size, "{\"action\":\"continue\",\"headers\":%.*s%s{\"name\":\"X-Hook\",\"value\":\"first\"}]}",
             (int)(end - headers), headers, end - headers > 1 ? "," : "");
    CHECK(zebkit_network_respond(request, reply, NULL, 0, NULL));
    free(reply);
}
static void cdp_order(void) {
    load(BASE, "/input");
    CHECK(zebkit_view_set_network_filter(view, "[{\"url\":\"**/held\"}]", NULL));
    zebkit_view_on_network(view, order_asked, NULL);
    wait_title("fetch-on");
    CHECK(ev_is("fetch('/held').then(r => r.text())", "\"from-cdp\""));
    zebkit_view_close(view);
}
static int two_asked[2], two_order[2], two_count, first_complete, second_cancelled;
static void two_download(zebkit_view *v, zebkit_download_request *request, const char *json, void *d) {
    (void)v; (void)json;
    int which = (int)(intptr_t)d;
    if (two_count < 2) two_order[two_count++] = which;
    ++two_asked[which];
    if (which == 0) CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_SAVE, target, NULL));
    else CHECK(zebkit_download_respond(request, ZEBKIT_DOWNLOAD_CANCEL, NULL, NULL));
}
static void two_progress(zebkit_view *v, const char *json, void *d) {
    (void)v;
    if (d == NULL && strstr(json, "\"state\":\"complete\"")) first_complete = 1;
    if (d != NULL && strstr(json, "\"state\":\"cancelled\"")) second_cancelled = 1;
}
static void block_all(zebkit_view *v, zebkit_network_request *request, const char *json, void *d) {
    (void)v; (void)json; (void)d;
    CHECK(zebkit_network_respond(request, "{\"action\":\"block\"}", NULL, 0, NULL));
}
static void two_views(void) {
    memset(two_asked, 0, sizeof two_asked);
    two_count = first_complete = second_cancelled = 0;
    snprintf(target, sizeof target, "%s/first.bin", DOWNLOADS);
    open_page();
    zebkit_view *second = others[another_page()];
    zebkit_view_on_download(view, two_download, (void *)0);
    zebkit_view_on_download(second, two_download, (void *)1);
    zebkit_view_on_download_progress(view, two_progress, NULL);
    zebkit_view_on_download_progress(second, two_progress, (void *)1);
    CHECK(zebkit_view_set_network_filter(view, "[{\"url\":\"**/held\"}]", NULL));
    zebkit_view_on_network(view, block_all, NULL);
    CHECK(zebkit_view_set_fullscreen_policy(view, ZEBKIT_FULLSCREEN_DENY));
    free(ev("location.href = '/download.bin'; 1"));
    WAIT(first_complete);
    free(ev_in(second, "location.href = '/download.bin'; 1"));
    WAIT(second_cancelled);
    CHECK(two_count == 2 && two_order[0] == 0 && two_order[1] == 1 && two_asked[0] == 1 && two_asked[1] == 1);
    CHECK(file_size(target) == 65536);
    CHECK(ev_is(PASSES, "\"failed\"") && ev_in_is(second, PASSES, "\"passed\""));
    zebkit_view_focus(second);
    CHECK(ev_is(FULL, "\"TypeError\"") && ev_in_is(second, FULL, "\"in\""));
    zebkit_view_close(second);
    zebkit_view_close(view);
}
#define SW_FETCH SW_SCRIPT("fetch")
#define SHARED_FETCH SHARED_SCRIPT("fetch")
static void workers_latch(void) {
    /* A policy outlives the view that held it: refused until a view takes over or says off. */
    open_page();
    zebkit_view *holder = view, *second = others[another_page()];
    int other = add_other(zebkit_view_new(NULL, 640, 480));
    const char *both[] = {SW_FETCH, SHARED_FETCH};
    CHECK(zebkit_view_set_network_filter_for_workers(holder, false));
    CHECK(zebkit_view_set_network_filter(holder, "[]", NULL));
    CHECK(zebkit_view_set_network_filter_for_workers(holder, true));
    for (int i = 0; i < 2; ++i) CHECK(ev_in_is(second, both[i], "\"through\""));
    CHECK(zebkit_view_set_network_filter(second, "[]", NULL));
    CHECK(!zebkit_view_set_network_filter_for_workers(second, false));
    zebkit_view_close(holder);
    WAIT(closed);
    for (int i = 0; i < 2; ++i) CHECK(ev_in_is(second, both[i], "\"failed\""));
    CHECK(zebkit_view_set_network_filter_for_workers(second, true));
    for (int i = 0; i < 2; ++i) CHECK(ev_in_is(second, both[i], "\"through\""));
    zebkit_view_close(second);
    WAIT(others_closed[0]);
    CHECK(zebkit_view_set_network_filter_for_workers(others[other], false));
    zebkit_view *page = others[another_page()];
    for (int i = 0; i < 2; ++i) CHECK(ev_in_is(page, both[i], "\"through\""));
    zebkit_view_close(page);
    zebkit_view_close(others[other]);
}

/* The modes this program has: any other is a failure, not a pass. */
static const char *const KNOWN[] = {
    "external-told", "external-none", "external-close",
    "download-save", "download-cancel", "download-timeout", "download-close", "download-pending-close",
    "download-replace", "download-default",
    "cert-provide", "cert-none", "cert-cancel", "cert-timeout", "cert-close", "cert-pending-close",
    "cert-replace", "cert-default", "cert-store", "cert-store-off",
    "world",
    "file-open", "file-multiple", "file-folder", "file-cancel", "file-timeout", "file-close",
    "file-pending-close", "file-replace", "media-allow", "media-deny", "media-timeout", "media-close",
    "media-pending-close", "media-replace", "keys", "menu", "fullscreen-allow", "fullscreen-deny",
    "net-none", "net-block", "net-headers", "net-redirect", "net-fulfill", "net-navigation", "net-response",
    "net-response-block", "net-observe", "net-timeout", "net-close", "net-pending-close", "net-replace",
    "net-filter", "net-server-redirect", "net-worker", "net-post", "net-redirect-manual", "net-redirect-error",
    "net-ws-none", "net-ws-nofunc", "net-ws-filter", "net-ws-block", "net-ws-continue", "net-ws-timeout",
    "net-ws-close", "net-ws-abort", "net-ws-replace",
    "net-sw-none", "net-sw-block", "net-sw-running", "net-sw-ws",
    "net-shared-none", "net-shared-block", "net-shared-running",
    "net-body", "net-body-none", "net-body-big", "net-body-backpressure", "net-body-error", "net-body-close",
    "net-body-abort", "net-body-replace", "net-file", "net-file-missing", "net-file-close",
    "net-stream", "net-stream-full", "net-stream-fail", "net-stream-close", "net-flood",
    "popup-inherit", "popup-orphan", "popup-own", "two-views", "cancel-running", "cdp-order", "workers-latch",
};

/* Every mode starts from nothing: the state the last one left is forgotten. */
static void reset_state(void) {
    closed = finished = failed = told = asked = 0;
    last_told[0] = 0;
    download_request = NULL;
    target[0] = 0;
    download_complete = download_cancelled = download_saved = download_timeout = download_replaced = download_in_profile = 0;
    cert_request = NULL;
    file_request = NULL;
    media_request = NULL;
    net_request = NULL;
    net_timeout = net_replaced = net_request_told = net_response_told = net_finished_told = net_failed_told = 0;
    body_ends = body_truncated = body_second = body_end_late = 0;
    body_bytes = 0;
    net_response_json[0] = body_id[0] = 0;
    net_stream = NULL;
    stream_left = 0;
    stream_blocked = 0;
    memset(&pending_eval, 0, sizeof pending_eval);
    other_count = 0;
}

static void run_mode(void) {
    size_t known = 0;
    while (known < sizeof KNOWN / sizeof *KNOWN && !IS(KNOWN[known])) ++known;
    if (known == sizeof KNOWN / sizeof *KNOWN) FAIL("an unknown mode");
    reset_state();
    view = zebkit_view_new(NULL, 640, 480);
    zebkit_view_on_close(view, on_close, NULL);
    zebkit_view_on_load(view, on_load, NULL);
    /* The workers' network policy is the engine's, and outlives the view that
       set it: off, as an engine starts. */
    (void)zebkit_view_set_network_filter_for_workers(view, false);
    if (!strncmp(MODE, "external", 8)) external();
    else if (!strncmp(MODE, "download", 8)) download();
    else if (!strncmp(MODE, "cert", 4)) certificate();
    else if (IS("world")) world();
    else if (!strncmp(MODE, "file", 4)) chooser();
    else if (!strncmp(MODE, "media", 5)) media();
    else if (IS("keys")) keys();
    else if (IS("menu")) menu();
    else if (!strncmp(MODE, "fullscreen", 10)) fullscreen();
    else if (!strncmp(MODE, "net", 3)) network();
    else if (!strncmp(MODE, "popup", 5)) popup();
    else if (IS("two-views")) two_views();
    else if (IS("cancel-running")) cancel_running();
    else if (IS("cdp-order")) cdp_order();
    else if (IS("workers-latch")) workers_latch();
    else FAIL("a known mode with no function");
    /* Its views gone before the next mode's, and what they left pending told. */
    if (!closed) zebkit_view_close(view);
    WAIT(closed);
    close_others();
    pump_for(50);
    printf("ok c %s\n", MODE);
    fflush(stdout);
}

/* ZEBKIT_HOOK_RUNS: the path of a file with a JSON array of objects whose
 * values are strings, as hook_policies.py writes it (a file: a group's runs
 * pass Windows' 32767 characters a variable); read here without a JSON library. */
static char *json_string(const char **at) {
    const char *p = *at;
    if (*p != '"') return NULL;
    char *out = malloc(strlen(p) + 1), *o = out;
    for (++p; *p && *p != '"'; ++p) {
        if (*p == '\\') {
            ++p;
            if (*p == 'n') *o++ = '\n';
            else if (*p == 't') *o++ = '\t';
            else if (*p == 'u') FAIL("ZEBKIT_HOOK_RUNS: \\u escapes are not read here");
            else *o++ = *p;
        } else *o++ = *p;
    }
    if (*p != '"') FAIL("ZEBKIT_HOOK_RUNS: an unterminated string");
    *o = 0;
    *at = p + 1;
    return out;
}
static void skip_space(const char **at) { while (**at == ' ' || **at == '\n' || **at == '\t' || **at == '\r') ++*at; }
/* The next object of the array into the mode's globals; false after the last. */
static int next_run(const char **at) {
    skip_space(at);
    if (**at == '[' || **at == ',') { ++*at; skip_space(at); }
    if (**at == ']' || !**at) return 0;
    if (**at != '{') FAIL("ZEBKIT_HOOK_RUNS: not an array of objects");
    ++*at;
    for (;;) {
        skip_space(at);
        if (**at == '}') { ++*at; return 1; }
        if (**at == ',') { ++*at; continue; }
        char *name = json_string(at);
        if (!name) FAIL("ZEBKIT_HOOK_RUNS: a name expected");
        skip_space(at);
        if (**at != ':') FAIL("ZEBKIT_HOOK_RUNS: ':' expected");
        ++*at;
        skip_space(at);
        char *value = json_string(at);
        if (!value) FAIL("ZEBKIT_HOOK_RUNS: a string value expected");
        if (!strcmp(name, "ZEBKIT_HOOK_MODE")) MODE = value;
        else if (!strcmp(name, "ZEBKIT_HOOK_BASE")) BASE = value;
        else if (!strcmp(name, "ZEBKIT_HOOK_TLS")) TLS = value;
        else if (!strcmp(name, "ZEBKIT_HOOK_FILES")) FILES = value;
        else if (!strcmp(name, "ZEBKIT_HOOK_DOWNLOADS")) DOWNLOADS = value;
        else free(value);
        free(name);
    }
}

int main(int argc, char **argv) {
    MODE = getenv("ZEBKIT_HOOK_MODE"); BASE = getenv("ZEBKIT_HOOK_BASE"); TLS = getenv("ZEBKIT_HOOK_TLS");
    FILES = getenv("ZEBKIT_HOOK_FILES"); DOWNLOADS = getenv("ZEBKIT_HOOK_DOWNLOADS");
    const char *runs_file = getenv("ZEBKIT_HOOK_RUNS");
    if (!MODE && !runs_file) return 2;
    const char *runs = runs_file ? read_file(runs_file) : NULL;
    CERT = read_file(getenv("ZEBKIT_HOOK_CERT"));
    KEY = read_file(getenv("ZEBKIT_HOOK_KEY"));
    OTHER_KEY = read_file(getenv("ZEBKIT_HOOK_OTHER_KEY"));
    if (zebkit_init(argc, argv)) return 1;
    if (runs) {
        while (next_run(&runs)) run_mode();
    } else {
        run_mode();
    }
    zebkit_shutdown();
    return 0;
}
