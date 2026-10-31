//! PDF documents on the engine's PDFium: `<zebkit/pdf.h>`.
//!
//! Structured answers are JSON, as the C API gives them, for the
//! application to read with the JSON library it uses; the C header says
//! what each holds. Coordinates are PDF page space: points, the origin at
//! the bottom left, before the page's rotation. None of it needs
//! [`crate::run`]; a document may move between threads, and PDFium does one
//! thing at a time.

use crate::services::Image;
use crate::canvas::Canvas;
use crate::{c_string, string_from, sys, take_bytes, take_error, take_string, Error};
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
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

    /// The digital signatures, as JSON: field, subFilter, reason, time,
    /// byteRange, docMDP, size and the signer's certificate.
    pub fn signatures(&mut self) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_signatures(self.raw, &mut error) };
        self.json(answer, error)
    }

    /// Signature `index`'s contents: the DER-encoded PKCS#7 it holds.
    pub fn signature_contents(&mut self, index: usize) -> Result<Vec<u8>, Error> {
        let (mut size, mut error) = (0, std::ptr::null_mut());
        unsafe {
            let data = sys::zebkit_pdf_signature_contents(self.raw, page_index(index), &mut size, &mut error);
            if data.is_null() {
                return Err(take_error(error, "no such signature"));
            }
            Ok(take_bytes(data, size))
        }
    }

    /// Image `image` of the page, as [`Document::images`] lists them: its
    /// JPEG as stored (and true), or its pixels as PNG (and false).
    pub fn image_data(&mut self, page: usize, image: usize) -> Result<(Vec<u8>, bool), Error> {
        let (mut size, mut jpeg, mut error) = (0, false, std::ptr::null_mut());
        unsafe {
            let data =
                sys::zebkit_pdf_image_data(self.raw, page_index(page), page_index(image), &mut size, &mut jpeg, &mut error);
            if data.is_null() {
                return Err(take_error(error, "no such image"));
            }
            Ok((take_bytes(data, size), jpeg))
        }
    }

    /// The text as lines, each with its box and how many of its characters
    /// have no Unicode of their own.
    pub fn text_lines(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_text_lines(self.raw, page_index(page), &mut error) };
        self.json(answer, error)
    }

    /// The images the page draws: pixel size and where.
    pub fn images(&mut self, page: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_images(self.raw, page_index(page), &mut error) };
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

    /// Replaces bookmarks as a JSON tree; an empty array removes them.
    pub fn set_outline(&mut self, json: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_set_outline(self.raw, c_string(json).as_ptr(), &mut error) };
        self.done(ok, error, "the outline cannot be set")
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

    /// A stamp's appearance as an image: `rgba`, `width` by `height`, rows
    /// top to bottom.
    pub fn set_annotation_image(&mut self, page: usize, index: usize, rgba: &[u8], width: u32, height: u32)
        -> Result<(), Error> {
        if rgba.len() != width as usize * height as usize * 4 {
            return Err(Error::new("the image is not width by height RGBA"));
        }
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_annotation_set_image(self.raw, page_index(page), page_index(index), rgba.as_ptr(),
                                                 width as i32, height as i32, &mut error)
        };
        self.done(ok, error, "the stamp's image cannot be set")
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

/// An XFDF or FDF file, for [`Document::export_annotations`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Interchange {
    Xfdf,
    Fdf,
}

impl Interchange {
    fn raw(self) -> c_int {
        match self {
            Interchange::Xfdf => sys::ZEBKIT_PDF_XFDF,
            Interchange::Fdf => sys::ZEBKIT_PDF_FDF,
        }
    }
}

impl Document {
    /// The top annotation at a point within `tolerance` points.
    pub fn annotation_at(&mut self, page: usize, x: f64, y: f64, tolerance: f64) -> Result<Option<usize>, Error> {
        let mut error = std::ptr::null_mut();
        let at = unsafe { sys::zebkit_pdf_annotation_at(self.raw, page_index(page), x, y, tolerance, &mut error) };
        if at < 0 && !error.is_null() {
            return Err(unsafe { take_error(error, "no page") });
        }
        Ok(usize::try_from(at).ok())
    }

