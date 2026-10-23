"""Draw zebkit's logo: docs/images/logo-mark.svg (the mark, a globe banded
like a zebra) and docs/images/logo.svg (the mark and the name, which follows
GitHub's light and dark themes).

    python3 scripts/logo-svg.py

The globe is the web; its two bands are cut out of it, so the page shows
through them on a light page and a dark one alike. They are drawn as the
solid slices between the bands, clipped to the circle.
"""
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent

RADIUS = 52
# Each band: where it starts at the centre, its thickness, the wave's
# height, its phase, and its tilt (rise per unit to the right).
BANDS = [(44, 11, 6, 0.4, -0.15), (74, 11, 6, 0.4, -0.15)]


def wave(y0, amplitude, phase, tilt, x0=-10, x1=138, steps=60):
    points = []
    for i in range(steps + 1):
        x = x0 + (x1 - x0) * i / steps
        points.append((x, y0 + amplitude * math.sin(x / 128 * 2 * math.pi * 0.9 + phase) + tilt * (x - 64)))
    return points


def path(points):
    return "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in points) + "Z"


def slices():
    """The solid parts: above the first band, between bands, below the last."""
    edges = [[(-10, -20), (138, -20)]]
    for y, thickness, amplitude, phase, tilt in BANDS:
        edges.append(wave(y, amplitude, phase, tilt))
        edges.append(wave(y + thickness, amplitude, phase, tilt))
    edges.append([(-10, 150), (138, 150)])
    return [path(edges[i] + edges[i + 1][::-1]) for i in range(0, len(edges), 2)]


def mark(gid):
    parts = "".join(f'<path d="{d}"/>' for d in slices())
    return f'''<defs>
    <linearGradient id="{gid}-g" gradientUnits="userSpaceOnUse" x1="16" y1="12" x2="112" y2="116">
      <stop offset="0" stop-color="#22D3EE"/>
      <stop offset="1" stop-color="#6366F1"/>
    </linearGradient>
    <clipPath id="{gid}-c"><circle cx="64" cy="64" r="{RADIUS}"/></clipPath>
  </defs>
  <g clip-path="url(#{gid}-c)" fill="url(#{gid}-g)">{parts}</g>'''


def svg(width, inner, label="zebkit"):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} 128" width="{width}" height="128" '
            f'role="img" aria-label="{label}">\n  {inner}\n</svg>\n')


WORD = """<style>
    .zebkit-word { fill: #0F172A; }
    @media (prefers-color-scheme: dark) { .zebkit-word { fill: #F8FAFC; } }
  </style>
  <text class="zebkit-word" x="138" y="89" font-family="Manrope, Inter, 'SF Pro Display', 'Segoe UI', Roboto, Helvetica, Arial, sans-serif" font-size="76" font-weight="800" letter-spacing="-2.5">zebkit</text>"""

(ROOT / "docs/images/logo-mark.svg").write_text(svg(128, mark("zebkit")))
(ROOT / "docs/images/logo.svg").write_text(svg(400, mark("zebkit") + "\n  " + WORD))
