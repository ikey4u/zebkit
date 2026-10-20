/* URL schemes of the application's own (zebkit_scheme_*): a page served
 * from app:, which fetches from app: and reads a file from asset:, whole
 * and in a range, as a player seeking does. Prints one line per check;
 * exit 0 when all pass. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

static int failures, checks;
static char file_path[1024];

static void check(int ok, const char *what, const char *detail) {
    checks++;
    if (!ok)
        failures++;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static const char page[] =
    "<!doctype html><title>loading</title><script>\n"
    "(async () => {\n"
    "  const results = {};\n"
    "  results.origin = location.origin;\n"
    "  results.secure = isSecureContext;\n"
    "  const data = await fetch('app://ui/data.json').then(r => r.json());\n"
    "  results.data = data.answer;\n"
    "  const posted = await fetch('app://ui/echo', {method: 'POST', body: 'zebkit'}).then(r => r.text());\n"
    "  results.posted = posted;\n"
    "  const whole = await fetch('asset://files/numbers.txt').then(r => r.text());\n"
    "  results.whole = whole;\n"
    "  const part = await fetch('asset://files/numbers.txt', {headers: {Range: 'bytes=3-5'}});\n"
    "  results.status = part.status;\n"
    "  results.part = await part.text();\n"
    "  results.range = part.headers.get('Content-Range');\n"
    "  results.missing = (await fetch('asset://files/none.txt')).status;\n"
    "  window.results = results;\n"
    "  document.title = 'done';\n"
    "})().catch(e => { window.results = {error: String(e)}; document.title = 'done'; });\n"
    "</script>";

static void serve_app(zebkit_request *request, void *data) {
    const char *url = zebkit_request_url(request);
    if (strcmp(url, "app://ui/index.html") == 0) {
        zebkit_request_respond(request, 200, "Content-Type: text/html", page, sizeof(page) - 1);
    } else if (strcmp(url, "app://ui/data.json") == 0) {
        static const char json[] = "{\"answer\": 42}";
        zebkit_request_respond(request, 200, "Content-Type: application/json", json, sizeof(json) - 1);
    } else if (strcmp(url, "app://ui/echo") == 0) {
        size_t size = 0;
        const uint8_t *body = zebkit_request_body(request, &size);
        char reply[64];
        snprintf(reply, sizeof(reply), "%s %.*s", zebkit_request_method(request), (int)size,
                 body ? (const char *)body : "");
        zebkit_request_respond(request, 200, "Content-Type: text/plain", reply, strlen(reply));
    } else {
        zebkit_request_respond(request, 404, NULL, NULL, 0);
    }
}

/* Another origin than the page's: the answers say who may read them. */
static const char cors[] = "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: Content-Range";

static void serve_asset(zebkit_request *request, void *data) {
    const char *url = zebkit_request_url(request);
    if (strstr(url, "numbers.txt"))
        zebkit_request_respond_file(request, file_path, cors);
    else
        zebkit_request_respond_file(request, "/no/such/file", cors);
}

static zebkit_view *view;

static void on_result(zebkit_view *v, const char *json, const char *error, void *data) {
    const char *r = json ? json : error;
#define HAS(part) (r && strstr(r, part) != NULL)
    check(HAS("\"origin\":\"app://ui\""), "a standard scheme: the page's origin is app://ui", r);
    check(HAS("\"secure\":true"), "a secure scheme: a secure context", NULL);
    check(HAS("\"data\":42"), "fetch() from the application's scheme", NULL);
    check(HAS("\"posted\":\"POST zebkit\""), "a request's method and body", NULL);
    check(HAS("\"whole\":\"0123456789\""), "zebkit_request_respond_file: the whole file", NULL);
    check(HAS("\"status\":206") && HAS("\"part\":\"345\"") && HAS("\"range\":\"bytes 3-5/10\""),
          "zebkit_request_respond_file: a range", NULL);
    check(HAS("\"missing\":404"), "zebkit_request_respond_file: a file that is not there", NULL);
    zebkit_quit();
}

static void on_title(zebkit_view *v, const char *title, void *data) {
    if (strcmp(title, "done") == 0)
        zebkit_view_evaluate(v, "window.results", on_result, NULL);
}

static void timeout(void *data) {
    check(0, "the page finished", NULL);
    zebkit_quit();
}

static void ready(void *data) {
    zebkit_scheme_handle("app", serve_app, NULL);
    zebkit_scheme_handle("asset", serve_asset, NULL);
    view = zebkit_view_new("app://ui/index.html", 400, 300);
    zebkit_view_on_title(view, on_title, NULL);
    zebkit_post_delayed_task(timeout, NULL, 60000);
}

int main(int argc, char **argv) {
    const char *dir = getenv("ZEBKIT_TEST_FILES");
    snprintf(file_path, sizeof(file_path), "%s/numbers.txt", dir ? dir : ".");
    FILE *file = fopen(file_path, "w");
    if (!file)
        return 1;
    fputs("0123456789", file);
    fclose(file);
    check(zebkit_scheme_register("app", ZEBKIT_SCHEME_STANDARD | ZEBKIT_SCHEME_SECURE | ZEBKIT_SCHEME_FETCH),
          "zebkit_scheme_register", NULL);
    check(zebkit_scheme_register("asset", ZEBKIT_SCHEME_STANDARD | ZEBKIT_SCHEME_FETCH), "a second scheme", NULL);
    check(!zebkit_scheme_register("https", 0), "zebkit_scheme_register refuses https", NULL);
    int code = zebkit_main(argc, argv, ready, NULL);
    printf("%d of %d checks passed\n", checks - failures, checks);
    return failures || code ? 1 : 0;
}