    /// Moves annotation `index` to `to` in the page's order: later is on top.
    pub fn move_annotation(&mut self, page: usize, index: usize, to: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_annotation_move(self.raw, page_index(page), page_index(index), page_index(to), &mut error)
        };
        self.done(ok, error, "the annotation cannot be moved")
    }

    /// Copies annotation `index` to `target_page` of `target`; `None` for this document.
    pub fn copy_annotation(
        &mut self,
        page: usize,
        index: usize,
        target: Option<&mut Document>,
        target_page: usize,
    ) -> Result<usize, Error> {
        let mut error = std::ptr::null_mut();
        let to = target.map_or(self.raw, |document| document.raw);
        let at = unsafe {
            sys::zebkit_pdf_annotation_copy(self.raw, page_index(page), page_index(index), to, page_index(target_page), &mut error)
        };
        usize::try_from(at).map_err(|_| unsafe { take_error(error, "the annotation cannot be copied") })
    }

    /// Makes annotation `index`'s appearance part of the page, and removes it.
    pub fn flatten_annotation(&mut self, page: usize, index: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_annotation_flatten(self.raw, page_index(page), page_index(index), &mut error) };
        self.done(ok, error, "the annotation cannot be flattened")
    }

    /// Makes `source_page` of `source`, as vectors, the appearance of stamp `index`.
    pub fn stamp_from_page(&mut self, page: usize, index: usize, source: &mut Document, source_page: usize) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_annotation_stamp_page(self.raw, page_index(page), page_index(index), source.raw, page_index(source_page), &mut error)
        };
        self.done(ok, error, "the page cannot be made a stamp")
    }

    /// Embeds a file in file attachment annotation `index`.
    pub fn attach_file(&mut self, page: usize, index: usize, name: &str, data: &[u8]) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_annotation_attach_file(self.raw, page_index(page), page_index(index), c_string(name).as_ptr(), data.as_ptr().cast(), data.len(), &mut error)
        };
        self.done(ok, error, "the file cannot be attached")
    }

    /// The name and bytes of the file in annotation `index`.
    pub fn annotation_file(&mut self, page: usize, index: usize) -> Result<(String, Vec<u8>), Error> {
        let (mut name, mut size, mut error) = (std::ptr::null_mut(), 0, std::ptr::null_mut());
        unsafe {
            let data = sys::zebkit_pdf_annotation_file(self.raw, page_index(page), page_index(index), &mut name, &mut size, &mut error);
            if data.is_null() {
                return Err(take_error(error, "the annotation has no file"));
            }
            Ok((take_string(name).unwrap_or_default(), take_bytes(data, size)))
        }
    }

    /// Draws `ops` (see [`Canvas::run`]) as the appearance of annotation `index`.
    pub fn draw_annotation(&mut self, page: usize, index: usize, ops: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_annotation_draw(self.raw, page_index(page), page_index(index), c_string(ops).as_ptr(), &mut error)
        };
        self.done(ok, error, "the appearance cannot be drawn")
    }

    /// What activating annotation `index` means, as JSON.
    pub fn annotation_action(&mut self, page: usize, index: usize) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_annotation_action(self.raw, page_index(page), page_index(index), &mut error) };
        self.json(answer, error)
    }

    /// The annotations as a file: of `page`, or of all; only `index` of it, or all.
    pub fn export_annotations(&mut self, format: Interchange, page: Option<usize>, index: Option<usize>) -> Result<Vec<u8>, Error> {
        let (mut size, mut error) = (0, std::ptr::null_mut());
        unsafe {
            let data = sys::zebkit_pdf_annotations_export(
                self.raw,
                format.raw(),
                page.map_or(-1, page_index),
                index.map_or(-1, page_index),
                &mut size,
                &mut error,
            );
            if data.is_null() {
                return Err(take_error(error, "the annotations cannot be written"));
            }
            Ok(take_bytes(data, size))
        }
    }

    /// Adds the annotations of a file; how many.
    pub fn import_annotations(&mut self, format: Interchange, data: &[u8]) -> Result<usize, Error> {
        let mut error = std::ptr::null_mut();
        let count = unsafe {
            sys::zebkit_pdf_annotations_import(self.raw, format.raw(), data.as_ptr().cast(), data.len(), &mut error)
        };
        usize::try_from(count).map_err(|_| unsafe { take_error(error, "the annotations cannot be read") })
    }

    /// Runs an operation a hook saw, or its `"inverse"`, as JSON.
    pub fn apply(&mut self, operation: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe { sys::zebkit_pdf_apply(self.raw, c_string(operation).as_ptr(), &mut error) };
        self.done(ok, error, "the operation cannot be run")
    }

    /// What flatten kept of annotations with entries of `prefix`, as JSON.
    pub fn page_data(&mut self, page: usize, prefix: &str) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let answer = unsafe { sys::zebkit_pdf_page_data(self.raw, page_index(page), c_string(prefix).as_ptr(), &mut error) };
        self.json(answer, error)
    }

    /// Family `family` of free text is this file; `""` for what the rest lack.
    pub fn add_font(&mut self, family: &str, font: &[u8]) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        let ok = unsafe {
            sys::zebkit_pdf_font_add(self.raw, c_string(family).as_ptr(), font.as_ptr().cast(), font.len(), &mut error)
        };
        self.done(ok, error, "the font cannot be used")
    }

    /// The points of a drawing tool through the built-in filters and hooks: JSON in and out.
    pub fn filter_input(&mut self, tool: &str, points: &str, options: Option<&str>) -> Result<String, Error> {
        let mut error = std::ptr::null_mut();
        let options = options.map(c_string);
        let answer = unsafe {
            sys::zebkit_pdf_filter_input(
                self.raw,
                c_string(tool).as_ptr(),
                c_string(points).as_ptr(),
                options.as_ref().map_or(std::ptr::null(), |o| o.as_ptr()),
                &mut error,
            )
        };
        self.json(answer, error)
    }

    fn raw_or_null(document: Option<&Document>) -> *mut sys::zebkit_pdf {
        document.map_or(std::ptr::null_mut(), |d| d.raw)
    }
}

