//! `<zebkit/canvas.h>`, `<zebkit/text.h>` and `<zebkit/pdf.h>`'s annotations and
//! hooks from Rust. None of it needs the engine running.

use std::sync::{Arc, Mutex};
use zebkit::canvas::{shape_text, Canvas, Font};
use zebkit::pdf::{self, Decision, Document, Interchange, Kind, RenderOptions};

const PAGE: &[u8] = b"%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n\
2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n\
3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >> endobj\n\
trailer << /Root 1 0 R >>\n%%EOF\n";

fn pixel(document: &mut Document, x: usize, y: usize) -> [u8; 3] {
    let image = document.render(0, &RenderOptions::default()).unwrap();
    let at = ((image.height as usize - 1 - y) * image.width as usize + x) * 4;
    [image.rgba[at], image.rgba[at + 1], image.rgba[at + 2]]
}

fn near(p: [u8; 3], color: [i32; 3]) -> bool {
    (0..3).all(|i| (p[i] as i32 - color[i]).abs() <= 40)
}

#[test]
fn canvas_draws_and_refuses() {
    let mut canvas = Canvas::new(40, 40).unwrap();
    canvas.set_fill_color(0xff0000ff);
    canvas.rect(0.0, 0.0, 20.0, 40.0);
    canvas.fill(false);
    assert!(canvas.set_linear_gradient((20.0, 0.0), (40.0, 0.0), &[0.0, 1.0], &[0x000000ff, 0xffffffff]));
    assert!(!canvas.set_blend("Nope"));
    assert!(canvas.fill_text("x", 0.0, 0.0).is_err());
    assert!(canvas.run("[[\"nothing\"]]").is_err());
    let pixels = canvas.pixels().unwrap();
    assert_eq!(&pixels[..4], &[255, 0, 0, 255]);
    assert_eq!(&canvas.png().unwrap()[..4], b"\x89PNG");
    assert!(Canvas::new(0, 0).is_err());
    assert!(Font::new(b"nope").is_err());
    let mut mine = vec![255u8; 4 * 4 * 4];
    Canvas::on_pixels(&mut mine, 4, 4, |canvas| {
        canvas.set_fill_color(0x0000ffff);
        canvas.rect(0.0, 0.0, 2.0, 2.0);
        canvas.fill(false);
    })
    .unwrap();
    assert_eq!(&mine[..4], &[0, 0, 255, 255]);
    assert!(shape_text(b"nope", 12.0, "a", None, None).is_err());
}

