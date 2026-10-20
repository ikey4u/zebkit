//! PDF documents on the engine's PDFium: `<zebkit/pdf.h>`.
//!
//! Structured answers are JSON, as the C API gives them, for the
//! application to read with the JSON library it uses; the C header says
//! what each holds. Coordinates are PDF page space: points, the origin at
//! the bottom left, before the page's rotation. None of it needs
//! [`crate::run`]; a document may move between threads, and PDFium does one
//! thing at a time.

use crate::services::Image;
use crate::{c_string, sys, take_bytes, take_error, take_string, Error};
use std::os::raw::c_int;
use std::path::Path;

/// What [`Document::render`] draws.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RenderOptions {
    /// Pixels per point.
    pub scale: f32,
    /// 0, 90, 180 or 270 degrees, added to the page's own.
    pub rotation: i32,
    /// `sys::ZEBKIT_PDF_RENDER_*` bits.
    pub flags: u32,
    /// 0xRRGGBBAA; 0 is transparent.
    pub background: u32,
    /// A tile of the rendered page, in its pixels: (x, y, width, height).
    /// `None` is the whole page.
    pub tile: Option<(i32, i32, i32, i32)>,
}

impl Default for RenderOptions {
    fn default() -> Self {
        RenderOptions {
            scale: 1.0,
            rotation: 0,
            flags: sys::ZEBKIT_PDF_RENDER_ANNOTATIONS,
            background: 0xffff_ffff,
            tile: None,
        }
    }
}

/// An open PDF document.
pub struct Document {
    raw: *mut sys::zebkit_pdf,
}

// The C API takes one lock around PDFium: a document may be used from any
// thread, one at a time, which &mut and Send give.
unsafe impl Send for Document {}

fn page_index(page: usize) -> c_int {
    c_int::try_from(page).unwrap_or(c_int::MAX)
}