/// What a "before" hook says.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Decision {
    /// Go on as it is.
    Continue,
    /// Go on with this operation instead, as JSON.
    Change(String),
    /// Nothing happens; the call fails with this reason.
    Refuse(String),
}

/// What a trust hook says of a signer.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Trust {
    Trusted,
    Untrusted,
    Unknown,
}

/// A hook of `<zebkit/pdf.h>`: every function here takes the document it is
/// for, or `None` for all. It goes when [`Hook::remove`] is called or its
/// document does; dropping this value alone leaves it in place. Callbacks run
/// on the thread that made the call that needs them, and may call on the
/// document they are given, whose owner holds it too.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Hook(c_int);

impl Hook {
    /// False when it is gone already.
    pub fn remove(self) -> bool {
        unsafe { sys::zebkit_pdf_hook_remove(self.0) }
    }
}

fn hook_result(id: c_int) -> Result<Hook, Error> {
    if id > 0 {
        Ok(Hook(id))
    } else {
        Err(Error::new("the hook cannot be registered"))
    }
}

/// A closure as a hook's data, freed by the engine once no call of it runs.
fn boxed<F>(function: F) -> *mut c_void {
    Box::into_raw(Box::new(function)).cast()
}

unsafe extern "C" fn destroy_boxed<F>(data: *mut c_void) {
    drop(Box::from_raw(data.cast::<F>()));
}

/// The document a callback is given: not its owner.
fn borrowed(pdf: *mut sys::zebkit_pdf) -> ManuallyDrop<Document> {
    ManuallyDrop::new(Document { raw: pdf })
}

fn give_string(text: &str) -> *mut c_char {
    unsafe { sys::zebkit_pdf_string(c_string(text).as_ptr()) }
}

