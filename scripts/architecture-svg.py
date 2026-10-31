"""Draw docs/images/architecture.svg, the picture of zebkit in README.md and
docs/ARCHITECTURE.md, in GitHub's light and dark themes.

    python3 scripts/architecture-svg.py
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
W, H = 960, 640
FONT = "Inter, 'SF Pro Text', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif"
MONO = "'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace"
out = []
def add(s): out.append(s)
def esc(t): return t.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
def rect(x, y, w, h, cls, rx=10, extra=""):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" class="{cls}" {extra}/>')
def text(x, y, t, cls, size=13, weight=400, anchor="start", family=FONT):
    add(f'<text x="{x}" y="{y}" class="{cls}" font-family="{family}" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{esc(t)}</text>')
def arrow(x, y1, y2, label=None, anchor="start"):
    add(f'<line x1="{x}" y1="{y1}" x2="{x}" y2="{y2 - 7}" class="wire"/>')
    add(f'<path d="M{x - 5} {y2 - 8}l5 8 5-8z" class="head"/>')
    if label:
        text(x + 10 if anchor == "start" else x - 10, (y1 + y2) / 2 + 4, label, "muted", 12, anchor=anchor)

add(f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" role="img" aria-label="zebkit's architecture: the application links one library, which runs Chromium's content layer, Blink and V8 in the application's process, with sandboxed helper processes, on Linux and macOS">
<style>
  .card {{ fill: #FFFFFF; stroke: #D0D7DE; }}
  .box {{ fill: #F6F8FA; stroke: #D0D7DE; }}
  .chip {{ fill: #FFFFFF; stroke: #D0D7DE; }}
  .zeb {{ fill: #ECFEFF; stroke: #0891B2; stroke-width: 1.5; }}
  .pill {{ fill: #E0F2FE; stroke: none; }}
  .os {{ fill: #F1F5F9; stroke: #CBD5E1; }}
  .plan {{ fill: none; stroke: #94A3B8; stroke-dasharray: 5 4; }}
  .ink {{ fill: #0F172A; }}
  .muted {{ fill: #57606A; }}
  .accent {{ fill: #0E7490; }}
  .wire {{ stroke: #64748B; stroke-width: 1.5; }}
  .head {{ fill: #64748B; }}
  @media (prefers-color-scheme: dark) {{
    .card {{ fill: #0D1117; stroke: #30363D; }}
    .box {{ fill: #161B22; stroke: #30363D; }}
    .chip {{ fill: #0D1117; stroke: #30363D; }}
    .zeb {{ fill: #082F3A; stroke: #22D3EE; }}
    .pill {{ fill: #0E3A47; }}
    .os {{ fill: #161B22; stroke: #30363D; }}
    .plan {{ stroke: #6E7681; }}
    .ink {{ fill: #E6EDF3; }}
    .muted {{ fill: #8B949E; }}
    .accent {{ fill: #67E8F9; }}
    .wire {{ stroke: #8B949E; }}
    .head {{ fill: #8B949E; }}
  }}
</style>''')
rect(0.5, 0.5, W - 1, H - 1, "card", 16)

# Row 1: who uses it.
rect(24, 24, 600, 100, "box")
text(44, 52, "Your application", "ink", 15, 700)
text(604, 52, "native code or JavaScript, no Node", "muted", 12, anchor="end")
for i, (a, b) in enumerate([("C / C++", "zebkit.h"), ("Rust", "zebkit crate"), ("Python", "ctypes binding"), ("JavaScript", "zebkit-js on V8")]):
    x = 44 + i * 144
    rect(x, 66, 132, 44, "chip", 8)
    text(x + 66, 84, a, "ink", 13, 600, "middle")
    text(x + 66, 101, b, "muted", 11, anchor="middle")
rect(648, 24, 288, 100, "box")
text(668, 52, "AI agents", "ink", 15, 700)
for i, (a, b, cls) in enumerate([("MCP", "tools, planned", "plan"), ("CDP", "pages, as Chrome", "chip")]):
    x = 668 + i * 128
    rect(x, 66, 116, 44, cls, 8)
    text(x + 58, 84, a, "ink", 13, 600, "middle")
    text(x + 58, 101, b, "muted", 11, anchor="middle")

arrow(324, 124, 156, "links one library: a C API")
arrow(792, 124, 156, "pages, PDF, files, media, capture", "end")

# Row 2: zebkit itself.
top, h = 156, 262
rect(24, top, 912, h, "zeb", 14)
mark = (ROOT / "docs/images/logo-mark.svg").read_text()
mark = mark[mark.index(">") + 1:mark.rindex("</svg>")].replace("zebkit-", "arch-")
add(f'<g transform="translate(44 {top + 16}) scale(0.25)">{mark}</g>')
text(86, top + 30, "zebkit", "ink", 18, 800)
text(86, top + 47, "libzebkit.so.1 on Linux, Zebkit.framework on macOS: inside your process", "muted", 12)
text(916, top + 30, "no Node, no Chrome browser UI", "accent", 12, 600, "end")
apis = [("zebkit.h", "views and windows"), ("schemes", "your own app://"), ("pdf.h", "PDF on PDFium"),
        ("services.h", "HTTP, crypto, images"), ("vendor/*.h", "17 libraries, 7,489 fns"), ("zebkit.ui", "native windows in JS")]
for i, (a, b) in enumerate(apis):
    x = 44 + i * 147
    rect(x, top + 64, 137, 52, "chip", 8)
    text(x + 12, top + 86, a, "ink", 13, 700, family=MONO)
    text(x + 12, top + 104, b, "muted", 11)
text(44, top + 144, "Chromium 155, built as a library", "ink", 13, 700)
parts = ["content layer", "Blink", "V8 + JIT", "Skia", "compositor", "network stack",
         "DevTools / CDP", "PDFium", "FFmpeg", "ICU", "SQLite", "BoringSSL"]
for i, p in enumerate(parts):
    x = 44 + (i % 6) * 147
    y = top + 158 + (i // 6) * 38
    rect(x, y, 137, 30, "pill", 15)
    text(x + 68.5, y + 20, p, "accent", 12, 600, "middle")

# Row 3: helper processes.
arrow(244, top + h, 456, "Mojo IPC; a page's calls are bytes, not base64")
y3 = 456
boxes = [(24, 440, "Renderer: Blink + V8", "your pages; zebkit.* only where you grant it"),
         (476, 224, "GPU", "Skia on GL, Vulkan or Metal"),
         (712, 224, "Utility", "network, audio, storage")]
for x, w, a, b in boxes:
    rect(x, y3, w, 76, "box")
    text(x + 18, y3 + 30, a, "ink", 14, 700)
    text(x + 18, y3 + 52, b, "muted", 12)
text(936, y3 - 10, "zebkit-helper processes, sandboxed", "muted", 12, anchor="end")

# Row 4: systems.
y4 = 560
for i, (a, b, cls) in enumerate([("Linux", "x86-64 · X11 · Wayland", "os"), ("macOS", "Apple silicon · AppKit · Metal", "os"), ("Windows", "planned, same API", "plan")]):
    x = 24 + i * 308
    rect(x, y4, 296, 56, cls)
    text(x + 18, y4 + 34, a, "ink", 14, 700)
    text(x + 278, y4 + 34, b, "muted", 12, anchor="end")
add("</svg>")
open(ROOT / "docs/images/architecture.svg", "w").write("\n".join(out) + "\n")