#[test]
fn annotations_and_interchange() {
    let mut document = Document::open(PAGE, None).unwrap();
    let line = document
        .add_annotation(
            0,
            r#"{"type":"line","line":[50,100,250,100],"color":[255,0,0],"border":2,
                "lineEndings":["none","closedArrow"],"interiorColor":[0,0,255]}"#,
        )
        .unwrap();
    assert!(near(pixel(&mut document, 243, 100), [0, 0, 255]));
    assert_eq!(document.annotation_at(0, 80.0, 100.0, 2.0).unwrap(), Some(line));
    assert!(document.add_annotation(0, r#"{"type":"line","line":[1,1,2,2],"lineEndings":["no","none"]}"#).is_err());
    let stamp = document.add_annotation(0, r#"{"type":"stamp","rect":[10,200,110,290],"stamp":"Draft"}"#).unwrap();
    document.move_annotation(0, stamp, 0).unwrap();
    for format in [Interchange::Xfdf, Interchange::Fdf] {
        let file = document.export_annotations(format, None, None).unwrap();
        let mut other = Document::open(PAGE, None).unwrap();
        assert_eq!(other.import_annotations(format, &file).unwrap(), 2);
        assert!(near(pixel(&mut other, 243, 100), [0, 0, 255]));
        assert!(other.import_annotations(Interchange::Xfdf, b"<!DOCTYPE a [<!ENTITY b 'c'>]><xfdf/>").is_err());
    }
    let file = document.add_annotation(0, r#"{"type":"fileAttachment","rect":[1,1,9,9]}"#).unwrap();
    document.attach_file(0, file, "a.bin", b"\x00data").unwrap();
    let mut saved = Document::open(&document.save(false).unwrap(), None).unwrap();
    let (name, data) = saved.annotation_file(0, file).unwrap();
    assert_eq!((name.as_str(), &data[..]), ("a.bin", &b"\x00data"[..]));
}

#[test]
fn hooks_refuse_change_and_undo() {
    let mut document = Document::open(PAGE, None).unwrap();
    let inverses = Arc::new(Mutex::new(Vec::new()));
    let kept = inverses.clone();
    pdf::hook_after(Some(&document), "annotation.add", move |_, operation| {
        kept.lock().unwrap().push(operation.to_string());
    })
    .unwrap();
    let refusing = pdf::hook_before(Some(&document), "annotation.add", |_, _| Decision::Refuse("not today".into())).unwrap();
    let error = document.add_annotation(0, r#"{"type":"square","rect":[1,1,2,2]}"#).unwrap_err();
    assert_eq!(error.message(), "not today");
    assert!(refusing.remove());
    assert!(!refusing.remove());
    assert_eq!(inverses.lock().unwrap().len(), 0);

    let change = pdf::hook_before(Some(&document), "annotation.add", |_, _| {
        Decision::Change(
            r#"{"page":0,"annotation":{"type":"square","rect":[10,10,50,50],"interiorColor":[0,0,255],"border":0}}"#.into(),
        )
    })
    .unwrap();
    document.add_annotation(0, r#"{"type":"square","rect":[1,1,2,2]}"#).unwrap();
    assert!(near(pixel(&mut document, 30, 30), [0, 0, 255]));
    change.remove();
    assert_eq!(inverses.lock().unwrap().len(), 1);
    let operation = inverses.lock().unwrap()[0].clone();
    let start = operation.find("\"inverse\":").unwrap() + "\"inverse\":".len();
    // A removal's inverse holds no object of its own: it ends at the first brace.
    let end = start + operation[start..].find('}').unwrap() + 1;
    let inverse = operation[start..end].to_string();
    document.apply(&inverse).unwrap();
    assert!(near(pixel(&mut document, 30, 30), [255, 255, 255]));

    // A kind that draws itself, a layer, a provider of text, a filter.
    pdf::hook_kind(
        Some(&document),
        Kind {
            name: "Sample_Mark",
            prefix: "Sample_",
            subtype: None,
            schema: Some(r#"{"mood":{"type":"string","required":true}}"#),
            draw: |canvas: &mut Canvas, _annotation: &str, width: f64, height: f64| {
                canvas.set_fill_color(0x00ff00ff);
                canvas.rect(0.0, 0.0, width, height);
                canvas.fill(false);
                true
            },
        },
    )
    .unwrap();
    assert!(document.add_annotation(0, r#"{"type":"Sample_Mark","rect":[60,60,90,90],"data":{}}"#).is_err());
    document.add_annotation(0, r#"{"type":"Sample_Mark","rect":[60,60,90,90],"data":{"mood":"x"}}"#).unwrap();
    assert!(near(pixel(&mut document, 75, 75), [0, 255, 0]));
    assert!(document.annotations(0).unwrap().contains(r#""type":"Sample_Mark""#));
    pdf::hook_layer(Some(&document), true, |canvas, _page, _info| {
        canvas.set_fill_color(0xff0000ff);
        canvas.rect(200.0, 200.0, 20.0, 20.0);
        canvas.fill(false);
    })
    .unwrap();
    assert!(near(pixel(&mut document, 210, 210), [255, 0, 0]));
    pdf::hook_ocr(Some(&document), |_page, pixels, width, height, _scale| {
        assert_eq!(pixels.len(), width as usize * height as usize * 4);
        Some(r#"[{"text":"from OCR","box":[1,1,2,2]}]"#.to_string())
    })
    .unwrap();
    assert_eq!(document.text(0).unwrap(), "from OCR");
    pdf::hook_input(Some(&document), "ink", |_tool, _points| Some("[[1,2]]".to_string())).unwrap();
    assert_eq!(document.filter_input("ink", "[[9,9]]", None).unwrap(), "[[1.0,2.0]]");
    assert_eq!(pdf::filter_input("[[12,18]]", Some(r#"{"snapGrid":10}"#)).unwrap(), "[[10.0,20.0]]");
    assert!(pdf::filter_input("[[1]]", None).is_err());
}