unsafe extern "C" fn before_trampoline<F>(
    pdf: *mut sys::zebkit_pdf,
    operation: *const c_char,
    change: *mut *mut c_char,
    reason: *mut *mut c_char,
    data: *mut c_void,
) -> c_int
where
    F: Fn(Option<&mut Document>, &str) -> Decision + 'static,
{
    let function = &*data.cast::<F>();
    let text = string_from(operation).unwrap_or_default();
    let mut document = borrowed(pdf);
    let target = (!pdf.is_null()).then_some(&mut *document as &mut Document);
    // A hook that panics has decided nothing it can stand by.
    match catch_unwind(AssertUnwindSafe(|| function(target, &text))) {
        Ok(Decision::Continue) => sys::ZEBKIT_PDF_CONTINUE,
        Ok(Decision::Change(json)) => {
            *change = give_string(&json);
            sys::ZEBKIT_PDF_CONTINUE
        }
        Ok(Decision::Refuse(why)) => {
            *reason = give_string(&why);
            sys::ZEBKIT_PDF_REFUSE
        }
        Err(_) => {
            *reason = give_string("a hook panicked");
            sys::ZEBKIT_PDF_REFUSE
        }
    }
}

unsafe extern "C" fn after_trampoline<F>(pdf: *mut sys::zebkit_pdf, operation: *const c_char, data: *mut c_void)
where
    F: Fn(&mut Document, &str) + 'static,
{
    let function = &*data.cast::<F>();
    let text = string_from(operation).unwrap_or_default();
    let mut document = borrowed(pdf);
    let _ = catch_unwind(AssertUnwindSafe(|| function(&mut document, &text)));
}

/// Runs `function` before `event` ("annotation.add", ..., or "*").
pub fn hook_before<F>(document: Option<&Document>, event: &str, function: F) -> Result<Hook, Error>
where
    F: Fn(Option<&mut Document>, &str) -> Decision + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_before(
            Document::raw_or_null(document),
            c_string(event).as_ptr(),
            before_trampoline::<F>,
            boxed(function),
            Some(destroy_boxed::<F>),
        )
    })
}

/// Runs `function` after `event`, given the operation as JSON.
pub fn hook_after<F>(document: Option<&Document>, event: &str, function: F) -> Result<Hook, Error>
where
    F: Fn(&mut Document, &str) + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_after(
            Document::raw_or_null(document),
            c_string(event).as_ptr(),
            after_trampoline::<F>,
            boxed(function),
            Some(destroy_boxed::<F>),
        )
    })
}

unsafe extern "C" fn appearance_trampoline<F>(
    canvas: *mut sys::zebkit_canvas,
    _pdf: *mut sys::zebkit_pdf,
    _page: c_int,
    _index: c_int,
    annotation: *const c_char,
    width: f64,
    height: f64,
    data: *mut c_void,
) -> bool
where
    F: Fn(&mut Canvas, &str, f64, f64) -> bool + 'static,
{
    let function = &*data.cast::<F>();
    let text = string_from(annotation).unwrap_or_default();
    let mut canvas = Canvas::borrowed(canvas);
    catch_unwind(AssertUnwindSafe(|| function(&mut canvas, &text, width, height))).unwrap_or(false)
}

/// Draws type `kind`'s appearance (or `"*"`'s): given a canvas that writes
/// PDF operators, the annotation as JSON, and its width and height in points.
/// False leaves it to the generator.
pub fn hook_appearance<F>(document: Option<&Document>, kind: &str, function: F) -> Result<Hook, Error>
where
    F: Fn(&mut Canvas, &str, f64, f64) -> bool + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_appearance(
            Document::raw_or_null(document),
            c_string(kind).as_ptr(),
            appearance_trampoline::<F>,
            boxed(function),
            Some(destroy_boxed::<F>),
        )
    })
}

/// A kind of annotation of the application's own: [`hook_kind`].
pub struct Kind<'a, F> {
    pub name: &'a str,
    pub prefix: &'a str,
    /// `None`: /Subtype is `name`. Or a standard one, and /`prefix`Kind says which it is.
    pub subtype: Option<&'a str>,
    /// What "data" may hold, as JSON (`<zebkit/pdf.h>`).
    pub schema: Option<&'a str>,
    /// Its appearance, as [`hook_appearance`]'s.
    pub draw: F,
}

