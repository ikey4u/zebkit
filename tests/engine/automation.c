/* Playwright's commands, natively (<zebkit/automation.h>), on
 * tests/engine/pages/automation.html in an off-screen view:
 *
 * - navigation: Frame.goto waiting for the load, its response, a failed
 *   load; Page.reload, goBack, goForward; Frame.setContent;
 * - reads that wait for their element: textContent, innerText, innerHTML,
 *   getAttribute, inputValue, the element states, queryCount, isVisible,
 *   isHidden, waitForSelector, title; role, text and CSS selectors;
 * - actions as Playwright does them: a click, one that waits until a
 *   button is enabled, one that waits until what covers the button goes,
 *   one that scrolls; fill, check, uncheck, hover, focus, press, type;
 * - keyboard and mouse: trusted keys, modifiers, text inserted, a click at
 *   a point;
 * - errors: strict mode, a timeout with its call log, an unknown key, a
 *   command that does not exist; Page.screenshot.
 * - handles: an ElementHandle queried and acted on, JSHandle properties
 *   and values, an evaluation given a handle, evalOnSelector;
 * - frames: a button in an <iframe> clicked through a frame locator;
 * - selectOption, setInputFiles from a path and from contents, expect,
 *   ariaSnapshot, waitForFunction;
 * - events: dialogs held and accepted, a file chooser answered with
 *   FileChooser.setFiles, navigations.
 * - the network: a route fulfilled by the client, responses and their
 *   bodies, offline; the profile: cookies, a request through the engine's
 *   network, permissions, a download; an exposed function answered by
 *   the client; Playwright's clock; media emulation.
 *
 *   ZEBKIT_TEST_BASE   http://127.0.0.1:<port>, serving tests/engine/pages
 *   ZEBKIT_TEST_FILES  a directory for the file uploaded
 *
 * With ZEBKIT_AUTOMATION_BENCH=<runs>, the timings of docs/AUTOMATION.md's
 * table instead, as JSON on standard output (scripts/automation-bench.sh).
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
#include <unistd.h>

#include <zebkit/automation.h>
#include <zebkit/offscreen.h>

static int failures, checks;

static void check(int ok, const char *what, const char *format, ...) {
    ++checks;
    if (!ok)
        ++failures;
    char detail[1024] = "";
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
static char base[256];
/* The last handle an answer gave ("h3"), for "@H" in the next params; the
 * file uploaded, for "@F"; the last dialog and file chooser, for "@D" and
 * "@C". */
static char last_handle[32], upload_path[512], last_dialog[32], last_chooser[32];
/* The last route, binding call and download ("@R", "@B", "@W"), and the
 * request of automation.html's last response ("@Q"). */
static char last_route[32], last_binding[32], last_download[32], page_request[32];
static char last_context[32], last_worker[32], last_har[32];
static char events[16384];

/* ---- The commands and what they should answer ---------------------------- */

typedef struct {
    const char *what;
    const char *method;
    /* %s is the base URL, where there is one. */
    const char *params;
    /* A part of the answer's JSON, or with "!" first, of the error. NULL
     * for a step whose answer is not checked. */
    const char *expect;
} command;

