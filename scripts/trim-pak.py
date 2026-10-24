#!/usr/bin/env python3
"""Write a copy of a .pak resource file with only the engine's resources.

    scripts/trim-pak.py <in.pak> <in.pak.info> <out.pak> [<more.pak> <more.pak.info>]...

The content shell's pack also holds the DevTools front end, about:tracing,
the browser's internal pages (chrome://media-internals and the like) and the
WebUI toolkit they are made of: 6.4 of its 6.9 MiB. An engine driven over
CDP shows none of them. What is kept is listed below by the .grd file the
resources come from (the .pak.info file the build writes beside the pack
names it), and by name for single resources of a source that otherwise goes.
Entries are copied as stored, compressed or not. More packs after the
output give only what KEEP_NAMES names of theirs: the template of printed
pages' headers and footers is in components' pack, which the content shell
does not repack, and is all of it the engine uses.
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "upstream" / "chromium" / "src" / "tools" / "grit"))
from grit.format import data_pack  # noqa: E402

KEEP_SOURCES = {
    "blink_resources.grd",               # user-agent style sheets, HTML parser data, form control images
    "blink_image_resources.grd",
    "blink_strings.grd",                 # text Blink shows: validation messages, input placeholders
    "permission_element_strings.grd",
    "permission_element_generated_strings.grd",
    "media_controls_resources.grd",      # the <video> and <audio> controls
    "inspector_overlay_resources.grd",   # what CDP's Overlay domain draws on a page
    "ui_resources.grd",                  # cursors, scroll bar and widget images
    "ui_strings.grd",
    "ax_strings.grd",                    # accessible names, for CDP's accessibility tree
    "auto_image_annotation_strings.grd",
    "app_locale_settings.grd",
    "net_resources.grd",                 # network error pages
    "content_resources.grd",
    "shell_resources.grd",
    "views_resources.grd",               # the shell's own window
}
KEEP_NAMES = {
    "COMPRESSED_PROTOCOL_JSON",          # the protocol definition served at /json/protocol
    "chrome_track_event_extension_descriptor",  # CDP's Tracing domain
    "IDR_PRINT_HEADER_FOOTER_TEMPLATE_PAGE",  # Page.printToPDF's headerTemplate and footerTemplate
}


def kept_ids(info, sources):
    keep = set()
    for line in open(info):
        parts = line.strip().split(",")
        if len(parts) < 3:
            continue
        name, resource_id, source = parts[0], int(parts[1]), parts[2]
        if (sources and source.rsplit("/", 1)[-1] in KEEP_SOURCES) or name in KEEP_NAMES:
            keep.add(resource_id)
    return keep


def main():
    if len(sys.argv) < 4 or len(sys.argv) % 2:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    pak_in, info, pak_out = sys.argv[1:4]

    pack = data_pack.ReadDataPack(pak_in)
    keep = kept_ids(info, sources=True)
    resources = {i: data for i, data in pack.resources.items() if i in keep}
    before = sum(len(d) for d in pack.resources.values())
    extra = sys.argv[4:]
    for more_pak, more_info in zip(extra[::2], extra[1::2]):
        more = data_pack.ReadDataPack(more_pak)
        more_keep = kept_ids(more_info, sources=False)
        resources.update({i: d for i, d in more.resources.items() if i in more_keep})
    data_pack.WriteDataPack(resources, pak_out, pack.encoding)
    after = sum(len(d) for d in resources.values())
    print(f"kept {len(resources)} of {len(pack.resources)} resources, "
          f"{after / 1048576:.2f} of {before / 1048576:.2f} MiB")

if __name__ == "__main__":
    main()
