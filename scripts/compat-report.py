#!/usr/bin/env python3
"""Compare a profile's site results with a reference profile's and judge each site.

Reads out/compat/<profile>/ and out/compat/<reference>/ (written by
scripts/compat.sh) and writes docs/reports/compat.md (on macOS,
docs/reports/macos/compat.md). Exits non-zero unless every
site passes. The rules are explained in docs/archive/webkit/COMPAT.md; in short, a site
passes when the profile loads it without crashing, paints it, builds a DOM
about as large as the reference build does, and throws no script error the
reference build does not also throw.

    scripts/compat-report.py <profile> [reference]      (reference defaults to baseline)
"""

import json
import pathlib
import platform
import re
import sys
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parent.parent

# A profile's page may be somewhat smaller than the reference's and still be
# the same page: sites rotate content between two loads seconds apart.
MIN_RATIO = 0.6
# Below this the viewport is blank or a single flat colour.
MIN_PAINTED = 0.01
MIN_COLOURS = 4
# A real landing page of a top-100 site has far more than this; fewer means a
# bot challenge, a block notice or an error page was served instead.
MIN_SITE_ELEMENTS = 60
CHALLENGE_TITLE = re.compile(r"just a moment|error page|access denied|attention required|are you a robot|нет соединения", re.I)


def load(profile, domain):
    path = ROOT / "out" / "compat" / profile / f"{domain}.json"
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return None


def usable(result):
    """Why a result cannot serve as a rendering of the site, or None if it can."""
    if result is None:
        return "no result"
    if result.get("probeFailed"):
        return "probe did not finish"
    if result.get("crashed"):
        return f"web process {result['crashed']}"
    page = result.get("page")
    if result.get("loadFailed") and not page:
        return f"load failed: {result['loadFailed']}"
    if not page:
        return "no page to measure"
    if result.get("loadFailed") and page.get("elements", 0) < 20:
        return f"load failed: {result['loadFailed']}"
    return None


def stub_page(result):
    """Describe the page if it is a bot challenge or error stub rather than the site, else None."""
    page = result["page"]
    title = page.get("title", "")
    if CHALLENGE_TITLE.search(title) or page.get("elements", 0) < MIN_SITE_ELEMENTS:
        return f"got a challenge or error page ({page.get('elements', 0)} elements, title {title[:40]!r})"
    return None


def normalise(message):
    """Make script error messages comparable between two loads of a page."""
    message = re.sub(r"https?://\S+", "<url>", message)
    return re.sub(r"\d+", "N", message)[:120]


def same_destination(a, b):
    ua, ub = urllib.parse.urlsplit(a), urllib.parse.urlsplit(b)
    return ua.hostname == ub.hostname and ua.path.rstrip("/") == ub.path.rstrip("/")