static const command commands[] = {
    {"goto waits for the load", "Frame.goto", "{\"url\": \"%s/automation.html\"}", "\"status\":200"},
    {"title", "Frame.title", NULL, "{\"value\":\"Automation\"}"},
    {"textContent", "Frame.textContent", "{\"selector\": \"#out\", \"strict\": true}", "{\"value\":\"none\"}"},
    {"click", "Frame.click", "{\"selector\": \"#go\", \"strict\": true}", "{}"},
    {"the click reached the page", "Frame.textContent", "{\"selector\": \"#out\"}", "\"clicked 1\""},
    {"a role selector", "Frame.click", "{\"selector\": \"internal:role=button[name=\\\"Go\\\"i]\", \"strict\": true}", "{}"},
    {"a text selector", "Frame.textContent", "{\"selector\": \"text=clicked 2\"}", "\"clicked 2\""},
    {"dblclick", "Frame.dblclick", "{\"selector\": \"#go\"}", "{}"},
    {"a double click is two clicks", "Frame.textContent", "{\"selector\": \"#out\"}", "\"clicked 4\""},
    {"queryCount", "Frame.queryCount", "{\"selector\": \".item\"}", "{\"value\":3}"},
    {"strict mode", "Frame.textContent", "{\"selector\": \".item\", \"strict\": true}", "!strict mode violation"},
    {"innerText", "Frame.innerText", "{\"selector\": \".item >> nth=1\"}", "\"two\""},
    {"innerHTML", "Frame.innerHTML", "{\"selector\": \"ul\"}", "li class=\\\"item\\\">one"},
    {"getAttribute", "Frame.getAttribute", "{\"selector\": \"#name\", \"name\": \"placeholder\"}", "\"Name\""},
    {"isVisible", "Frame.isVisible", "{\"selector\": \"#go\"}", "{\"value\":true}"},
    {"isVisible of a hidden element", "Frame.isVisible", "{\"selector\": \"#hidden\"}", "{\"value\":false}"},
    {"isHidden of no element", "Frame.isHidden", "{\"selector\": \"#nothing\"}", "{\"value\":true}"},
    {"isEnabled", "Frame.isEnabled", "{\"selector\": \"#later\"}", "{\"value\":false}"},
    {"isEditable", "Frame.isEditable", "{\"selector\": \"#name\"}", "{\"value\":true}"},
    {"fill", "Frame.fill", "{\"selector\": \"#name\", \"value\": \"Ada Lovelace\"}", "{}"},
    {"inputValue", "Frame.inputValue", "{\"selector\": \"#name\"}", "\"Ada Lovelace\""},
    {"a listener for the fill's input event", "Frame.evaluateExpression",
     "{\"expression\": \"() => { document.querySelector('#notes').addEventListener('input', e => window.trusted = e.isTrusted, {once: true}); }\", \"isFunction\": true}",
     NULL},
    {"fill a textarea", "Frame.fill", "{\"selector\": \"#notes\", \"value\": \"line\"}", "{}"},
    {"the fill's input event was trusted", "Frame.evaluateExpression", "{\"expression\": \"window.trusted\"}", "{\"value\":true}"},
    {"fill to empty", "Frame.fill", "{\"selector\": \"#name\", \"value\": \"\"}", "{}"},
    {"inputValue after", "Frame.inputValue", "{\"selector\": \"#name\"}", "{\"value\":\"\"}"},
    {"type", "Frame.type", "{\"selector\": \"#name\", \"text\": \"Hi!\"}", "{}"},
    {"press", "Frame.press", "{\"selector\": \"#name\", \"key\": \"Shift+A\"}", "{}"},
    {"the keys were trusted", "Frame.textContent", "{\"selector\": \"#keys\"}", "\"Delete H i ! Shift A\""},
    {"the typed text", "Frame.inputValue", "{\"selector\": \"#name\"}", "\"Hi!A\""},
    {"Page.keyboardPress", "Page.keyboardPress", "{\"key\": \"Backspace\"}", "{}"},
    {"Page.keyboardInsertText", "Page.keyboardInsertText", "{\"text\": \"\xc3\xa9\xe2\x9c\x93\"}", "{}"},
    {"Backspace and inserted text", "Frame.inputValue", "{\"selector\": \"#name\"}", "\"Hi!\xc3\xa9\xe2\x9c\x93\""},
    {"an unknown key", "Page.keyboardPress", "{\"key\": \"NoSuchKey\"}", "!Unknown key: \"NoSuchKey\""},
    {"check", "Frame.check", "{\"selector\": \"#agree\"}", "{}"},
    {"isChecked", "Frame.isChecked", "{\"selector\": \"#agree\"}", "{\"value\":true}"},
    {"check, already checked", "Frame.check", "{\"selector\": \"#agree\"}", "{}"},
    {"uncheck", "Frame.uncheck", "{\"selector\": \"#agree\"}", "{}"},
    {"isChecked after", "Frame.isChecked", "{\"selector\": \"#agree\"}", "{\"value\":false}"},
    {"a label checks its box", "Frame.check", "{\"selector\": \"text=I agree\"}", "{}"},
    {"the box checked", "Frame.isChecked", "{\"selector\": \"#agree\"}", "{\"value\":true}"},
    {"a button enabled later", "Frame.evaluateExpression",
     "{\"expression\": \"() => setTimeout(() => { const b = document.querySelector('#later'); b.disabled = false; b.onclick = () => out.textContent = 'later'; }, 300)\", \"isFunction\": true}",
     NULL},
    {"click waits until it is enabled", "Frame.click", "{\"selector\": \"#later\"}", "{}"},
    {"the enabled button clicked", "Frame.textContent", "{\"selector\": \"#out\"}", "\"later\""},
    {"a cover over a button", "Frame.evaluateExpression",
     "{\"expression\": \"() => { const c = document.createElement('div'); c.id = 'cover'; document.body.append(c); setTimeout(() => c.remove(), 300); }\", \"isFunction\": true}",
     NULL},
    {"click waits until nothing covers it", "Frame.click", "{\"selector\": \"#under\"}", "{}"},
    {"the covered button clicked, not the cover", "Frame.textContent", "{\"selector\": \"#out\"}", "\"under\""},
    {"click scrolls to a far button", "Frame.click", "{\"selector\": \"#far\"}", "{}"},
    {"the far button clicked", "Frame.textContent", "{\"selector\": \"#out\"}", "\"far\""},
    {"hover", "Frame.hover", "{\"selector\": \"#go\"}", "{}"},
    {"focus", "Frame.focus", "{\"selector\": \"#notes\"}", "{}"},
    {"the element focused", "Frame.evaluateExpression", "{\"expression\": \"document.activeElement.id\"}", "{\"value\":\"notes\"}"},
    {"evaluate with an argument", "Frame.evaluateExpression",
     "{\"expression\": \"(a) => a.x * 2\", \"isFunction\": true, \"arg\": {\"x\": 21}}", "{\"value\":42}"},
    {"evaluate that throws", "Frame.evaluateExpression", "{\"expression\": \"() => { throw new Error('boom') }\", \"isFunction\": true}", "!Error: boom"},
    {"Page.mouseClick at a point", "Page.mouseClick", "{\"x\": 5, \"y\": 5}", "{}"},
    {"waitForSelector, visible", "Frame.waitForSelector", "{\"selector\": \"#go\"}", "\"element\":{\"element\":true"},
    {"waitForSelector, hidden", "Frame.waitForSelector", "{\"selector\": \"#hidden\", \"state\": \"hidden\"}", "{}"},
    {"a timeout with its call log", "Frame.click", "{\"selector\": \"#nothing\", \"timeout\": 300}",
     "!Timeout 300ms exceeded.\nCall log:\n  - waiting for #nothing"},
    {"a link navigates in the document", "Frame.click", "{\"selector\": \"#link\"}", "{}"},
    {"the fragment", "Frame.evaluateExpression", "{\"expression\": \"location.hash\"}", "\"#second\""},
    {"goBack", "Page.goBack", NULL, "{"},
    {"reload", "Page.reload", NULL, "\"status\":200"},
    {"the reloaded page", "Frame.textContent", "{\"selector\": \"#out\"}", "\"none\""},
    {"setContent", "Frame.setContent", "{\"html\": \"<p id=p>written</p>\"}", "{}"},
    {"the content written", "Frame.textContent", "{\"selector\": \"#p\"}", "\"written\""},
    {"content", "Frame.content", NULL, "p id=\\\"p\\\">written"},
    {"a failed load (a port Chromium refuses)", "Frame.goto", "{\"url\": \"http://127.0.0.1:1/\"}", "!net::ERR_UNSAFE_PORT"},
    {"setViewportSize", "Page.setViewportSize", "{\"viewportSize\": {\"width\": 640, \"height\": 400}}", "{}"},
    {"the viewport", "Frame.evaluateExpression", "{\"expression\": \"innerWidth + 'x' + innerHeight\"}", "\"640x400\""},
    {"Page.screenshot", "Page.screenshot", NULL, "\"binary\":\"iVBORw0KGgo"},
    {"a command that does not exist", "Frame.nothing", NULL, "!zebkit: no command Frame.nothing"},
    {"back to the page", "Frame.goto", "{\"url\": \"%s/automation.html\"}", "\"status\":200"},
    {"querySelector gives a handle", "Frame.querySelector", "{\"selector\": \"#go\"}", "button id=\\\"go\\\""},
    {"ElementHandle.textContent", "ElementHandle.textContent", "{\"handle\": \"@H\"}", "{\"value\":\"Go\"}"},
    {"ElementHandle.click", "ElementHandle.click", "{\"handle\": \"@H\"}", "{}"},
    {"ElementHandle.boundingBox", "ElementHandle.boundingBox", "{\"handle\": \"@H\"}", "\"width\":"},
    {"evaluate given the handle", "Frame.evaluateExpression",
     "{\"expression\": \"(b) => b.id + ':' + b.isConnected\", \"isFunction\": true, \"arg\": {\"handle\": \"@H\"}}", "{\"value\":\"go:true\"}"},
    {"evaluateExpressionHandle", "Frame.evaluateExpressionHandle",
     "{\"expression\": \"() => ({a: 1, b: [2, 3]})\", \"isFunction\": true}", "\"handle\":{\"handle\":\"h"},
    {"JSHandle.getProperty", "JSHandle.getProperty", "{\"handle\": \"@H\", \"name\": \"b\"}", "\"preview\":\"Array(2)\""},
    {"JSHandle.jsonValue", "JSHandle.jsonValue", "{\"handle\": \"@H\"}", "{\"value\":[2,3]}"},
    {"JSHandle.dispose", "JSHandle.dispose", "{\"handle\": \"@H\"}", "{}"},
    {"a disposed handle", "JSHandle.jsonValue", "{\"handle\": \"@H\"}", "!JSHandle is disposed"},
    {"evalOnSelector", "Frame.evalOnSelector",
     "{\"selector\": \".item >> nth=2\", \"expression\": \"(e, x) => e.textContent + x\", \"isFunction\": true, \"arg\": \"!\"}", "{\"value\":\"three!\"}"},
    {"evalOnSelectorAll", "Frame.evalOnSelectorAll",
     "{\"selector\": \".item\", \"expression\": \"(es) => es.length\", \"isFunction\": true}", "{\"value\":3}"},
    {"a click in an iframe", "Frame.click", "{\"selector\": \"#inner >> internal:control=enter-frame >> #deep\"}", "{}"},
    {"the iframe's button clicked", "Frame.textContent", "{\"selector\": \"#inner >> internal:control=enter-frame >> #deep\"}", "{\"value\":\"pressed\"}"},
    {"Page.frames", "Page.frames", NULL, "\"parentFrame\""},
    {"selectOption by label", "Frame.selectOption", "{\"selector\": \"#color\", \"options\": [{\"valueOrLabel\": \"Green\"}]}", "{\"values\":[\"g\"]}"},
    {"the option selected", "Frame.inputValue", "{\"selector\": \"#color\"}", "{\"value\":\"g\"}"},
    {"setInputFiles from a path", "Frame.setInputFiles", "{\"selector\": \"#upload\", \"localPaths\": [\"@F\"]}", "{}"},
    {"the file chosen", "Frame.evaluateExpression",
     "{\"expression\": \"() => document.querySelector('#upload').files[0].name\", \"isFunction\": true}", "-upload.txt\"}"},
    {"setInputFiles from contents", "Frame.setInputFiles",
     "{\"selector\": \"#upload\", \"payloads\": [{\"name\": \"a.txt\", \"mimeType\": \"text/plain\", \"buffer\": \"aGk=\"}, {\"name\": \"b.txt\", \"mimeType\": \"text/plain\", \"buffer\": \"aGk=\"}]}", "{}"},
    {"the files given", "Frame.evaluateExpression",
     "{\"expression\": \"() => [...document.querySelector('#upload').files].map(f => f.name).join()\", \"isFunction\": true}", "{\"value\":\"a.txt,b.txt\"}"},
    {"expect toHaveText", "Frame.expect",
     "{\"selector\": \"#out\", \"expression\": \"to.have.text\", \"expectedText\": [{\"string\": \"clicked 1\"}], \"isNot\": false}", "\"matches\":true"},
    {"expect toHaveCount, failing", "Frame.expect",
     "{\"selector\": \".item\", \"expression\": \"to.have.count\", \"expectedNumber\": 5, \"isNot\": false, \"timeout\": 300}", "\"timedOut\":true"},
    {"ariaSnapshot", "Frame.ariaSnapshot", "{\"selector\": \"ul\"}", "- listitem: one"},
    {"waitForFunction", "Frame.waitForFunction",
     "{\"expression\": \"() => { setTimeout(() => window.ready = 7, 100); return window.ready; }\", \"isFunction\": true}", "\"handle\":{\"handle\""},
    {"dialogs held", "Page.updateSubscription", "{\"event\": \"dialog\", \"enabled\": true}", "{}"},
    {"a confirm", "Frame.evaluateExpression",
     "{\"expression\": \"() => { setTimeout(() => window.answer = confirm('Sure?'), 0); }\", \"isFunction\": true}", "{"},
    {"the dialog event", "@event", NULL, "BrowserContext.dialog {\"defaultValue\":\"\",\"dialog\":\"d"},
    {"Dialog.accept", "Dialog.accept", "{\"dialog\": \"@D\"}", "{}"},
    {"the page got OK", "Frame.waitForFunction", "{\"expression\": \"() => window.answer === true\", \"isFunction\": true, \"timeout\": 2000}", "\"handle\""},
    {"file choosers taken", "Page.updateSubscription", "{\"event\": \"fileChooser\", \"enabled\": true}", "{}"},
    {"a click opens a file chooser", "Frame.click", "{\"selector\": \"#upload\"}", "{}"},
    {"the file chooser event", "@event", NULL, "Page.fileChooser {\"fileChooser\":\"c"},
    {"FileChooser.setFiles", "FileChooser.setFiles", "{\"fileChooser\": \"@C\", \"localPaths\": [\"@F\"]}", "{}"},
    {"the chosen file", "Frame.waitForFunction",
     "{\"expression\": \"() => document.querySelector('#upload').files.length === 1 && document.querySelector('#upload').files[0].name\", \"isFunction\": true, \"timeout\": 2000}", "\"handle\""},
    {"a navigation event", "@event", NULL, "Frame.navigated"},
    {"a fresh document, its requests seen", "Frame.goto", "{\"url\": \"%s/automation.html\"}", "\"status\":200"},
    {"the page's response event", "@event", NULL, "Page.response {\"headers\""},
    {"Response.body", "Response.body", "{\"request\": \"@Q\"}", "\"binary\":\"PCFkb2N0eXBl"},
    {"a route", "Page.setNetworkInterceptionPatterns", "{\"patterns\": [{\"glob\": \"**/routed.json\"}]}", "{}"},
    {"a fetch the route holds", "Frame.evaluateExpression",
     "{\"expression\": \"() => { fetch('routed.json').then(r => r.text()).then(t => window.routed = t); }\", \"isFunction\": true}", "{"},
    {"the route event", "@event", NULL, "BrowserContext.route {\"request\""},
    {"Route.fulfill", "Route.fulfill",
     "{\"route\": \"@R\", \"status\": 200, \"headers\": [{\"name\": \"content-type\", \"value\": \"application/json\"}], \"body\": \"eyJhIjoxfQ==\", \"isBase64\": true}", "{}"},
    {"the page got the client's response", "Frame.waitForFunction",
     "{\"expression\": \"() => window.routed === '{\\\"a\\\":1}'\", \"isFunction\": true, \"timeout\": 3000}", "\"handle\""},
    {"no route", "Page.setNetworkInterceptionPatterns", "{\"patterns\": []}", "{}"},
    {"offline", "BrowserContext.setOffline", "{\"offline\": true}", "{}"},
    {"a fetch offline fails", "Frame.evaluateExpression",
     "{\"expression\": \"() => fetch('hello.txt').then(() => 'loaded', () => 'failed')\", \"isFunction\": true}", "{\"value\":\"failed\"}"},
    {"online", "BrowserContext.setOffline", "{\"offline\": false}", "{}"},
    {"addCookies", "BrowserContext.addCookies", "{\"cookies\": [{\"name\": \"flavour\", \"value\": \"oat\", \"url\": \"%s/\"}]}", "{}"},
    {"the page sees the cookie", "Frame.evaluateExpression", "{\"expression\": \"document.cookie\"}", "flavour=oat"},
    {"cookies", "BrowserContext.cookies", NULL, "\"name\":\"flavour\""},
    {"storageState", "BrowserContext.storageState", NULL, "\"cookies\":["},
    {"clearCookies", "BrowserContext.clearCookies", NULL, "{}"},
    {"no cookies", "Frame.evaluateExpression", "{\"expression\": \"document.cookie\"}", "{\"value\":\"\"}"},
    {"APIRequestContext.fetch", "APIRequestContext.fetch", "{\"url\": \"%s/hello.txt\"}", "\"status\":200"},
    {"exposeBinding", "Page.exposeBinding", "{\"name\": \"compute\"}", "{}"},
    {"the page calls it", "Frame.evaluateExpression",
     "{\"expression\": \"() => { compute(2, 3).then(v => window.computed = v); }\", \"isFunction\": true}", "{"},
    {"the binding call", "@event", NULL, "BrowserContext.bindingCall {\"args\":[2,3]"},
    {"BindingCall.resolve", "BindingCall.resolve", "{\"binding\": \"@B\", \"result\": 5}", "{}"},
    {"the page got the answer", "Frame.waitForFunction",
     "{\"expression\": \"() => window.computed === 5\", \"isFunction\": true, \"timeout\": 3000}", "\"handle\""},
    {"clock installed", "BrowserContext.clockInstall", "{\"timeNumber\": 0}", "{}"},
    {"clock fixed", "BrowserContext.clockSetFixedTime", "{\"timeNumber\": 1000}", "{}"},
    {"the page's time", "Frame.evaluateExpression", "{\"expression\": \"Date.now()\"}", "{\"value\":1000}"},
    {"grantPermissions", "BrowserContext.grantPermissions", "{\"permissions\": [\"geolocation\"]}", "{}"},
    {"the page has it", "Frame.evaluateExpression",
     "{\"expression\": \"() => navigator.permissions.query({name: 'geolocation'}).then(r => r.state)\", \"isFunction\": true}", "{\"value\":\"granted\"}"},
    {"the others denied", "Frame.evaluateExpression",
     "{\"expression\": \"() => navigator.permissions.query({name: 'camera'}).then(r => r.state)\", \"isFunction\": true}", "{\"value\":\"denied\"}"},
    {"clearPermissions", "BrowserContext.clearPermissions", NULL, "{}"},
    {"emulateMedia", "Page.emulateMedia", "{\"colorScheme\": \"dark\"}", "{}"},
    {"the page is dark", "Frame.waitForFunction",
     "{\"expression\": \"() => matchMedia('(prefers-color-scheme: dark)').matches\", \"isFunction\": true, \"timeout\": 3000}", "\"handle\""},
    {"a download", "Frame.evaluateExpression",
     "{\"expression\": \"() => { const a = document.createElement('a'); a.download = 'note.txt'; a.href = 'hello.txt'; document.body.append(a); a.click(); }\", \"isFunction\": true}", "{"},
    {"the download event", "@event", NULL, "Page.download {\"download\":\"w"},
    {"Artifact.pathAfterFinished", "Artifact.pathAfterFinished", "{\"download\": \"@W\"}", "note.txt\"}"},
    {"Artifact.failure", "Artifact.failure", "{\"download\": \"@W\"}", "{}"},
    /* Contexts of their own: a profile each, pages in them. */
    {"Browser.newContext", "Browser.newContext", "{\"viewport\": {\"width\": 640, \"height\": 480}}", "\"context\":\"x"},
    {"BrowserContext.newPage", "BrowserContext.newPage", "{\"context\": \"@X\"}", "\"page\":\"p"},
    {"BrowserContext.pages", "BrowserContext.pages", "{\"context\": \"@X\"}", "\"p"},
    {"a context's cookies are its own", "BrowserContext.addCookies",
     "{\"context\": \"@X\", \"cookies\": [{\"name\": \"only\", \"value\": \"x\", \"url\": \"%s/\"}]}", "{}"},
    {"the context's cookies", "BrowserContext.cookies", "{\"context\": \"@X\"}", "\"only\""},
    {"BrowserContext.close", "BrowserContext.close", "{\"context\": \"@X\"}", "{}"},
    /* Geolocation. */
    {"grantPermissions geolocation", "BrowserContext.grantPermissions", "{\"permissions\": [\"geolocation\"]}", "{}"},
    {"setGeolocation", "BrowserContext.setGeolocation", "{\"geolocation\": {\"latitude\": 59.95, \"longitude\": 30.31}}", "{}"},
    {"the page reads the position", "Frame.evaluateExpression",
     "{\"expression\": \"() => new Promise((ok, no) => navigator.geolocation.getCurrentPosition(p => ok(p.coords.latitude), e => no(e.message)))\", \"isFunction\": true}",
     "{\"value\":59.95}"},
    /* Touch. */
    {"a touch listener", "Frame.evaluateExpression",
     "{\"expression\": \"() => { window.touched = 0; document.addEventListener('touchstart', e => window.touched = e.touches.length); }\", \"isFunction\": true}", "{"},
    {"touchscreenTap", "Page.touchscreenTap", "{\"x\": 20, \"y\": 20}", "{}"},
    {"the page saw a touch", "Frame.evaluateExpression", "{\"expression\": \"window.touched\"}", "{\"value\":1}"},
    /* Workers. */
    {"a worker", "Frame.evaluateExpression",
     "{\"expression\": \"() => { window.worker = new Worker(URL.createObjectURL(new Blob(['self.x = 7']))); }\", \"isFunction\": true}", "{"},
    {"the worker event", "@event", NULL, "Page.worker {\"frame\""},
    {"Worker.evaluateExpression", "Worker.evaluateExpression", "{\"worker\": \"@K\", \"expression\": \"self.x * 6\"}", "{\"value\":42}"},
    /* Tracing, HAR, video. */
    {"tracingStart", "Tracing.tracingStart", "{\"screenshots\": true, \"snapshots\": true}", "{}"},
    {"tracingStartChunk", "Tracing.tracingStartChunk", NULL, "{"},
    {"a traced click", "Frame.click", "{\"selector\": \"#go\"}", "{}"},
    {"tracingStopChunk writes the zip", "Tracing.tracingStopChunk", "{\"mode\": \"archive\"}", ".zip\"}"},
    {"tracingStop", "Tracing.tracingStop", NULL, "{}"},
    {"harStart", "Tracing.harStart", "{\"options\": {\"content\": \"embed\"}}", "\"harId\""},
    {"a page for the HAR", "Frame.goto", "{\"url\": \"%s/automation.html\"}", "\"status\":200"},
    {"harExport writes the HAR", "Tracing.harExport", "{\"harId\": \"@A\"}", ".har\"}"},
    {"startVideo", "Page.startVideo", "{\"size\": {\"width\": 320, \"height\": 240}}", ".webm\"}"},
    {"some frames", "Frame.waitForTimeout", "{\"timeout\": 400}", "{}"},
    {"stopVideo writes the WebM", "Page.stopVideo", NULL, ".webm\"}"},
};

