/*
 * window: a small browser window, on zebkit.
 *
 *   window [URL]
 *
 * A window with a web page in it and, across the top, a back button, a
 * forward button and a box to type an address into. The same program on
 * Linux and macOS.
 *
 * What a host does is all here: start the engine, make a view, load
 * something, and quit when its window closes. The engine chooses the
 * display itself; with none (or --headless) the view has no window, and
 * the page still loads and can be driven over CDP.
 *
 * zebkit draws web pages, and has no buttons or text boxes of its own for
 * an application's frame (zebkit.ui is that, for a script). This program
 * has none either, so the bar is itself a piece of web page: a script the
 * engine runs before every document's own, which calls this program
 * through a function it binds, zebkit.app.bar(). Build against a package
 * (examples/README.md):
 *
 *   cc -std=c11 -I$PKG/include examples/window.c -L$PKG -l:libzebkit.so.1 -Wl,-rpath,$PKG -o window
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zebkit/zebkit.h>

/*
 * The bar, in the top document of every page. It calls
 * zebkit.app.bar("back"), ("forward") and ("go", address); what the
 * buttons can do it reads from the page's own history (the Navigation
 * API), so the host has nothing to tell it.
 */
static const char bar_script[] =
    "(() => {\n"
    "  if (window !== top) return;\n"
    "  const height = 40;\n"
    "  const send = (...args) => globalThis.zebkit?.app?.bar(...args);\n"
    "  const make = (tag, css) => { const e = document.createElement(tag); e.style.cssText = css; return e; };\n"
    "  const host = make('div', 'all:initial;position:fixed;left:0;top:0;width:100%;height:' + height + 'px;z-index:2147483647');\n"
    "  const bar = make('div', 'display:flex;gap:6px;align-items:center;box-sizing:border-box;height:100%;padding:5px 8px;'\n"
    "    + 'background:#f1f3f4;border-bottom:1px solid #c8ccd0;font:14px system-ui,sans-serif');\n"
    "  const button = (label, title, message) => {\n"
    "    const b = make('button', 'width:32px;height:28px;border:1px solid #c8ccd0;border-radius:6px;background:#fff;font:16px system-ui,sans-serif;color:#202124');\n"
    "    b.textContent = label; b.title = title;\n"
    "    b.addEventListener('click', () => send(message));\n"
    "    return b;\n"
    "  };\n"
    "  const back = button('\\u2190', 'Back', 'back');\n"
    "  const forward = button('\\u2192', 'Forward', 'forward');\n"
    "  const address = make('input', 'flex:1;min-width:0;height:28px;box-sizing:border-box;padding:0 10px;'\n"
    "    + 'border:1px solid #c8ccd0;border-radius:14px;background:#fff;font:14px system-ui,sans-serif;color:#202124;outline:none');\n"
    "  address.type = 'text'; address.spellcheck = false; address.placeholder = 'Type an address';\n"
    "  address.value = location.href === 'about:blank' ? '' : location.href;\n"
    "  address.addEventListener('focus', () => address.select());\n"
    "  address.addEventListener('keydown', event => {\n"
    "    event.stopPropagation();\n"
    "    if (event.key === 'Enter' && address.value.trim()) send('go', address.value.trim());\n"
    "  });\n"
    "  for (const type of ['keyup', 'keypress', 'input'])\n"
    "    address.addEventListener(type, event => event.stopPropagation());\n"
    "  bar.append(back, forward, address);\n"
    "  host.attachShadow({ mode: 'closed' }).append(bar);\n"
    "  const update = () => {\n"
    "    const can = globalThis.navigation ?? { canGoBack: true, canGoForward: true };\n"
    "    back.disabled = !can.canGoBack; forward.disabled = !can.canGoForward;\n"
    "    back.style.opacity = can.canGoBack ? 1 : 0.4; forward.style.opacity = can.canGoForward ? 1 : 0.4;\n"
    "  };\n"
    "  const place = () => {\n"
    "    const root = document.documentElement;\n"
    "    if (!root) return;\n"
    "    if (host.parentNode !== root) root.append(host);\n"
    "    root.style.setProperty('margin-top', height + 'px', 'important');\n"
    "    update();\n"
    "  };\n"
    "  place();\n"
    "  document.addEventListener('DOMContentLoaded', place);\n"
    "  addEventListener('load', place);\n"
    "})();\n";

static const char *start_url = "https://example.com/";

/* What was typed is an address if it looks like one; "example.com" means
 * "https://example.com". */
static char *address_from_input(const char *text) {
    size_t length = strlen(text);
    char *address = malloc(length + sizeof "https://");
    if (!address)
        return NULL;
    if (strstr(text, "://") || !strncmp(text, "about:", 6) || !strncmp(text, "data:", 5))
        memcpy(address, text, length + 1);
    else
        snprintf(address, length + sizeof "https://", "https://%s", text);
    return address;
}

/* zebkit.app.bar(...): @args_json is ["back"], ["forward"] or ["go", "<text>"]. */
static void bar(zebkit_view *view, const char *origin, const char *args_json, zebkit_call *call, void *data) {
    (void)origin;
    (void)data;
    if (!strcmp(args_json, "[\"back\"]")) {
        zebkit_view_go_back(view);
    } else if (!strcmp(args_json, "[\"forward\"]")) {
        zebkit_view_go_forward(view);
    } else if (!strncmp(args_json, "[\"go\",", 6)) {
        /* The text, a JSON string: copied with its escapes undone (an
         * address has no \u escapes worth keeping). */
        const char *p = strchr(args_json + 6, '"');
        char typed[2048];
        size_t n = 0;
        for (p = p ? p + 1 : ""; *p && *p != '"' && n + 1 < sizeof typed; p++) {
            if (*p == '\\' && p[1])
                p++;
            typed[n++] = *p;
        }
        typed[n] = '\0';
        char *address = address_from_input(typed);
        if (address) {
            zebkit_view_load_url(view, address);
            free(address);
        }
    }
    zebkit_call_resolve(call, NULL);
}

static void closed(zebkit_view *view, void *data) {
    (void)view;
    (void)data;
    zebkit_quit();
}

static void ready(void *data) {
    (void)data;
    zebkit_view *view = zebkit_view_new(NULL, 1024, 768);
    /* The bar is in every page, so every origin may call zebkit.app.bar(),
     * and nothing else of zebkit.*. */
    zebkit_view_grant(view, "*", ZEBKIT_ALLOW_APP, NULL);
    zebkit_view_bind(view, "bar", bar, NULL);
    zebkit_view_add_script(view, bar_script);
    zebkit_view_on_close(view, closed, NULL);
    zebkit_view_load_url(view, start_url);
}

int main(int argc, char **argv) {
    /* The first argument that is not one of Chromium's switches is the URL. */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            start_url = argv[i];
            break;
        }
    }
    return zebkit_main(argc, argv, ready, NULL);
}