def judge(result, reference):
    """Return (status, reasons). Status is PASS, FAIL or BLOCKED."""
    reference_problem = usable(reference)
    if reference_problem:
        # The stock engine could not load it from here either, so there is
        # nothing to compare against: not a statement about the profile.
        return "BLOCKED", [f"reference: {reference_problem}"]
    reference_stub = stub_page(reference)
    if reference_stub:
        # Comparing two copies of a challenge page proves nothing about the site.
        return "BLOCKED", [f"reference {reference_stub}"]

    problem = usable(result)
    if problem:
        return "FAIL", [problem]

    reasons = []
    page, ref_page = result["page"], reference["page"]
    paint, ref_paint = result.get("paint") or {}, reference.get("paint") or {}

    if ref_paint.get("paintedFraction", 0) >= MIN_PAINTED:
        if paint.get("paintedFraction", 0) < MIN_PAINTED or paint.get("colours", 0) < MIN_COLOURS:
            reasons.append("viewport is blank")

    for key, label in (("elements", "DOM elements"), ("textLength", "text")):
        ref_value = ref_page.get(key, 0)
        if ref_value >= 40 and page.get(key, 0) < MIN_RATIO * ref_value:
            reasons.append(f"{label}: {page.get(key, 0)} vs {ref_value} in reference")

    ref_errors = {normalise(e) for e in ref_page.get("errors", [])}
    new_errors = [e for e in page.get("errors", []) if normalise(e) not in ref_errors]
    if new_errors:
        reasons.append(f"{len(new_errors)} script error(s) not in reference, first: {new_errors[0][:100]}")

    if not same_destination(page.get("url", ""), ref_page.get("url", "")) and reasons:
        reasons.append(f"ended at {page.get('url', '')[:80]} instead of {ref_page.get('url', '')[:80]}")

    return ("FAIL" if reasons else "PASS"), reasons


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    profile = sys.argv[1]
    reference_name = sys.argv[2] if len(sys.argv) == 3 else "baseline"
    sites = json.loads((ROOT / "assets" / "WEBSITE-TOP-100.json").read_text())["sites"]

    rows, counts = [], {"PASS": 0, "FAIL": 0, "BLOCKED": 0}
    for site in sites:
        result, reference = load(profile, site["domain"]), load(reference_name, site["domain"])
        status, reasons = judge(result, reference)
        counts[status] += 1
        rows.append((site, status, reasons, result or {}, reference or {}))

    def cell(result, key):
        return str((result.get("page") or {}).get(key, "-"))

    def painted(result):
        paint = result.get("paint")
        return f"{100 * paint['paintedFraction']:.0f}%" if paint else "-"

    lines = [
        f"# Site compatibility: `{profile}` against `{reference_name}`",
        "",
        f"- Sites: {len(sites)} from `assets/WEBSITE-TOP-100.json`",
        f"- **Pass: {counts['PASS']}, fail: {counts['FAIL']}, blocked: {counts['BLOCKED']}**",
        "- Blocked means the reference build did not get the real site from the test machine either",
        "  (unreachable, or a bot challenge or error page), so the comparison says nothing about this profile.",
        "- Columns show this profile / the reference. Rules are in `docs/archive/webkit/COMPAT.md`.",
        "",
    ]
    for wanted, title in (("FAIL", "Failing"), ("BLOCKED", "Blocked")):
        selected = [r for r in rows if r[1] == wanted]
        if selected:
            lines += [f"## {title}", "", "| # | Site | Why |", "|---:|---|---|"]
            lines += [f"| {s['rank']} | {s['domain']} | {'; '.join(reasons).replace('|', '/')} |"
                      for s, _, reasons, _, _ in selected]
            lines.append("")

    lines += ["## All sites", "",
              "| # | Site | Result | Elements | Text | Painted | Script errors |",
              "|---:|---|---|---:|---:|---:|---:|"]
    for site, status, _, result, reference in rows:
        errors = lambda r: str(len((r.get("page") or {}).get("errors", []))) if r.get("page") else "-"
        lines.append(
            f"| {site['rank']} | {site['domain']} | {status} "
            f"| {cell(result, 'elements')} / {cell(reference, 'elements')} "
            f"| {cell(result, 'textLength')} / {cell(reference, 'textLength')} "
            f"| {painted(result)} / {painted(reference)} "
            f"| {errors(result)} / {errors(reference)} |")

    # macOS has reports of the same names in a directory of its own.
    reports = ROOT / "docs" / "reports" / ("macos" if platform.system() == "Darwin" else "")
    reports.mkdir(parents=True, exist_ok=True)
    report = reports / "compat.md"
    report.write_text("\n".join(lines) + "\n")
    print(f"pass {counts['PASS']}, fail {counts['FAIL']}, blocked {counts['BLOCKED']}; wrote {report.relative_to(ROOT)}")
    for site, status, reasons, _, _ in rows:
        if status != "PASS":
            print(f"  {status:7} {site['domain']}: {'; '.join(reasons)}")
    sys.exit(0 if counts["PASS"] == len(sites) else 1)


if __name__ == "__main__":
    main()