static const int command_count = sizeof(commands) / sizeof(commands[0]);
static int current;

static void run_next(void *data);

/* @from replaced by @to in @text, every time. */
static void replace(char *text, size_t size, const char *from, const char *to) {
    char out[4096];
    size_t used = 0;
    for (const char *p = text; *p && used + 1 < sizeof(out);) {
        if (strncmp(p, from, strlen(from)) == 0) {
            used += snprintf(out + used, sizeof(out) - used, "%s", to);
            p += strlen(from);
        } else {
            out[used++] = *p++;
        }
    }
    out[used] = 0;
    snprintf(text, size, "%s", out);
}

/* "@event": an event, seen within 3 s. */
static const command *event_wanted;
static int64_t event_until;
static void wait_event(void *data) {
    (void)data;
    const int seen = strstr(events, event_wanted->expect) != NULL;
    if (seen || now_us() > event_until) {
        check(seen, event_wanted->what, "%s", seen ? event_wanted->expect : events);
        zebkit_post_task(run_next, NULL);
        return;
    }
    zebkit_post_delayed_task(wait_event, NULL, 20);
}

/* @key's value in @json, a string ("handle": "h3") or NULL. */
static int find_string(const char *json, const char *key, char *out, size_t size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *at = json ? strstr(json, pattern) : NULL;
    if (!at)
        return 0;
    at += strlen(pattern);
    size_t n = 0;
    while (at[n] && at[n] != '"' && n + 1 < size)
        ++n;
    memcpy(out, at, n);
    out[n] = 0;
    return 1;
}

