"""A long PDF, written here so that every system has one: each page says its
number, has a page-sized band that differs from its neighbours', and the first
has a link to page 40 and one to the web. `real_pdf()` is the real one the
suite also opens, upstream.lock's PDF_REAL_URL, if it was fetched."""
import os
from pathlib import Path


def long_pdf(pages=300, link="http://example.invalid/linked"):
    objects = [None, None, None]  # catalog, pages, font
    kids = []
    for n in range(1, pages + 1):
        shade = (n * 37 % 100) / 100
        text = (f"BT /F1 24 Tf 72 720 Td (Page number {n}) Tj ET "
                f"BT /F1 12 Tf 72 690 Td (zebkit continuous needle{n} text) Tj ET "
                f"{shade:.2f} 0.5 {1 - shade:.2f} rg 72 100 468 500 re f")
        stream = len(objects) + 1
        objects.append(f"<< /Length {len(text)} >>\nstream\n{text}\nendstream")
        annots = ""
        if n == 1:
            # Filled in below: the page's internal link needs the 40th page's number.
            annots = " /Annots [ {{LINKS}} ]"
        page = len(objects) + 1
        objects.append(f"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents {stream} 0 R "
                       f"/Resources << /Font << /F1 3 0 R >> >>{annots} >>")
        kids.append(page)
    objects[0] = "<< /Type /Catalog /Pages 2 0 R >>"
    objects[1] = f"<< /Type /Pages /Count {pages} /Kids [{' '.join(f'{k} 0 R' for k in kids)}] >>"
    objects[2] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"
    internal = len(objects) + 1
    objects.append(f"<< /Type /Annot /Subtype /Link /Rect [72 600 300 640] /Border [0 0 0] "
                   f"/Dest [{kids[min(39, pages - 1)]} 0 R /Fit] >>")
    external = len(objects) + 1
    objects.append(f"<< /Type /Annot /Subtype /Link /Rect [72 540 300 580] /Border [0 0 0] "
                   f"/A << /S /URI /URI ({link}) >> >>")
    objects = [o.replace("{{LINKS}}", f"{internal} 0 R {external} 0 R") for o in objects]
    out = bytearray(b"%PDF-1.7\n")
    offsets = []
    for i, body in enumerate(objects):
        offsets.append(len(out))
        out += f"{i + 1} 0 obj\n{body}\nendobj\n".encode("latin-1")
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n0000000000 65535 f \n".encode()
    for offset in offsets:
        out += f"{offset:010d} 00000 n \n".encode()
    out += f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n".encode()
    return bytes(out)


def real_pdf():
    path = os.environ.get("ZEBKIT_TEST_REAL_PDF")
    return Path(path) if path and Path(path).is_file() else None
