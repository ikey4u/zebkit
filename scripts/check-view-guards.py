#!/usr/bin/env python3
"""The C API's view rule (zebkit.h): every exported function taking a
zebkit_view* checks its view (ZEBKIT_LIVE_VIEW_OR_RETURN or LiveView()) before
any other use of it, and no exported function uses DCHECK_CURRENTLY_ON
(a release build would go on off the engine's thread).

    scripts/check-view-guards.py [checkout]
"""
import glob, os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
inc = os.path.join(root, "src/engine/include/zebkit")
decl = re.compile(r"ZEBKIT_API\s+[^;(]*?\b(zebkit_\w+)\s*\(([^)]*)\)\s*;", re.S)
view_functions, exported = set(), set()
for h in glob.glob(os.path.join(inc, "*.h")):
    text = re.sub(r"/\*.*?\*/", "", open(h).read(), flags=re.S)
    for m in decl.finditer(text):
        exported.add(m.group(1))
        if re.search(r"\bzebkit_view\s*\*", m.group(2)):
            view_functions.add(m.group(1))

DEFINITION = r'^(?:[A-Za-z_][\w:\*&<>" \t]*?[ \t\*&])?%s\s*\(([^;{\[]*?)\)\s*\{'
GUARD = r"(ZEBKIT_LIVE_VIEW_OR_RETURN|LiveView)\(\s*%s\b"
definitions = {}
sources = [p for ext in ("cc", "mm") for p in glob.glob(os.path.join(root, "src/engine/**/*." + ext), recursive=True)]
for path in sources:
    text = open(path).read()
    for name in exported:
        if name not in text:
            continue
        for m in re.finditer(DEFINITION % re.escape(name), text, re.M):
            depth, i = 1, m.end()
            while depth and i < len(text):
                depth += {"{": 1, "}": -1}.get(text[i], 0)
                i += 1
            body = re.sub(r"//[^\n]*", "", text[m.end():i - 1]).strip()
            line = text.count("\n", 0, m.start()) + 1
            views = re.findall(r"zebkit_view\s*\*\s*(\w+)", m.group(1))
            definitions.setdefault(name, []).append((os.path.relpath(path, root), line, body, views))

problems = []
for name in sorted(definitions):
    for path, line, body, views in definitions[name]:
        where = "%s:%d %s" % (path, line, name)
        if "DCHECK_CURRENTLY_ON" in body:
            problems.append("DCHECK_CURRENTLY_ON: " + where)
        if name not in view_functions:
            continue
        # Each view parameter's first use is the guard's.
        for view in views or ["view"]:
            # Not uses: a test for NULL, and the view passed back to the
            # application's own callback.
            harmless = r"(\b(func|function|callback)\s*\(\s*%s\b|\b%s\s*&&|!\s*%s\b)" % (view, view, view)
            scan = re.sub(harmless, lambda m: "_" * len(m.group(0)), body)
            use = re.search(r"\b%s\b" % view, scan)
            guard = re.search(GUARD % view, scan)
            if not guard or use.start() < guard.start() + len(guard.group(1)):
                problems.append("used before a guard: %s (%s)" % (where, view))
for name in sorted(view_functions - set(definitions)):
    problems.append("not found in src/engine: " + name)
print("\n".join(problems))
print("%d exported view functions, %d problems" % (len(view_functions), len(problems)))
sys.exit(1 if problems else 0)