static void on_event(zebkit_view *v, const char *event, const char *json, void *data) {
    (void)v, (void)data;
    size_t used = strlen(events);
    snprintf(events + used, sizeof(events) - used, "%s %s\n", event, json);
    if (strcmp(event, "BrowserContext.dialog") == 0)
        find_string(json, "dialog", last_dialog, sizeof(last_dialog));
    if (strcmp(event, "Page.fileChooser") == 0)
        find_string(json, "fileChooser", last_chooser, sizeof(last_chooser));
    if (strcmp(event, "BrowserContext.route") == 0)
        find_string(json, "route", last_route, sizeof(last_route));
    if (strcmp(event, "BrowserContext.bindingCall") == 0)
        find_string(json, "binding", last_binding, sizeof(last_binding));
    if (strcmp(event, "Page.worker") == 0)
        find_string(json, "worker", last_worker, sizeof(last_worker));
    if (strcmp(event, "Page.download") == 0)
        find_string(json, "download", last_download, sizeof(last_download));
    if (strcmp(event, "Page.response") == 0 && strstr(json, "automation.html\""))
        find_string(json, "request", page_request, sizeof(page_request));
}

static void answered(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v;
    const command *c = data;
    if (json && strstr(json, "\"handle\":\"h"))
        find_string(json, "handle", last_handle, sizeof(last_handle));
    if (json && strcmp(c->method, "Browser.newContext") == 0)
        find_string(json, "context", last_context, sizeof(last_context));
    if (json && strcmp(c->method, "Tracing.harStart") == 0)
        find_string(json, "harId", last_har, sizeof(last_har));
    if (c->expect) {
        const int want_error = c->expect[0] == '!';
        const char *expected = want_error ? c->expect + 1 : c->expect;
        const char *got = want_error ? error : json;
        check(got && strstr(got, expected) != NULL, c->what, "%s", json ? json : error ? error : "(nothing)");
    } else {
        check(json != NULL, c->what, "%s", json ? json : error);
    }
    zebkit_post_task(run_next, NULL);
}

