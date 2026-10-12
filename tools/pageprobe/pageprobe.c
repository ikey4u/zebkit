/*
 * pageprobe: load one URL in a headless WPE WebKit view and print what
 * happened as a single JSON object on stdout.
 *
 *   pageprobe [--settle SECONDS] [--timeout SECONDS] [--snapshot FILE] [--eval SCRIPT]
 *             [--no-web-security] URL
 *
 * It reports whether the load finished or failed, whether the web process
 * crashed, uncaught script errors, a set of DOM measurements taken after the
 * page has settled, and how much of the viewport was actually painted.
 * scripts/compat.sh runs it for every site in assets/WEBSITE-TOP-100.json
 * against two engine builds and compares the answers.
 *
 * --snapshot writes the viewport as raw pixels: a text line "width height
 * stride" followed by the bytes, four per pixel.
 * --eval runs SCRIPT in the settled page instead of the standard
 * measurements and reports its string result as "page"; it is for
 * investigating a failing site.
 * --no-web-security turns off the same-origin policy, which makes errors
 * from cross-origin scripts show their message instead of "Script error.".
 * Also only for investigation: pages can behave differently with it.
 *
 * pageprobe.m is the same program for the Mac engine, with the same report.
 */

#include "probe-scripts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wpe/headless/wpe-headless.h>
#include <wpe/webkit.h>

typedef struct {
    GMainLoop* loop;
    WebKitWebView* view;
    const char* url;
    const char* snapshotPath;
    const char* evalScript;
    gboolean noWebSecurity;
    guint settleSeconds;
    gint64 startTime;
    gint64 finishTime;
    gboolean finished;
    gboolean measuring;
    char* failure;
    char* crash;
    char* page;
    char* paint;
} Probe;

static void appendJSONString(GString* out, const char* value)
{
    if (!value) {
        g_string_append(out, "null");
        return;
    }
    g_string_append_c(out, '"');
    for (const unsigned char* c = (const unsigned char*)value; *c; c++) {
        if (*c == '"' || *c == '\\')
            g_string_append_printf(out, "\\%c", *c);
        else if (*c < 0x20)
            g_string_append_printf(out, "\\u%04x", *c);
        else
            g_string_append_c(out, *c);
    }
    g_string_append_c(out, '"');
}

static void report(Probe* probe)
{
    GString* out = g_string_new("{\"requested\":");
    appendJSONString(out, probe->url);
    g_string_append_printf(out, ",\"loadFinished\":%s", probe->finished ? "true" : "false");
    if (probe->finished)
        g_string_append_printf(out, ",\"loadMs\":%" G_GINT64_FORMAT, (probe->finishTime - probe->startTime) / 1000);
    g_string_append(out, ",\"loadFailed\":");
    appendJSONString(out, probe->failure);
    g_string_append(out, ",\"crashed\":");
    appendJSONString(out, probe->crash);
    g_string_append_printf(out, ",\"paint\":%s", probe->paint ? probe->paint : "null");
    g_string_append_printf(out, ",\"page\":%s}", probe->page ? probe->page : "null");
    puts(out->str);
    fflush(stdout);
    g_string_free(out, TRUE);
    g_main_loop_quit(probe->loop);
}

/* How much of the viewport differs from its most common colour, and how many
 * distinct colours a coarse sample sees. A blank or single-colour viewport is
 * the signature of a page that loaded but did not render. */
static void snapshotReady(GObject* source, GAsyncResult* result, gpointer data)
{
    Probe* probe = data;
    GError* error = NULL;
    WebKitImage* image = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error);
    if (!image) {
        g_clear_error(&error);
        report(probe);
        return;
    }

    int width = webkit_image_get_width(image);
    int height = webkit_image_get_height(image);
    guint stride = webkit_image_get_stride(image);
    GBytes* bytes = webkit_image_as_bytes(image);
    gsize size = 0;
    const guint8* pixels = g_bytes_get_data(bytes, &size);

    GHashTable* colours = g_hash_table_new(g_direct_hash, g_direct_equal);
    guint samples = 0, mostCommon = 0;
    for (int y = 0; y < height; y += 4) {
        for (int x = 0; x < width; x += 4) {
            guint32 pixel;
            memcpy(&pixel, pixels + (gsize)y * stride + (gsize)x * 4, 4);
            /* Drop the low bits so antialiasing noise does not count as colour. */
            gpointer key = GUINT_TO_POINTER((pixel & 0x00f0f0f0) | 1);
            guint seen = GPOINTER_TO_UINT(g_hash_table_lookup(colours, key)) + 1;
            g_hash_table_insert(colours, key, GUINT_TO_POINTER(seen));
            if (seen > mostCommon)
                mostCommon = seen;
            samples++;
        }
    }
    probe->paint = g_strdup_printf("{\"width\":%d,\"height\":%d,\"colours\":%u,\"paintedFraction\":%.3f}",
        width, height, g_hash_table_size(colours), samples ? 1.0 - (double)mostCommon / samples : 0.0);
    g_hash_table_unref(colours);

    if (probe->snapshotPath) {
        FILE* file = fopen(probe->snapshotPath, "wb");
        if (file) {
            fprintf(file, "%d %d %u\n", width, height, stride);
            fwrite(pixels, 1, size, file);
            fclose(file);
        }
    }
    g_object_unref(image); /* owns the bytes */
    report(probe);
}

