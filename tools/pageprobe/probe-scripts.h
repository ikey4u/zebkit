/*
 * What pageprobe runs in a page, for pageprobe.c (Linux) and pageprobe.m
 * (macOS): the two ask a page the same questions, so that their reports
 * can be compared.
 */

#pragma once

#define VIEW_WIDTH 1280
#define VIEW_HEIGHT 800

/* Runs before any page script, in the main frame. Uncaught exceptions and
 * rejected promises are what distinguish a page that works from one that only
 * looks loaded; failed subresources are counted separately. */
static const char* errorHookScript =
    "(function () {"
    "  if (window.__probe) return;"
    "  const p = window.__probe = { errors: [], resourceErrors: 0, consoleErrors: 0, consoleMessages: [] };"
    "  const note = m => { if (p.errors.length < 50) p.errors.push(String(m).slice(0, 240)); };"
    "  addEventListener('error', e => {"
    "    if (e.target && e.target !== window && e.target.tagName) p.resourceErrors++;"
    "    else note(e.message || 'error');"
    "  }, true);"
    "  addEventListener('unhandledrejection', e =>"
    "    note('unhandled rejection: ' + (e.reason && e.reason.message || e.reason)));"
    "  const original = console.error;"
    "  console.error = function () {"
    "    p.consoleErrors++;"
    "    if (p.consoleMessages.length < 20)"
    "      p.consoleMessages.push(Array.from(arguments).map(a => String(a && a.message || a)).join(' ').slice(0, 240));"
    "    return original.apply(this, arguments);"
    "  };"
    "})();";

/* Elements and text are counted through open shadow roots as well: sites
 * built from web components keep most of their page there. */
static const char* measureScript =
    "(function () {"
    "  const p = window.__probe || { errors: [], resourceErrors: 0, consoleErrors: 0 };"
    "  let elements = 0, textLength = 0, sample = '';"
    "  const tags = { IMG: 0, IFRAME: 0, VIDEO: 0, CANVAS: 0, svg: 0, A: 0 };"
    "  const skip = { SCRIPT: 1, STYLE: 1, NOSCRIPT: 1, TEMPLATE: 1 };"
    "  const walk = root => {"
    "    for (const node of root.childNodes) {"
    "      if (node.nodeType === 3) {"
    "        const t = node.nodeValue.trim();"
    "        if (t) { textLength += t.length; if (sample.length < 200) sample += t + ' '; }"
    "      } else if (node.nodeType === 1) {"
    "        elements++;"
    "        if (node.tagName in tags) tags[node.tagName]++;"
    "        if (skip[node.tagName]) continue;"
    "        if (node.shadowRoot) walk(node.shadowRoot);"
    "        walk(node);"
    "      }"
    "    }"
    "  };"
    "  if (document.documentElement) walk(document);"
    "  const images = Array.from(document.images);"
    "  return JSON.stringify({"
    "    url: location.href,"
    "    title: document.title,"
    "    readyState: document.readyState,"
    "    elements: elements,"
    "    textLength: textLength,"
    "    links: tags.A,"
    "    images: tags.IMG,"
    "    imagesBroken: images.filter(i => i.complete && i.currentSrc && i.naturalWidth === 0).length,"
    "    scripts: document.scripts.length,"
    "    stylesheets: document.styleSheets.length,"
    "    iframes: tags.IFRAME,"
    "    videos: tags.VIDEO,"
    "    canvases: tags.CANVAS,"
    "    svgs: tags.svg,"
    "    scrollHeight: document.documentElement ? document.documentElement.scrollHeight : 0,"
    "    errors: p.errors,"
    "    resourceErrors: p.resourceErrors,"
    "    consoleErrors: p.consoleErrors,"
    "    consoleMessages: p.consoleMessages || [],"
    "    textSample: sample.replace(/\\s+/g, ' ').slice(0, 200)"
    "  });"
    "})()";