static void run_next(void *data) {
    (void)data;
    if (current >= command_count) {
        printf("%d of %d checks passed\n", checks - failures, checks);
        zebkit_quit();
        return;
    }
    const command *c = &commands[current++];
    if (strcmp(c->method, "@event") == 0) {
        event_wanted = c;
        event_until = now_us() + 3000000;
        zebkit_post_task(wait_event, NULL);
        return;
    }
    char format[2048], params[4096];
    snprintf(format, sizeof(format), "%s", c->params ? c->params : "{}");
    snprintf(params, sizeof(params), format, base);
    replace(params, sizeof(params), "@H", last_handle);
    replace(params, sizeof(params), "@F", upload_path);
    replace(params, sizeof(params), "@D", last_dialog);
    replace(params, sizeof(params), "@C", last_chooser);
    replace(params, sizeof(params), "@R", last_route);
    replace(params, sizeof(params), "@B", last_binding);
    replace(params, sizeof(params), "@W", last_download);
    replace(params, sizeof(params), "@Q", page_request);
    replace(params, sizeof(params), "@X", last_context);
    replace(params, sizeof(params), "@K", last_worker);
    replace(params, sizeof(params), "@A", last_har);
    /* A context's commands name it, with no view. */
    zebkit_automation_call(strstr(params, "\"context\": \"x") ? NULL : view, c->method, params, answered, (void *)c);
}