/// Registers a kind.
pub fn hook_kind<F>(document: Option<&Document>, kind: Kind<'_, F>) -> Result<Hook, Error>
where
    F: Fn(&mut Canvas, &str, f64, f64) -> bool + 'static,
{
    let name = c_string(kind.name);
    let prefix = c_string(kind.prefix);
    let subtype = kind.subtype.map(c_string);
    let schema = kind.schema.map(c_string);
    let raw = sys::zebkit_pdf_kind {
        name: name.as_ptr(),
        prefix: prefix.as_ptr(),
        subtype: subtype.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        schema: schema.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        check: None,
        draw: Some(appearance_trampoline::<F>),
        hit_test: None,
        data: boxed(kind.draw),
        destroy: Some(destroy_boxed::<F>),
    };
    let mut error = std::ptr::null_mut();
    let id = unsafe { sys::zebkit_pdf_hook_kind(Document::raw_or_null(document), &raw, &mut error) };
    if id <= 0 {
        unsafe { destroy_boxed::<F>(raw.data) };
        return Err(unsafe { take_error(error, "the kind cannot be registered") });
    }
    Ok(Hook(id))
}

unsafe extern "C" fn layer_trampoline<F>(
    canvas: *mut sys::zebkit_canvas,
    _pdf: *mut sys::zebkit_pdf,
    page: c_int,
    info: *const sys::zebkit_pdf_layer_info,
    data: *mut c_void,
) where
    F: Fn(&mut Canvas, usize, &sys::zebkit_pdf_layer_info) + 'static,
{
    let function = &*data.cast::<F>();
    let mut canvas = Canvas::borrowed(canvas);
    let _ = catch_unwind(AssertUnwindSafe(|| function(&mut canvas, page.max(0) as usize, &*info)));
}

/// Draws above (or below) a page's content when it is rendered, in page space.
pub fn hook_layer<F>(document: Option<&Document>, above: bool, function: F) -> Result<Hook, Error>
where
    F: Fn(&mut Canvas, usize, &sys::zebkit_pdf_layer_info) + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_layer(
            Document::raw_or_null(document),
            if above { sys::ZEBKIT_PDF_LAYER_ABOVE } else { sys::ZEBKIT_PDF_LAYER_BELOW },
            layer_trampoline::<F>,
            boxed(function),
            Some(destroy_boxed::<F>),
        )
    })
}

unsafe extern "C" fn ocr_trampoline<F>(
    _pdf: *mut sys::zebkit_pdf,
    page: c_int,
    rgba: *const u8,
    width: c_int,
    height: c_int,
    scale: f64,
    data: *mut c_void,
) -> *mut c_char
where
    F: Fn(usize, &[u8], u32, u32, f64) -> Option<String> + 'static,
{
    let function = &*data.cast::<F>();
    let (w, h) = (width.max(0) as usize, height.max(0) as usize);
    let pixels = std::slice::from_raw_parts(rgba, w * h * 4);
    match catch_unwind(AssertUnwindSafe(|| function(page.max(0) as usize, pixels, w as u32, h as u32, scale))) {
        Ok(Some(json)) => give_string(&json),
        _ => std::ptr::null_mut(),
    }
}

/// Text for pages with none of their own: given the page as RGBA, its size and
/// pixels per point, it returns `[{"text", "box"}]` as JSON.
pub fn hook_ocr<F>(document: Option<&Document>, function: F) -> Result<Hook, Error>
where
    F: Fn(usize, &[u8], u32, u32, f64) -> Option<String> + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_ocr(Document::raw_or_null(document), ocr_trampoline::<F>, boxed(function), Some(destroy_boxed::<F>))
    })
}

unsafe extern "C" fn font_trampoline<F>(
    _pdf: *mut sys::zebkit_pdf,
    family: *const c_char,
    size: *mut usize,
    data: *mut c_void,
) -> *mut u8
where
    F: Fn(&str) -> Option<Vec<u8>> + 'static,
{
    let function = &*data.cast::<F>();
    let family = string_from(family).unwrap_or_default();
    match catch_unwind(AssertUnwindSafe(|| function(&family))) {
        Ok(Some(bytes)) => {
            *size = bytes.len();
            sys::zebkit_pdf_bytes(bytes.as_ptr().cast(), bytes.len())
        }
        _ => std::ptr::null_mut(),
    }
}