static void measured(GObject* source, GAsyncResult* result, gpointer data)
{
    Probe* probe = data;
    GError* error = NULL;
    JSCValue* value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
    if (value && jsc_value_is_string(value)) {
        probe->page = jsc_value_to_string(value);
        if (probe->evalScript) {
            /* Not JSON of our own making: quote it. */
            GString* quoted = g_string_new(NULL);
            appendJSONString(quoted, probe->page);
            g_free(probe->page);
            probe->page = g_string_free(quoted, FALSE);
        }
    }
    g_clear_object(&value);
    g_clear_error(&error);
    if (probe->crash) {
        report(probe);
        return;
    }
    webkit_web_view_get_snapshot(probe->view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
        NULL, snapshotReady, probe);
}

static gboolean measure(gpointer data)
{
    Probe* probe = data;
    if (probe->measuring)
        return G_SOURCE_REMOVE;
    probe->measuring = TRUE;
    if (probe->crash) {
        report(probe);
        return G_SOURCE_REMOVE;
    }
    webkit_web_view_evaluate_javascript(probe->view, probe->evalScript ? probe->evalScript : measureScript, -1,
        NULL, NULL, NULL, measured, probe);
    return G_SOURCE_REMOVE;
}

static void loadChanged(WebKitWebView* view, WebKitLoadEvent event, gpointer data)
{
    Probe* probe = data;
    if (event != WEBKIT_LOAD_FINISHED || probe->finished)
        return;
    probe->finished = TRUE;
    probe->finishTime = g_get_monotonic_time();
    /* Give scripts time to build the page after the load event. */
    g_timeout_add_seconds(probe->settleSeconds, measure, probe);
}

static gboolean loadFailed(WebKitWebView* view, WebKitLoadEvent event, const char* uri, GError* error, gpointer data)
{
    Probe* probe = data;
    /* A cancelled load is a redirect or a script navigating away, not a failure. */
    if (!probe->failure && !g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED)
        && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        probe->failure = g_strdup_printf("%s (%s)", error->message, uri);
    return FALSE;
}

static gboolean loadFailedWithTLSErrors(WebKitWebView* view, const char* uri, GTlsCertificate* certificate,
    GTlsCertificateFlags errors, gpointer data)
{
    Probe* probe = data;
    if (!probe->failure)
        probe->failure = g_strdup_printf("TLS certificate error 0x%x (%s)", errors, uri);
    return FALSE;
}

static void webProcessTerminated(WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer data)
{
    Probe* probe = data;
    static const char* reasons[] = { "crashed", "exceeded memory limit", "terminated by API" };
    if (!probe->crash)
        probe->crash = g_strdup(reason < G_N_ELEMENTS(reasons) ? reasons[reason] : "terminated");
    if (!probe->measuring) {
        probe->measuring = TRUE;
        report(probe);
    }
}

static gboolean timedOut(gpointer data)
{
    /* Measure whatever is there; "loadFinished" stays false in the report. */
    measure(data);
    return G_SOURCE_REMOVE;
}

static gboolean gaveUp(gpointer data)
{
    /* The page never answered the measurement: its main thread is stuck. */
    Probe* probe = data;
    if (!probe->failure)
        probe->failure = g_strdup("page stopped responding");
    report(probe);
    return G_SOURCE_REMOVE;
}

int main(int argc, char** argv)
{
    Probe probe = { .settleSeconds = 6 };
    guint timeoutSeconds = 60;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--settle") && i + 1 < argc)
            probe.settleSeconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc)
            timeoutSeconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc)
            probe.snapshotPath = argv[++i];
        else if (!strcmp(argv[i], "--eval") && i + 1 < argc)
            probe.evalScript = argv[++i];
        else if (!strcmp(argv[i], "--no-web-security"))
            probe.noWebSecurity = TRUE;
        else
            probe.url = argv[i];
    }
    if (!probe.url) {
        fprintf(stderr, "usage: %s [--settle SECONDS] [--timeout SECONDS] [--snapshot FILE] [--eval SCRIPT] [--no-web-security] URL\n", argv[0]);
        return 2;
    }

    WPEDisplay* display = wpe_display_headless_new();
    WebKitNetworkSession* session = webkit_network_session_new_ephemeral();
    WebKitUserContentManager* content = webkit_user_content_manager_new();
    WebKitUserScript* script = webkit_user_script_new(errorHookScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(content, script);
    webkit_user_script_unref(script);

    probe.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "display", display,
        "network-session", session,
        "user-content-manager", content,
        NULL));
    if (probe.noWebSecurity)
        webkit_settings_set_disable_web_security(webkit_web_view_get_settings(probe.view), TRUE);
    WPEView* wpeView = webkit_web_view_get_wpe_view(probe.view);
    if (wpeView && wpe_view_get_toplevel(wpeView))
        wpe_toplevel_resize(wpe_view_get_toplevel(wpeView), VIEW_WIDTH, VIEW_HEIGHT);

    g_signal_connect(probe.view, "load-changed", G_CALLBACK(loadChanged), &probe);
    g_signal_connect(probe.view, "load-failed", G_CALLBACK(loadFailed), &probe);
    g_signal_connect(probe.view, "load-failed-with-tls-errors", G_CALLBACK(loadFailedWithTLSErrors), &probe);
    g_signal_connect(probe.view, "web-process-terminated", G_CALLBACK(webProcessTerminated), &probe);

    probe.loop = g_main_loop_new(NULL, FALSE);
    probe.startTime = g_get_monotonic_time();
    g_timeout_add_seconds(timeoutSeconds, timedOut, &probe);
    g_timeout_add_seconds(timeoutSeconds + probe.settleSeconds + 20, gaveUp, &probe);
    webkit_web_view_load_uri(probe.view, probe.url);
    g_main_loop_run(probe.loop);
    return 0;
}