/* ---- The benchmark ---------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *method;
    const char *params;
} timed;

/* docs/AUTOMATION.md's table, on the page of scripts/automation-bench.sh. */
static const timed timings[] = {
    {"goto", "Frame.goto", "{\"url\": \"%s/\"}"},
    {"evaluate 1+1", "Frame.evaluateExpression", "{\"expression\": \"() => 1 + 1\", \"isFunction\": true}"},
    {"locator.textContent", "Frame.textContent", "{\"selector\": \"#out\", \"strict\": true}"},
    {"locator.click", "Frame.click", "{\"selector\": \"#go\", \"strict\": true}"},
    {"locator.fill", "Frame.fill", "{\"selector\": \"#name\", \"strict\": true, \"value\": \"hello world\"}"},
    {"locator.press Enter", "Frame.press", "{\"selector\": \"#name\", \"strict\": true, \"key\": \"Enter\"}"},
    {"locator.isVisible", "Frame.isVisible", "{\"selector\": \"#go\", \"strict\": true}"},
    {"locator.count (200)", "Frame.queryCount", "{\"selector\": \".item\"}"},
    {"allTextContents (200)", "Frame.evaluateExpression",
     "{\"expression\": \"() => [...document.querySelectorAll('.item')].map(e => e.textContent)\", \"isFunction\": true}"},
    {"getByRole click", "Frame.click", "{\"selector\": \"internal:role=button[name=\\\"Go\\\"i]\", \"strict\": true}"},
    {"screenshot", "Page.screenshot", NULL},
};
static const int timing_count = sizeof(timings) / sizeof(timings[0]);
static int bench_runs = 50, bench_index, bench_run;
static double bench_times[200];
static int64_t bench_start;
static char bench_page[256];

