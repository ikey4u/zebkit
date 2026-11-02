"""The start of docs/reports/electron-compare.md: versions, what was held the same, sizes. For compare.py report."""
import json
import datetime
import sys

size_file, electron, chromium, electron_seen, profile = sys.argv[1:6]
size = json.load(open(size_file))
mib = lambda b: f"{b / 1048576:.1f}"  # noqa: E731


def row(label, zebkit, other):
    percent = (other - zebkit) / other * 100
    return f"| {label} | {mib(zebkit)} | {mib(other)} | {'zebkit' if zebkit < other else 'Electron'}, {abs(percent):.0f}% less |"


intro = [
    f"# zebkit and Electron {electron}, measured the same way", "",
    f"`scripts/electron-compare.sh`, {datetime.date.today()}, Linux x86-64: zebkit `{profile}` (Chromium {chromium}) and "
    f"Electron {electron_seen}, the latest stable release on npm, on one machine. Each cell is the median and, in "
    "parentheses, the lowest and highest run. A result is called within noise when those ranges overlap. "
    "`docs/SIZE_AND_SPEED.md` has what was held the same and what could not be.", "",
    "## Size", "",
    "MiB. As published is what each project hands out; as an application ships it is the runtime trimmed as far as it goes "
    "without building it, with the same thin host: Electron with one locale and no licence file, zebkit without the headers "
    "an application is built against.", "",
    "| | zebkit | Electron | Smaller |", "|---|---:|---:|---|",
    row("As published, uncompressed", size["package_bytes"], size["electron_package_bytes"]),
    row("As published, `.tar.xz`", size["package_xz"], size["electron_package_xz"]),
    row("As published, `.zip`", size["package_zip"], size["electron_package_zip"]),
    row("As an application ships it, uncompressed", size["shipped_bytes"], size["electron_shipped_bytes"]),
    row("As an application ships it, `.tar.xz`", size["shipped_xz"], size["electron_shipped_xz"]),
    row("As an application ships it, `.zip`", size["shipped_zip"], size["electron_shipped_zip"]),
    "", "## Engines", "",
]
json.dump({"intro": intro, "electron": electron_seen, "zebkit_chromium": chromium, "size": size}, sys.stdout)
