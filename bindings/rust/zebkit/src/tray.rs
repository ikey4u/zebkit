//! The application's icon in the system tray or menu bar (`<zebkit/zebkit.h>`):
//! a PNG image, a tooltip, a menu of `app::set_menu`'s items and a click.
//! Engine thread only.

use std::ffi::{c_char, c_void};
use std::rc::Rc;

use crate::engine::require_engine_thread;
use crate::{c_string, string_from, sys, take_error, Error, Slot};

// Out of their slots while they run: a menu item's function may set a new menu.
#[derive(Default)]
struct Functions {
    menu: Slot<dyn FnMut(&str)>,
    click: Slot<dyn FnMut()>,
}

/// A tray icon; dropping it (or [`Tray::close`]) removes it.
pub struct Tray {
    raw: *mut sys::zebkit_tray,
    // Boxed in an Rc: the engine has its address.
    functions: Rc<Functions>,
}

/// The functions, held while one runs: it may drop the tray.
unsafe fn functions(data: *mut c_void) -> Rc<Functions> {
    let pointer = data.cast_const().cast::<Functions>();
    Rc::increment_strong_count(pointer);
    Rc::from_raw(pointer)
}

unsafe extern "C" fn on_menu(id: *const c_char, data: *mut c_void) {
    let id = string_from(id).unwrap_or_default();
    functions(data).menu.call(|function| function(&id));
}

unsafe extern "C" fn on_click(data: *mut c_void) {
    functions(data).click.call(|function| function());
}

impl Tray {
    /// A tray icon from PNG bytes, with a tooltip.
    pub fn new(png: &[u8], tooltip: &str) -> Result<Tray, Error> {
        require_engine_thread("Tray::new()");
        let mut error = std::ptr::null_mut();
        let raw = unsafe { sys::zebkit_tray_new(png.as_ptr(), png.len(), c_string(tooltip).as_ptr(), &mut error) };
        if raw.is_null() {
            return Err(unsafe { take_error(error, "the tray icon was refused") });
        }
        Ok(Tray { raw, functions: Rc::new(Functions::default()) })
    }

    fn data(&self) -> *mut c_void {
        Rc::as_ptr(&self.functions).cast_mut().cast()
    }

    /// A new image, PNG bytes.
    pub fn set_icon(&self, png: &[u8]) -> Result<(), Error> {
        if self.raw.is_null() {
            return Err(Error::new("the tray icon is closed"));
        }
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_tray_set_icon(self.raw, png.as_ptr(), png.len(), &mut error) } {
            Ok(())
        } else {
            Err(unsafe { take_error(error, "the image was refused") })
        }
    }

    pub fn set_tooltip(&self, tooltip: &str) {
        if !self.raw.is_null() {
            unsafe { sys::zebkit_tray_set_tooltip(self.raw, c_string(tooltip).as_ptr()) }
        }
    }

    /// Its menu, items as `app::set_menu`'s; `function` gets a chosen item's id.
    pub fn set_menu<F: FnMut(&str) + 'static>(&self, json: &str, function: F) -> Result<(), Error> {
        if self.raw.is_null() {
            return Err(Error::new("the tray icon is closed"));
        }
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_tray_set_menu(self.raw, c_string(json).as_ptr(), Some(on_menu), self.data(), &mut error) } {
            // Only now: a refused menu leaves the old one, with its function.
            self.functions.menu.set(Some(Box::new(function)));
            Ok(())
        } else {
            Err(unsafe { take_error(error, "the menu was refused") })
        }
    }

    /// A click on the icon (on macOS, with a menu, a click opens the menu instead).
    pub fn on_click<F: FnMut() + 'static>(&self, function: F) {
        if self.raw.is_null() {
            return;
        }
        self.functions.click.set(Some(Box::new(function)));
        unsafe { sys::zebkit_tray_on_click(self.raw, Some(on_click), self.data()) }
    }

    /// Removes the icon.
    pub fn close(&mut self) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        if !raw.is_null() {
            unsafe { sys::zebkit_tray_close(raw) }
        }
    }
}

impl Drop for Tray {
    fn drop(&mut self) {
        self.close();
    }
}