/// Fonts for free text, by family; `""` for what the standard fonts lack.
pub fn hook_font<F>(document: Option<&Document>, function: F) -> Result<Hook, Error>
where
    F: Fn(&str) -> Option<Vec<u8>> + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_font(Document::raw_or_null(document), font_trampoline::<F>, boxed(function), Some(destroy_boxed::<F>))
    })
}

unsafe extern "C" fn trust_trampoline<F>(
    _pdf: *mut sys::zebkit_pdf,
    signature: *const c_char,
    contents: *const u8,
    size: usize,
    data: *mut c_void,
) -> c_int
where
    F: Fn(&str, &[u8]) -> Trust + 'static,
{
    let function = &*data.cast::<F>();
    let signature = string_from(signature).unwrap_or_default();
    let contents = if contents.is_null() { &[][..] } else { std::slice::from_raw_parts(contents, size) };
    match catch_unwind(AssertUnwindSafe(|| function(&signature, contents))) {
        Ok(Trust::Trusted) => sys::ZEBKIT_PDF_TRUSTED,
        Ok(Trust::Untrusted) => sys::ZEBKIT_PDF_UNTRUSTED,
        _ => sys::ZEBKIT_PDF_TRUST_UNKNOWN,
    }
}

/// Trust in a signer: given the signature as JSON and its PKCS#7.
pub fn hook_trust<F>(document: Option<&Document>, function: F) -> Result<Hook, Error>
where
    F: Fn(&str, &[u8]) -> Trust + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_trust(Document::raw_or_null(document), trust_trampoline::<F>, boxed(function), Some(destroy_boxed::<F>))
    })
}

unsafe extern "C" fn action_trampoline<F>(_pdf: *mut sys::zebkit_pdf, action: *const c_char, data: *mut c_void) -> bool
where
    F: Fn(&str) -> bool + 'static,
{
    let function = &*data.cast::<F>();
    let action = string_from(action).unwrap_or_default();
    catch_unwind(AssertUnwindSafe(|| function(&action))).unwrap_or(false)
}

/// Actions, given as JSON: true when the application has done it.
pub fn hook_action<F>(document: Option<&Document>, function: F) -> Result<Hook, Error>
where
    F: Fn(&str) -> bool + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_action(Document::raw_or_null(document), action_trampoline::<F>, boxed(function), Some(destroy_boxed::<F>))
    })
}

unsafe extern "C" fn input_trampoline<F>(
    _pdf: *mut sys::zebkit_pdf,
    tool: *const c_char,
    points: *const c_char,
    data: *mut c_void,
) -> *mut c_char
where
    F: Fn(&str, &str) -> Option<String> + 'static,
{
    let function = &*data.cast::<F>();
    let tool = string_from(tool).unwrap_or_default();
    let points = string_from(points).unwrap_or_default();
    match catch_unwind(AssertUnwindSafe(|| function(&tool, &points))) {
        Ok(Some(json)) => give_string(&json),
        _ => std::ptr::null_mut(),
    }
}

/// A filter of `tool`'s points: JSON in, new points or `None` for no change.
pub fn hook_input<F>(document: Option<&Document>, tool: &str, function: F) -> Result<Hook, Error>
where
    F: Fn(&str, &str) -> Option<String> + 'static,
{
    hook_result(unsafe {
        sys::zebkit_pdf_hook_input(
            Document::raw_or_null(document),
            c_string(tool).as_ptr(),
            input_trampoline::<F>,
            boxed(function),
            Some(destroy_boxed::<F>),
        )
    })
}

/// The built-in filters with no document: JSON in and out.
pub fn filter_input(points: &str, options: Option<&str>) -> Result<String, Error> {
    let mut error = std::ptr::null_mut();
    let options = options.map(c_string);
    let answer = unsafe {
        sys::zebkit_pdf_filter_input(
            std::ptr::null_mut(),
            std::ptr::null(),
            c_string(points).as_ptr(),
            options.as_ref().map_or(std::ptr::null(), |o| o.as_ptr()),
            &mut error,
        )
    };
    if answer.is_null() {
        return Err(unsafe { take_error(error, "the points cannot be filtered") });
    }
    Ok(unsafe { take_string(answer) }.unwrap_or_default())
}
