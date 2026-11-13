"""<zebkit/canvas.h>, <zebkit/text.h> and <zebkit/pdf.h>'s annotations and hooks
from Python, in pixels. The font's path is the first argument. Runs without the
engine:

    ZEBKIT_DIR=<package> PYTHONPATH=bindings/python python3 tests/engine/pdf_bindings.py <font.ttf>

Exit 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from harness import check, finish, zebkit  # noqa: E402


def refuses(function, what):
    try:
        function()
        check(False, what, "it did not fail")
    except zebkit.Error as e:
        check(True, what, str(e))


font = open(sys.argv[1], "rb").read()

# ---- Canvas ----
c = zebkit.Canvas(100, 100)
c.set_fill_color(0xff0000ff)
c.rect(10, 10, 20, 20)
c.fill()
c.save()
c.translate(50, 0)
c.scale(2, 2)
c.set_fill_color(0x00ff00ff)
c.begin_path()
c.rect(0, 5, 10, 10)
c.fill()
c.restore()
c.begin_path()
c.set_stroke_color(0x0000ffff)
c.set_line_width(4)
c.move_to(10, 50)
c.line_to(90, 50)
c.stroke()
check(c.set_linear_gradient(50, 0, 90, 0, [0, 1], [0xff0000ff, 0x0000ffff]), "a gradient")
c.begin_path()
c.rect(50, 70, 40, 10)
c.fill()
check(c.draw_image(bytes([9, 9, 9, 255]), 1, 1, 0, 80, 20, 20), "an image")
f = zebkit.Font(font)
c.set_font(f, 20)
c.set_fill_color(0x000000ff)
c.fill_text("X", 40, 30)
check(c.measure_text("X") == 20, "the advance of text")
pixels = c.pixels()


def at(x, y):
    return tuple(pixels[(y * 100 + x) * 4:(y * 100 + x) * 4 + 4])


check(at(15, 15) == (255, 0, 0, 255), "a fill", at(15, 15))
check(at(65, 15) == (0, 255, 0, 255), "a matrix")
check(at(50, 50) == (0, 0, 255, 255), "a stroke")
check(at(52, 75)[0] > 200 and at(88, 75)[2] > 200, "a gradient drawn")
check(at(10, 90) == (9, 9, 9, 255), "an image drawn")
check(at(45, 20) == (0, 0, 0, 255) and at(45, 5)[3] == 0, "text in a font from bytes")
png = c.png()
check(png[:4] == b"\x89PNG", "PNG out")
c.run([["fillColor", 0xff00ffff], ["rect", 0, 0, 5, 5], ["fill", False]])
c.close()
refuses(lambda: zebkit.Canvas(0, 0), "a canvas of no size")
refuses(lambda: zebkit.Font(b"nope"), "what is not a font")
wrong = zebkit.Canvas(5, 5)
refuses(lambda: wrong.run([["nothing"]]), "an operation of no such name")
refuses(lambda: wrong.fill_text("x", 0, 0), "text without a font")
wrong.close()
mine = bytearray(b"\xff\xff\xff\xff" * 16)
with zebkit.Canvas.over(mine, 4, 4) as over:
    over.set_fill_color(0x0000ffff)
    over.rect(0, 0, 2, 2)
    over.fill()
check(mine[:4] == b"\x00\x00\xff\xff" and mine[-4:] == b"\xff\xff\xff\xff", "a canvas over a bytearray")

# ---- Text ----
shaped = zebkit.shape_text(font, 20, "AB", language="en")
check(shaped["direction"] == "ltr" and shaped["advance"][0] == 40 and shaped["glyphs"][1]["cluster"] == 1, "shaped")
check(zebkit.shape_text(font, 20, "AB", direction="rtl")["glyphs"][0]["cluster"] == 1, "right to left")
refuses(lambda: zebkit.shape_text(b"no", 20, "a"), "shaping in no font")
refuses(lambda: zebkit.shape_text(font, -1, "a"), "shaping at a size below 0")


# ---- Annotations ----
def blank(width=300, height=300):
    # Pdf opens files; the smallest file with one page.
    objects = (b"%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n"
               b"2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n"
               b"3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 " + str(width).encode() + b" " +
               str(height).encode() + b"] >> endobj\ntrailer << /Root 1 0 R >>\n%%EOF\n")
    return zebkit.Pdf(objects)


def page_pixel(image, x, y):
    width, height, data = image
    place = ((height - 1 - y) * width + x) * 4
    return tuple(data[place:place + 3])


def near(p, r, g, b, t=40):
    return abs(p[0] - r) <= t and abs(p[1] - g) <= t and abs(p[2] - b) <= t


pdf = blank()
pdf.add_font("Ahem", font)
line = pdf.add_annotation(0, {"type": "line", "line": [50, 100, 250, 100], "color": [255, 0, 0], "border": 2,
                              "lineEndings": ["none", "closedArrow"], "interiorColor": [0, 0, 255],
                              "measure": {"unit": "mm", "scale": 0.5, "precision": 1}})
pdf.add_annotation(0, {"type": "freeText", "rect": [10, 10, 200, 60], "contents": "X", "font": "Ahem", "fontSize": 20,
                       "textColor": [0, 128, 0]})
stamp = pdf.add_annotation(0, {"type": "stamp", "rect": [50, 200, 250, 280], "stamp": "Approved"})
image = pdf.render(0)
check(near(page_pixel(image, 243, 100), 0, 0, 255), "a line's ending")
check(near(page_pixel(image, 20, 45), 0, 128, 0), "free text in a given font")
check(pdf.annotations(0)[0]["measure"]["text"] == "100.0 mm", "a measurement")
check(pdf.annotation_at(0, 80, 100) == line and pdf.annotation_at(0, 5, 290) is None, "the annotation at a point")
pdf.move_annotation(0, 2, 0)
check(pdf.annotations(0)[0]["type"] == "stamp", "reordered")
attachment = pdf.add_annotation(0, {"type": "fileAttachment", "rect": [270, 270, 290, 290], "icon": "Tag"})
pdf.attach_file(0, attachment, "a.bin", b"\x00\x01data")
with zebkit.Pdf(pdf.save()) as again:
    check(again.annotation_file(0, attachment) == ("a.bin", b"\x00\x01data"), "a file through a save")
    check(near(page_pixel(again.render(0), 243, 100), 0, 0, 255), "the same after a save")
for name in ("xfdf", "fdf"):
    data = pdf.export_annotations(name)
    other = blank()
    check(other.import_annotations(data, name) == 4, f"{name} out and in")
    check(near(page_pixel(other.render(0), 243, 100), 0, 0, 255), f"{name}: drawn")
    refuses(lambda: other.import_annotations(b"<!DOCTYPE a [<!ENTITY b 'c'>]><xfdf/>", "xfdf"), "a DTD refused")
    other.close()
copy_target = blank()
check(pdf.copy_annotation(0, 0, copy_target, 0) == 0 and copy_target.annotations(0)[0]["stamp"] == "Approved", "copied")
source = blank(100, 100)
source.add_annotation(0, {"type": "square", "rect": [0, 0, 100, 100], "interiorColor": [0, 0, 255], "border": 0})
source.flatten(0)
stamped = blank()
index = stamped.add_annotation(0, {"type": "stamp", "rect": [0, 0, 300, 300]})
stamped.stamp_from_page(0, index, source, 0)
check(near(page_pixel(stamped.render(0), 150, 150), 0, 0, 255), "a stamp from a page")
drawn = stamped.add_annotation(0, {"type": "stamp", "rect": [10, 10, 60, 60]})
stamped.draw_annotation(0, drawn, [["fillColor", 0xff0000ff], ["rect", 0, 0, 50, 50], ["fill", False]])
check(near(page_pixel(stamped.render(0), 30, 30), 255, 0, 0), "operations as an appearance")
refuses(lambda: stamped.draw_annotation(0, 99, []), "a drawing of nothing")
pdf.flatten_annotation(0, 0)
check(len(pdf.annotations(0)) == 3, "one flattened")
refuses(lambda: pdf.add_annotation(0, {"type": "line", "line": [1, 1, 2, 2], "lineEndings": ["no", "none"]}), "wrong values refused")
pdf.close()

# ---- Hooks: the example plugin's shape, in Python ----
pdf = blank()
told = []
history = []
hook_after = pdf.hooks.after("*", lambda operation: (told.append(operation), history.append(operation.get("inverse"))))


def draw(canvas, annotation, width, height):
    canvas.set_fill_color(0xffdd00ff)
    canvas.begin_path()
    canvas.arc(width / 2, height / 2, min(width, height) / 2, 0, 360)
    canvas.fill()
    return True


kind = pdf.hooks.kind("Sample_Sticker", "Sample_", schema={"mood": {"type": "string", "required": True}},
                      check=lambda data: None if data["mood"] != "" else "not empty", draw=draw,
                      hit_test=lambda a, x, y: (x - 100) ** 2 + (y - 100) ** 2 <= 2500)
refuses(lambda: pdf.add_annotation(0, {"type": "Sample_Sticker", "rect": [50, 50, 150, 150], "data": {}}), "the kind's schema")
refuses(lambda: pdf.add_annotation(0, {"type": "Sample_Sticker", "rect": [50, 50, 150, 150], "data": {"mood": ""}}), "its own check")
sticker = pdf.add_annotation(0, {"type": "Sample_Sticker", "rect": [50, 50, 150, 150], "data": {"mood": "happy"}})
image = pdf.render(0)
check(near(page_pixel(image, 100, 100), 255, 221, 0) and page_pixel(image, 52, 52) == (255, 255, 255), "a kind's appearance")
check(pdf.annotations(0)[0]["data"]["mood"] == "happy", "its data")
check(pdf.annotation_at(0, 100, 100, 0) == sticker and pdf.annotation_at(0, 52, 52, 0) is None, "its hit test")
kept = zebkit.Pdf(pdf.save())
check(kept.annotations(0)[0]["type"] == "unknown" and kept.annotations(0)[0]["subtype"] == "Sample_Sticker",
      "its subtype through a save, unknown to that document")
kept.close()
inverse = [i for i in history if i][-1]
pdf.apply(inverse)
check(len(pdf.annotations(0)) == 0, "undone by its inverse")

# Refusal, change, exceptions.
hook = pdf.hooks.before("annotation.add", lambda operation: {"refuse": "not today"})
try:
    pdf.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]})
    check(False, "refused")
except zebkit.Error as e:
    check(str(e) == "not today", "refused", str(e))
check(zebkit.remove_pdf_hook(hook) and not zebkit.remove_pdf_hook(hook), "a hook removed")
hook = pdf.hooks.before("annotation.add", lambda operation: 1 / 0)
refuses(lambda: pdf.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]}), "an exception refuses")
zebkit.remove_pdf_hook(hook)
hook = pdf.hooks.before("annotation.add", lambda operation: {**operation, "annotation": {**operation["annotation"], "interiorColor": [0, 0, 255], "border": 0}})
pdf.add_annotation(0, {"type": "square", "rect": [10, 10, 50, 50], "interiorColor": [255, 0, 0]})
check(near(page_pixel(pdf.render(0), 30, 30), 0, 0, 255), "changed by a hook")
zebkit.remove_pdf_hook(hook)

# What leaves the function as an exception would come to C as "go on".
hook = pdf.hooks.before("annotation.add", lambda operation: {"not json": object()})
refuses(lambda: pdf.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]}), "a change that cannot be written refuses")
zebkit.remove_pdf_hook(hook)


def exits(operation):
    raise SystemExit(0)


hook = pdf.hooks.before("annotation.add", exits)
refuses(lambda: pdf.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]}), "SystemExit refuses too")
zebkit.remove_pdf_hook(hook)

# A canvas kept after its call draws on nothing.
kept_canvases = []
hook = pdf.hooks.layer(lambda canvas, index, info: kept_canvases.append(canvas))
pdf.render(0)
zebkit.remove_pdf_hook(hook)
kept_canvases[0].begin_path()
kept_canvases[0].rect(1, 1, 2, 2)
kept_canvases[0].fill()
check(kept_canvases[0].width == 0, "a canvas kept after its call has nothing behind it")

# A closed document's hook is not one for every document.
gone = zebkit.Pdf(bytes(pdf.save()))
gone.close()
refuses(lambda: gone.hooks.before("open", lambda operation: {"refuse": "all"}), "no hook on a closed document")
gone = zebkit.Pdf(bytes(pdf.save()))
gone.close()

# A stride below zero is not a large one.
refuses_value = False
try:
    zebkit.Canvas.over(bytearray(16), 1, 1, stride=-4, unpremultiplied=True)
except ValueError:
    refuses_value = True
check(refuses_value, "a negative stride")

# A hook that removes itself, and one that closes the document.
holder = {}


def removes_itself(operation):
    holder["calls"] = holder.get("calls", 0) + 1
    zebkit.remove_pdf_hook(holder["id"])


holder["id"] = pdf.hooks.after("annotation.add", removes_itself)
pdf.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]})
pdf.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]})
check(holder["calls"] == 1, "a hook that removes itself")

closing = blank()
closing.hooks.before("annotation.add", lambda operation: closing.close())
refuses(lambda: closing.add_annotation(0, {"type": "square", "rect": [1, 1, 2, 2]}), "a hook that closes the document")

# Layers, OCR, fonts, actions, filters, for all.
page = blank()
page.hooks.layer(lambda canvas, index, info: (canvas.set_fill_color(0xff0000ff), canvas.rect(200, 200, 20, 20), canvas.fill()))
check(near(page_pixel(page.render(0), 210, 210), 255, 0, 0), "a layer, in page space")
page.hooks.ocr(lambda index, width, height, rgba, scale: [{"text": "from OCR", "box": [1, 1, 2, 2]}])
check(page.text(0) == "from OCR", "OCR")
page.hooks.font(lambda family: font if family == "Boxes" else None)
page.add_annotation(0, {"type": "freeText", "rect": [10, 10, 200, 60], "contents": "X", "font": "Boxes", "fontSize": 20, "textColor": [0, 0, 255]})
check(near(page_pixel(page.render(0), 20, 45), 0, 0, 255), "a font from a hook")
link = page.add_annotation(0, {"type": "link", "rect": [1, 1, 9, 9], "url": "https://example.com/"})
page.hooks.action(lambda action: action["kind"] == "uri")
check(page.annotation_action(0, link)["handled"] is True, "an action taken")
page.hooks.input("ink", lambda points: points + [[9, 9]])
check(page.filter_input("ink", [[0, 0]])[-1] == [9.0, 9.0], "a filter")
check(zebkit.pdf_filter_input([[12, 18]], {"snapGrid": 10}) == [[10, 20]], "built-in filters with no document")
global_hook = zebkit.pdf_hooks.before("open", lambda operation: {"refuse": "closed"})
refuses(blank, "opening refused, for all documents")
zebkit.remove_pdf_hook(global_hook)
page.close()
sys.exit(finish())
