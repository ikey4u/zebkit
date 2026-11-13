//! 2D drawing on the engine's Skia, `<zebkit/canvas.h>`, and shaping a line of
//! text on its HarfBuzz, `<zebkit/text.h>`. None of it needs [`crate::run`].

use crate::{c_string, sys, take_bytes, take_error, take_string, Bytes, Error};
use std::os::raw::c_int;

/// A font from its bytes, for [`Canvas::set_font`].
pub struct Font {
    raw: *mut sys::zebkit_canvas_font,
}

unsafe impl Send for Font {}

impl Font {
    pub fn new(data: &[u8]) -> Result<Font, Error> {
        let mut error = std::ptr::null_mut();
        let raw = unsafe { sys::zebkit_canvas_font_new(data.as_ptr().cast(), data.len(), &mut error) };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "not a font") });
        }
        Ok(Font { raw })
    }
}

impl Drop for Font {
    fn drop(&mut self) {
        unsafe { sys::zebkit_canvas_font_close(self.raw) }
    }
}

/// A canvas over a bitmap of its own, or one a PDF hook lends.
pub struct Canvas {
    raw: *mut sys::zebkit_canvas,
    owned: bool,
}

unsafe impl Send for Canvas {}

impl Canvas {
    /// A transparent canvas of `width` by `height` pixels.
    pub fn new(width: u32, height: u32) -> Result<Canvas, Error> {
        let mut error = std::ptr::null_mut();
        let raw = unsafe {
            sys::zebkit_canvas_new(c_int::try_from(width).unwrap_or(-1), c_int::try_from(height).unwrap_or(-1), &mut error)
        };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "the canvas cannot be made") });
        }
        Ok(Canvas { raw, owned: true })
    }

    /// Draws on `rgba` (`width * 4` bytes a row, opaque or premultiplied)
    /// inside `draw`, and then it is the caller's again.
    pub fn on_pixels<R>(rgba: &mut [u8], width: u32, height: u32, draw: impl FnOnce(&mut Canvas) -> R) -> Result<R, Error> {
        if rgba.len() != width as usize * height as usize * 4 {
            return Err(Error::new("the pixels are not width by height RGBA"));
        }
        let mut error = std::ptr::null_mut();
        let raw = unsafe {
            sys::zebkit_canvas_wrap(rgba.as_mut_ptr(), width as c_int, height as c_int, 0, 0, &mut error)
        };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "the canvas cannot be made") });
        }
        let mut canvas = Canvas { raw, owned: true };
        let answer = draw(&mut canvas);
        // Swapped out with `mem::swap`, it would outlive the pixels it writes to.
        if !canvas.is(raw) {
            std::process::abort();
        }
        Ok(answer)
    }

    pub(crate) fn is(&self, raw: *mut sys::zebkit_canvas) -> bool {
        self.raw == raw
    }

    pub(crate) fn borrowed(raw: *mut sys::zebkit_canvas) -> Canvas {
        Canvas { raw, owned: false }
    }

    pub fn width(&self) -> u32 {
        unsafe { sys::zebkit_canvas_width(self.raw) }.max(0) as u32
    }

    pub fn height(&self) -> u32 {
        unsafe { sys::zebkit_canvas_height(self.raw) }.max(0) as u32
    }

    /// The pixels, RGBA not premultiplied.
    pub fn pixels(&self) -> Result<Bytes, Error> {
        let mut error = std::ptr::null_mut();
        unsafe {
            let data = sys::zebkit_canvas_pixels(self.raw, &mut error);
            if data.is_null() {
                return Err(take_error(error, "this canvas has no pixels"));
            }
            Ok(take_bytes(data, self.width() as usize * self.height() as usize * 4))
        }
    }

    /// The pixels as a PNG file.
    pub fn png(&self) -> Result<Bytes, Error> {
        let (mut size, mut error) = (0, std::ptr::null_mut());
        unsafe {
            let data = sys::zebkit_canvas_png(self.raw, &mut size, &mut error);
            if data.is_null() {
                return Err(take_error(error, "the PNG cannot be made"));
            }
            Ok(take_bytes(data, size))
        }
    }

    pub fn save(&mut self) {
        unsafe { sys::zebkit_canvas_save(self.raw) }
    }
    pub fn restore(&mut self) {
        unsafe { sys::zebkit_canvas_restore(self.raw) }
    }
    pub fn translate(&mut self, x: f64, y: f64) {
        unsafe { sys::zebkit_canvas_translate(self.raw, x, y) }
    }
    pub fn scale(&mut self, x: f64, y: f64) {
        unsafe { sys::zebkit_canvas_scale(self.raw, x, y) }
    }
    pub fn rotate(&mut self, degrees: f64) {
        unsafe { sys::zebkit_canvas_rotate(self.raw, degrees) }
    }
    pub fn transform(&mut self, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64) {
        unsafe { sys::zebkit_canvas_transform(self.raw, a, b, c, d, e, f) }
    }
    pub fn set_matrix(&mut self, a: f64, b: f64, c: f64, d: f64, e: f64, f: f64) {
        unsafe { sys::zebkit_canvas_set_matrix(self.raw, a, b, c, d, e, f) }
    }
    /// 0xRRGGBBAA.
    pub fn set_fill_color(&mut self, rgba: u32) {
        unsafe { sys::zebkit_canvas_set_fill_color(self.raw, rgba) }
    }
    pub fn set_stroke_color(&mut self, rgba: u32) {
        unsafe { sys::zebkit_canvas_set_stroke_color(self.raw, rgba) }
    }
    pub fn set_alpha(&mut self, alpha: f64) {
        unsafe { sys::zebkit_canvas_set_alpha(self.raw, alpha) }
    }
    /// PDF's name of a blend mode. False for another.
    pub fn set_blend(&mut self, mode: &str) -> bool {
        unsafe { sys::zebkit_canvas_set_blend(self.raw, c_string(mode).as_ptr()) }
    }
    pub fn set_line_width(&mut self, width: f64) {
        unsafe { sys::zebkit_canvas_set_line_width(self.raw, width) }
    }
    /// 0 butt, 1 round, 2 square.
    pub fn set_line_cap(&mut self, cap: i32) {
        unsafe { sys::zebkit_canvas_set_line_cap(self.raw, cap) }
    }
    /// 0 miter, 1 round, 2 bevel.
    pub fn set_line_join(&mut self, join: i32) {
        unsafe { sys::zebkit_canvas_set_line_join(self.raw, join) }
    }
    pub fn set_miter_limit(&mut self, limit: f64) {
        unsafe { sys::zebkit_canvas_set_miter_limit(self.raw, limit) }
    }
    pub fn set_dash(&mut self, lengths: &[f64], phase: f64) {
        unsafe { sys::zebkit_canvas_set_dash(self.raw, lengths.as_ptr(), lengths.len() as c_int, phase) }
    }
    /// False when the stops are not 2 to 256, going up from 0 to 1.
    pub fn set_linear_gradient(&mut self, from: (f64, f64), to: (f64, f64), offsets: &[f64], colors: &[u32]) -> bool {
        offsets.len() == colors.len()
            && unsafe {
                sys::zebkit_canvas_set_linear_gradient(self.raw, from.0, from.1, to.0, to.1, offsets.as_ptr(), colors.as_ptr(), offsets.len() as c_int)
            }
    }
    pub fn set_radial_gradient(&mut self, center: (f64, f64), radius: f64, offsets: &[f64], colors: &[u32]) -> bool {
        offsets.len() == colors.len()
            && unsafe {
                sys::zebkit_canvas_set_radial_gradient(self.raw, center.0, center.1, radius, offsets.as_ptr(), colors.as_ptr(), offsets.len() as c_int)
            }
    }
    pub fn begin_path(&mut self) {
        unsafe { sys::zebkit_canvas_begin_path(self.raw) }
    }
    pub fn move_to(&mut self, x: f64, y: f64) {
        unsafe { sys::zebkit_canvas_move_to(self.raw, x, y) }
    }
    pub fn line_to(&mut self, x: f64, y: f64) {
        unsafe { sys::zebkit_canvas_line_to(self.raw, x, y) }
    }
    pub fn quad_to(&mut self, cx: f64, cy: f64, x: f64, y: f64) {
        unsafe { sys::zebkit_canvas_quad_to(self.raw, cx, cy, x, y) }
    }
    pub fn curve_to(&mut self, c1x: f64, c1y: f64, c2x: f64, c2y: f64, x: f64, y: f64) {
        unsafe { sys::zebkit_canvas_curve_to(self.raw, c1x, c1y, c2x, c2y, x, y) }
    }
    pub fn arc(&mut self, x: f64, y: f64, radius: f64, start: f64, end: f64, counterclockwise: bool) {
        unsafe { sys::zebkit_canvas_arc(self.raw, x, y, radius, start, end, counterclockwise) }
    }
    pub fn rect(&mut self, x: f64, y: f64, width: f64, height: f64) {
        unsafe { sys::zebkit_canvas_rect(self.raw, x, y, width, height) }
    }
    pub fn close_path(&mut self) {
        unsafe { sys::zebkit_canvas_close_path(self.raw) }
    }
    pub fn fill(&mut self, even_odd: bool) {
        unsafe { sys::zebkit_canvas_fill(self.raw, even_odd) }
    }
    pub fn stroke(&mut self) {
        unsafe { sys::zebkit_canvas_stroke(self.raw) }
    }
    pub fn clip(&mut self, even_odd: bool) {
        unsafe { sys::zebkit_canvas_clip(self.raw, even_odd) }
    }
    pub fn clear(&mut self, rgba: u32) {
        unsafe { sys::zebkit_canvas_clear(self.raw, rgba) }
    }
    /// RGBA pixels, not premultiplied, over a rectangle. False for what is not `width` by `height`.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_image(&mut self, rgba: &[u8], width: u32, height: u32, x: f64, y: f64, dest_width: f64, dest_height: f64) -> bool {
        rgba.len() == width as usize * height as usize * 4
            && unsafe {
                sys::zebkit_canvas_draw_image(self.raw, rgba.as_ptr(), width as c_int, height as c_int, x, y, dest_width, dest_height)
            }
    }
    pub fn set_font(&mut self, font: &Font, size: f64) {
        unsafe { sys::zebkit_canvas_set_font(self.raw, font.raw, size) }
    }
    pub fn fill_text(&mut self, text: &str, x: f64, y: f64) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_canvas_fill_text(self.raw, c_string(text).as_ptr(), x, y, &mut error) } {
            Ok(())
        } else {
            Err(unsafe { take_error(error, "the text cannot be drawn") })
        }
    }
    pub fn text_path(&mut self, text: &str, x: f64, y: f64) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_canvas_text_path(self.raw, c_string(text).as_ptr(), x, y, &mut error) } {
            Ok(())
        } else {
            Err(unsafe { take_error(error, "the text cannot be drawn") })
        }
    }
    /// The advance of `text` in the canvas's font; `None` without one.
    pub fn measure_text(&mut self, text: &str) -> Option<f64> {
        let width = unsafe { sys::zebkit_canvas_measure_text(self.raw, c_string(text).as_ptr()) };
        (width >= 0.0).then_some(width)
    }
    /// Runs a JSON list of operations, `<zebkit/canvas.h>` says which.
    pub fn run(&mut self, ops: &str) -> Result<(), Error> {
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_canvas_run(self.raw, c_string(ops).as_ptr(), &mut error) } {
            Ok(())
        } else {
            Err(unsafe { take_error(error, "the operations cannot be run") })
        }
    }
}

impl Drop for Canvas {
    fn drop(&mut self) {
        if self.owned {
            unsafe { sys::zebkit_canvas_close(self.raw) }
        }
    }
}

/// Shapes a line of text as `<zebkit/text.h>` says, the answer as JSON:
/// `language` a BCP 47 tag, `direction` "ltr", "rtl", "ttb" or "btt"; `None` to
/// tell from the text.
pub fn shape_text(font: &[u8], size: f64, text: &str, language: Option<&str>, direction: Option<&str>) -> Result<String, Error> {
    let language = language.map(c_string);
    let direction = direction.map(c_string);
    let mut error = std::ptr::null_mut();
    let answer = unsafe {
        sys::zebkit_text_shape(
            font.as_ptr().cast(),
            font.len(),
            size,
            c_string(text).as_ptr(),
            language.as_ref().map_or(std::ptr::null(), |l| l.as_ptr()),
            direction.as_ref().map_or(std::ptr::null(), |d| d.as_ptr()),
            &mut error,
        )
    };
    if answer.is_null() {
        return Err(unsafe { take_error(error, "the text cannot be shaped") });
    }
    Ok(unsafe { take_string(answer) }.unwrap_or_default())
}
