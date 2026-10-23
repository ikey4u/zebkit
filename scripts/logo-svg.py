"""Draw zebkit's logo: docs/images/logo-mark.svg (the mark, a cube in a
zebra's stripes on a cyan tile) and docs/images/logo.svg (the mark and the
name, which follows GitHub's light and dark themes).

    python3 scripts/logo-svg.py
"""
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

# The cube, in isometric view: the top face A (top), B (right), C (centre),
# D (left); the side faces go H down. The stripes run over the top face and
# on down the left face, as a zebra's do over its back and flank.
A, B, C, D = (64, 22), (100, 43), (64, 64), (28, 43)
H = 42
DOWN = (0, H)


def add(p, q, k=1):
    return (p[0] + k * q[0], p[1] + k * q[1])


def poly(points):
    return "M" + " L".join(f"{round(x, 2)} {round(y, 2)}" for x, y in points) + "Z"


U = (A[0] - D[0], A[1] - D[1])  # along the top face, D to A
V = (C[0] - D[0], C[1] - D[1])  # across it, D to C
W = (B[0] - C[0], B[1] - C[1])  # along the right face's top edge, C to B
L1, L2, R1 = add(D, DOWN), add(C, DOWN), add(B, DOWN)


def on_top(s, t):
    return add(add(D, U, s), V, t)


top_stripes, left_stripes, right_stripes = [], [], []
for a, b in [(0.10, 0.27), (0.42, 0.59), (0.74, 0.91)]:
    top_stripes.append(poly([on_top(0, a), on_top(1, a), on_top(1, b), on_top(0, b)]))
    # Over the edge D-C and down the left face, leaning a little.
    pa, pb = on_top(0, a), on_top(0, b)
    left_stripes.append(poly([pa, pb, add(add(pb, DOWN), (-3, 0)), add(add(pa, DOWN), (-3, 0))]))
for a, b in [(0.12, 0.30), (0.46, 0.64), (0.80, 0.98)]:
    # Down the right face from its top edge, leaning the other way.
    pa, pb = add(C, W, a), add(C, W, b)
    right_stripes.append(poly([pa, pb, add(pb, (6, H)), add(pa, (6, H))]))


def paths(polys):
    return "".join(f'<path d="{p}"/>' for p in polys)


def mark(gid):
    return f'''<defs>
    <linearGradient id="{gid}-tile" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#22D3EE"/>
      <stop offset="1" stop-color="#0E7490"/>
    </linearGradient>
    <clipPath id="{gid}-top"><path d="{poly([A, B, C, D])}"/></clipPath>
    <clipPath id="{gid}-left"><path d="{poly([D, C, L2, L1])}"/></clipPath>
    <clipPath id="{gid}-right"><path d="{poly([C, B, R1, L2])}"/></clipPath>
  </defs>
  <rect width="128" height="128" rx="30" fill="url(#{gid}-tile)"/>
  <path d="{poly([A, B, C, D])}" fill="#FFFFFF"/>
  <path d="{poly([D, C, L2, L1])}" fill="#E2E8F0"/>
  <path d="{poly([C, B, R1, L2])}" fill="#CBD5E1"/>
  <g fill="#0F172A">
    <g clip-path="url(#{gid}-top)">{paths(top_stripes)}</g>
    <g clip-path="url(#{gid}-left)">{paths(left_stripes)}</g>
    <g clip-path="url(#{gid}-right)">{paths(right_stripes)}</g>
  </g>'''


def svg(width, inner, label="zebkit"):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 128" width="{width}" height="128" '
            f'role="img" aria-label="{label}">\n  {inner}\n</svg>\n')


WORD = """<style>
    .zebkit-word { fill: #0B1120; }
    @media (prefers-color-scheme: dark) { .zebkit-word { fill: #F8FAFC; } }
  </style>
  <text class="zebkit-word" x="150" y="90" font-family="Inter, 'SF Pro Display', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif" font-size="78" font-weight="700" letter-spacing="-2.5">zebkit</text>"""

(ROOT / "docs/images/logo-mark.svg").write_text(svg(128, mark("zebkit")))
(ROOT / "docs/images/logo.svg").write_text(svg(420, mark("zebkit") + "\n  " + WORD))