impl Document {
    /// Opens a PDF from its bytes, which are copied.
    pub fn open(data: &[u8], password: Option<&str>) -> Result<Document, Error> {
        let password = password.map(c_string);
        let mut error = std::ptr::null_mut();
        let raw = unsafe {
            sys::zebkit_pdf_open(
                data.as_ptr().cast(),
                data.len(),
                password.as_ref().map_or(std::ptr::null(), |p| p.as_ptr()),
                &mut error,
            )
        };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "the document cannot be opened") });
        }
        Ok(Document { raw })
    }

    /// Opens a PDF file, which is read as it is needed.
    pub fn open_file(path: &Path, password: Option<&str>) -> Result<Document, Error> {
        let password = password.map(c_string);
        let mut error = std::ptr::null_mut();
        let raw = unsafe {
            sys::zebkit_pdf_open_file(
                c_string(&path.to_string_lossy()).as_ptr(),
                password.as_ref().map_or(std::ptr::null(), |p| p.as_ptr()),
                &mut error,
            )
        };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "the document cannot be opened") });
        }
        Ok(Document { raw })
    }

    fn json(&mut self, answer: *mut std::os::raw::c_char, error: *mut std::os::raw::c_char) -> Result<String, Error> {
        if answer.is_null() {
            return Err(unsafe { take_error(error, "failed") });
        }
        Ok(unsafe { take_string(answer) }.unwrap_or_default())
    }

    fn done(&mut self, ok: bool, error: *mut std::os::raw::c_char, fallback: &str) -> Result<(), Error> {
        if ok {
            Ok(())
        } else {
            Err(unsafe { take_error(error, fallback) })
        }
    }

    /// Pages, version, encryption, permissions, form, language, metadata.
    pub fn info(&mut self) -> String {
        unsafe { take_string(sys::zebkit_pdf_info(self.raw)) }.unwrap_or_default()
    }

    /// Sets the document information from a JSON object of its entries.
    pub fn set_metadata(&mut self, json: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_set_metadata(self.raw, c_string(json).as_ptr(), &mut error) };
        self.done(ok, error, "the metadata cannot be set")
    }

    pub fn page_count(&mut self) -> usize {
        usize::try_from(unsafe { sys::zebkit_pdf_page_count(self.raw) }).unwrap_or(0)
    }

    /// A page's size as shown, rotation, label and boxes.
    pub fn page(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_page(self.raw, page_index(page), &mut error) };
        self.json(answer, error)
    }

    /// The page, or a tile of it, as RGBA.
    pub fn render(&mut self, page: usize, options: &RenderOptions) -> Result<Image, Error> {
        let (x, y, width, height) = options.tile.unwrap_or((0, 0, 0, 0));
        let raw = sys::zebkit_pdf_render_options {
            scale: options.scale,
            rotation: options.rotation,
            flags: options.flags,
            background: options.background,
            x,
            y,
            width,
            height,
        };
        let (mut out_width, mut out_height, mut error) = (0, 0, std::ptr::null_mut());
        unsafe {
            let pixels =
                sys::zebkit_pdf_render(self.raw, page_index(page), &raw, &mut out_width, &mut out_height, &mut error);
            if pixels.is_null() {
                return Err(take_error(error, "the page cannot be rendered"));
            }
            let (width, height) = (out_width.max(0) as u32, out_height.max(0) as u32);
            Ok(Image { width, height, rgba: take_bytes(pixels, width as usize * height as usize * 4) })
        }
    }

    /// Appearance `mode` (0 normal, 1 rollover, 2 down) of annotation
    /// `annotation` alone, over its rectangle, at the options' scale and
    /// rotation on their background (the tile and flags are not used).
    /// `Ok(None)` when it has no such appearance.
    pub fn render_annotation(
        &mut self,
        page: usize,
        annotation: usize,
        mode: i32,
        options: &RenderOptions,
    ) -> Result<Option<Image>, Error> {
        let raw = sys::zebkit_pdf_render_options {
            scale: options.scale,
            rotation: options.rotation,
            flags: 0,
            background: options.background,
            x: 0,
            y: 0,
            width: 0,
            height: 0,
        };
        let (mut out_width, mut out_height, mut error) = (0, 0, std::ptr::null_mut());
        unsafe {
            let pixels = sys::zebkit_pdf_render_annotation(
                self.raw,
                page_index(page),
                page_index(annotation),
                mode,
                &raw,
                &mut out_width,
                &mut out_height,
                &mut error,
            );
            if pixels.is_null() {
                let error = take_error(error, "the annotation cannot be rendered");
                return if error.message() == "no appearance" { Ok(None) } else { Err(error) };
            }
            let (width, height) = (out_width.max(0) as u32, out_height.max(0) as u32);
            Ok(Some(Image { width, height, rgba: take_bytes(pixels, width as usize * height as usize * 4) }))
        }
    }

    pub fn text(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_text(self.raw, page_index(page), &mut error) };
        self.json(answer, error)
    }

    /// Each character with its box, font, size and angle.
    pub fn text_layout(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_text_layout(self.raw, page_index(page), &mut error) };
        self.json(answer, error)
    }

    /// The rectangles a selection of `count` characters from `start` draws.
    pub fn text_rects(&mut self, page: usize, start: usize, count: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe {
            sys::zebkit_pdf_text_rects(self.raw, page_index(page), page_index(start), page_index(count), &mut error)
        };
        self.json(answer, error)
    }

    /// The character at (x, y), within `tolerance` points.
    pub fn char_at(&mut self, page: usize, x: f64, y: f64, tolerance: f64) -> Option<usize> {
        usize::try_from(unsafe { sys::zebkit_pdf_char_at(self.raw, page_index(page), x, y, tolerance) }).ok()
    }

    /// Every match from page `first` on, at most `limit` (0 for all).
    pub fn search(&mut self, query: &str, match_case: bool, whole_word: bool, first: usize, limit: usize)
        -> Result<String, Error> {
        let mut flags = 0;
        if match_case {
            flags |= sys::ZEBKIT_PDF_SEARCH_CASE;
        }
        if whole_word {
            flags |= sys::ZEBKIT_PDF_SEARCH_WHOLE_WORD;
        }
        let mut error = std::ptr::null_mut();
        let answer = unsafe {
            sys::zebkit_pdf_search(
                self.raw,
                c_string(query).as_ptr(),
                flags,
                page_index(first),
                page_index(limit),
                &mut error,
            )
        };
        self.json(answer, error)
    }

    pub fn outline(&mut self) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_outline(self.raw, &mut error) };
        self.json(answer, error)
    }

    pub fn links(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_links(self.raw, page_index(page), &mut error) };
        self.json(answer, error)
    }

    pub fn annotations(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_annotations(self.raw, page_index(page), &mut error) };
        self.json(answer, error)
    }

    /// Adds an annotation from a JSON object; returns its index.
    pub fn add_annotation(&mut self, page: usize, json: &str) -> Result<usize, Error> {
        let mut error = std::ptr::null_mut();
        let index =
            unsafe { sys::zebkit_pdf_annotation_add(self.raw, page_index(page), c_string(json).as_ptr(), &mut error) };
        usize::try_from(index).map_err(|_| unsafe { take_error(error, "the annotation cannot be made") })
    }

    pub fn update_annotation(&mut self, page: usize, index: usize, json: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_annotation_update(
                self.raw,
                page_index(page),
                page_index(index),
                c_string(json).as_ptr(),
                &mut error,
            )
        };
        self.done(ok, error, "the annotation cannot be changed")
    }

    pub fn remove_annotation(&mut self, page: usize, index: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_annotation_remove(self.raw, page_index(page), page_index(index), &mut error) };
        self.done(ok, error, "the annotation cannot be removed")
    }

    pub fn flatten(&mut self, page: usize, for_print: bool) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_flatten(self.raw, page_index(page), for_print, &mut error) };
        self.done(ok, error, "the page cannot be flattened")
    }

    pub fn attachments(&mut self) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_attachments(self.raw, &mut error) };
        self.json(answer, error)
    }

    pub fn attachment(&mut self, index: usize) -> Result<Vec<u8>, Error> {
        let (mut size, mut error) = (0, std::ptr::null_mut());
        unsafe {
            let data = sys::zebkit_pdf_attachment(self.raw, page_index(index), &mut size, &mut error);
            if data.is_null() {
                return Err(take_error(error, "no attachment"));
            }
            Ok(take_bytes(data, size))
        }
    }

    pub fn attach(&mut self, name: &str, data: &[u8]) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_attach(self.raw, c_string(name).as_ptr(), data.as_ptr().cast(), data.len(), &mut error)
        };
        self.done(ok, error, "the file cannot be attached")
    }

    pub fn detach(&mut self, index: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_detach(self.raw, page_index(index), &mut error) };
        self.done(ok, error, "no attachment")
    }

    /// An empty document, to build from others' pages.
    pub fn new() -> Result<Document, Error> {
        let mut error = std::ptr::null_mut();
        let raw = unsafe { sys::zebkit_pdf_new(&mut error) };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "the document cannot be made") });
        }
        Ok(Document { raw })
    }

    /// Takes what is under `areas_json` (a JSON array of [l, b, r, t]; `None`
    /// for the page's redaction marks) out of the page for good, with
    /// boxes of `fill` (0xRRGGBBAA, 0 for none) over it. Returns how many
    /// areas.
    pub fn redact(&mut self, page: usize, areas_json: Option<&str>, fill: u32) -> Result<usize, Error> {
        let areas = areas_json.map(c_string);
        let mut error = std::ptr::null_mut();
        let count = unsafe {
            sys::zebkit_pdf_redact(
                self.raw,
                page_index(page),
                areas.as_ref().map_or(std::ptr::null(), |a| a.as_ptr()),
                fill,
                &mut error,
            )
        };
        usize::try_from(count).map_err(|_| unsafe { take_error(error, "the page cannot be redacted") })
    }

    /// The form's fields, as JSON.
    pub fn form_fields(&mut self) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_form_fields(self.raw, &mut error) };
        self.json(answer, error)
    }

    /// Sets field `name` to `value` (text, an option, or a button's export value).
    pub fn set_field(&mut self, name: &str, value: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok =
            unsafe { sys::zebkit_pdf_form_set(self.raw, c_string(name).as_ptr(), c_string(value).as_ptr(), &mut error) };
        self.done(ok, error, "the field cannot be set")
    }

    pub fn reset_form(&mut self) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_form_reset(self.raw, &mut error) };
        self.done(ok, error, "the form cannot be reset")
    }

    /// Sets a page's rotation: 0, 90, 180 or 270.
    pub fn rotate_page(&mut self, page: usize, rotation: i32) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_page_rotate(self.raw, page_index(page), rotation, &mut error) };
        self.done(ok, error, "the page cannot be turned")
    }

    pub fn delete_pages(&mut self, pages: &[i32]) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_pages_delete(self.raw, pages.as_ptr(), page_index(pages.len()), &mut error)
        };
        self.done(ok, error, "the pages cannot be deleted")
    }

    /// Moves pages, in the order given, to start at index `to`.
    pub fn move_pages(&mut self, pages: &[i32], to: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_pages_move(self.raw, pages.as_ptr(), page_index(pages.len()), page_index(to), &mut error)
        };
        self.done(ok, error, "the pages cannot be moved")
    }

    /// Inserts a blank page, `width` by `height` points, at index `at`.
    pub fn insert_page(&mut self, at: usize, width: f64, height: f64) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_page_insert(self.raw, page_index(at), width, height, &mut error) };
        self.done(ok, error, "the page cannot be made")
    }

    /// Copies pages of `from` (all for `None`) in at index `at`.
    pub fn import_pages(&mut self, from: &mut Document, pages: Option<&[i32]>, at: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let (pointer, count) = pages.map_or((std::ptr::null(), 0), |p| (p.as_ptr(), page_index(p.len())));
        let ok = unsafe { sys::zebkit_pdf_pages_import(self.raw, from.raw, pointer, count, page_index(at), &mut error) };
        self.done(ok, error, "the pages cannot be copied")
    }

    /// The document as a file, whole or with its changes appended.
    pub fn save(&mut self, incremental: bool) -> Result<Vec<u8>, Error> {
        let flags = if incremental { sys::ZEBKIT_PDF_SAVE_INCREMENTAL } else { 0 };
        let (mut size, mut error) = (0, std::ptr::null_mut());
        unsafe {
            let data = sys::zebkit_pdf_save(self.raw, flags, &mut size, &mut error);
            if data.is_null() {
                return Err(take_error(error, "the document cannot be saved"));
            }
            Ok(take_bytes(data, size))
        }
    }
}

impl Drop for Document {
    fn drop(&mut self) {
        unsafe { sys::zebkit_pdf_close(self.raw) }
    }
}
