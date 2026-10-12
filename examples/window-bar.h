/*
 * The bar of the window examples (window.c on Linux, window.m on macOS):
 * a back button, a forward button and an address box, as a script the
 * engine runs in every document. It is the same on both, since it is a
 * piece of web page; what differs is how the host hands it to the engine
 * and hears from it.
 */

#pragma once

/* The JavaScript world the bar lives in, and the name of its channel. */
#define BAR_WORLD "zebkit-window-bar"
#define BAR_CHANNEL "zebkitWindow"

/* Runs at the start of every document of the main frame. It sends "back",
 * "forward", "go <address>" and "ready"; the host calls zebkitBar.update(). */
static const char toolbar_script[] =
    "(() => {\n"
    "  const height = 40;\n"
    "  const send = text => webkit.messageHandlers." BAR_CHANNEL ".postMessage(text);\n"
    "  const make = (tag, css) => { const e = document.createElement(tag); e.style.cssText = css; return e; };\n"
    "  const host = make('div', 'all:initial;position:fixed;left:0;top:0;width:100%;height:' + height + 'px;z-index:2147483647');\n"
    "  const bar = make('div', 'display:flex;gap:6px;align-items:center;box-sizing:border-box;height:100%;padding:5px 8px;'\n"
    "    + 'background:#f1f3f4;border-bottom:1px solid #c8ccd0;font:14px system-ui,sans-serif');\n"
    "  const button = (label, title, message) => {\n"
    "    const b = make('button', 'width:32px;height:28px;border:1px solid #c8ccd0;border-radius:6px;background:#fff;font:16px system-ui,sans-serif;color:#202124');\n"
    "    b.textContent = label; b.title = title; b.id = message;\n"
    "    b.addEventListener('click', () => send(message));\n"
    "    return b;\n"
    "  };\n"
    "  const back = button('\\u2190', 'Back', 'back');\n"
    "  const forward = button('\\u2192', 'Forward', 'forward');\n"
    "  const address = make('input', 'flex:1;min-width:0;height:28px;box-sizing:border-box;padding:0 10px;'\n"
    "    + 'border:1px solid #c8ccd0;border-radius:14px;background:#fff;font:14px system-ui,sans-serif;color:#202124;outline:none');\n"
    "  address.id = 'address'; address.type = 'text'; address.spellcheck = false; address.placeholder = 'Type an address';\n"
    "  address.value = location.href === 'about:blank' ? '' : location.href;\n"
    "  address.addEventListener('focus', () => address.select());\n"
    "  address.addEventListener('keydown', event => {\n"
    "    event.stopPropagation();\n"
    "    if (event.key === 'Enter' && address.value.trim()) send('go ' + address.value.trim());\n"
    "  });\n"
    "  for (const type of ['keyup', 'keypress', 'input'])\n"
    "    address.addEventListener(type, event => event.stopPropagation());\n"
    "  bar.append(back, forward, address);\n"
    "  host.attachShadow({ mode: 'closed' }).append(bar);\n"
    "\n"
    "  const place = () => {\n"
    "    const root = document.documentElement;\n"
    "    if (!root) return;\n"
    "    if (host.parentNode !== root) root.append(host);\n"
    "    root.style.setProperty('margin-top', height + 'px', 'important');\n"
    "  };\n"
    "  place();\n"
    "  document.addEventListener('DOMContentLoaded', place);\n"
    "  addEventListener('load', place);\n"
    "\n"
    "  globalThis.zebkitBar = {\n"
    "    update(url, canGoBack, canGoForward) {\n"
    "      if (bar.getRootNode().activeElement !== address) address.value = url === 'about:blank' ? '' : url;\n"
    "      back.disabled = !canGoBack; forward.disabled = !canGoForward;\n"
    "      back.style.opacity = canGoBack ? 1 : 0.4; forward.style.opacity = canGoForward ? 1 : 0.4;\n"
    "    }\n"
    "  };\n"
    "  send('ready');\n"
    "})();\n";