static int compare(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void bench_next(void *data);

static void bench_answered(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)json, (void)data;
    const int runs = strcmp(timings[bench_index].name, "goto") == 0 || strcmp(timings[bench_index].name, "screenshot") == 0
                         ? (bench_runs < 20 ? bench_runs : 20)
                         : bench_runs;
    if (error) {
        fprintf(stderr, "%s: %s\n", timings[bench_index].name, error);
        ++failures;
    }
    /* Three warm-up runs first, as the Playwright side has. */
    if (bench_run >= 3)
        bench_times[bench_run - 3] = (now_us() - bench_start) / 1000.0;
    if (++bench_run >= runs + 3) {
        qsort(bench_times, runs, sizeof(double), compare);
        printf("%s\"%s\": {\"median\": %.2f, \"p90\": %.2f}", bench_index ? ", " : "{\"engine\": \"zebkit native\", \"results\": {",
               timings[bench_index].name, bench_times[runs / 2], bench_times[runs * 9 / 10]);
        bench_run = 0;
        ++bench_index;
    }
    zebkit_post_task(bench_next, NULL);
}

static void bench_next(void *data) {
    (void)data;
    if (bench_index >= timing_count) {
        printf("}}\n");
        zebkit_quit();
        return;
    }
    char params[1024];
    snprintf(params, sizeof(params), timings[bench_index].params ? timings[bench_index].params : "{}", bench_page);
    bench_start = now_us();
    zebkit_automation_call(view, timings[bench_index].method, params, bench_answered, NULL);
}

static void bench_loaded(zebkit_view *v, const char *json, const char *error, void *data) {
    (void)v, (void)json, (void)data;
    if (error) {
        fprintf(stderr, "the page did not load: %s\n", error);
        failures = 1;
        zebkit_quit();
        return;
    }
    zebkit_post_task(bench_next, NULL);
}

static int benchmark;

static void ready(void *data) {
    (void)data;
    view = zebkit_view_new_offscreen(NULL, 1280, 800, 1);
    if (benchmark) {
        char params[512];
        snprintf(bench_page, sizeof(bench_page), "%s", base);
        snprintf(params, sizeof(params), "{\"url\": \"%s/\"}", base);
        zebkit_automation_call(view, "Frame.goto", params, bench_loaded, NULL);
        return;
    }
    const char *files = getenv("ZEBKIT_TEST_FILES");
    snprintf(upload_path, sizeof(upload_path), "%s/zebkit-automation-%d-upload.txt", files && *files ? files : "/tmp",
             (int)getpid());
    FILE *upload = fopen(upload_path, "w");
    if (upload) {
        fputs("uploaded", upload);
        fclose(upload);
    }
    zebkit_automation_on_event(view, on_event, NULL);
    char *names = zebkit_automation_commands();
    check(names && strstr(names, "\"Frame.click\""), "the commands", "%.80s...", names ? names : "");
    zebkit_free(names);
    zebkit_post_task(run_next, NULL);
}

int main(int argc, char **argv) {
    const char *env = getenv("ZEBKIT_TEST_BASE");
    snprintf(base, sizeof(base), "%s", env ? env : "http://127.0.0.1:8000");
    const char *runs = getenv("ZEBKIT_AUTOMATION_BENCH");
    if (runs && *runs) {
        benchmark = 1;
        bench_runs = atoi(runs);
        if (bench_runs < 1 || bench_runs > 190)
            bench_runs = 50;
    }
    int code = zebkit_main(argc, argv, ready, NULL);
    if (code)
        return code;
    if (benchmark)
        return failures ? 1 : 0;
    return failures || checks < command_count ? 1 : 0;
}
