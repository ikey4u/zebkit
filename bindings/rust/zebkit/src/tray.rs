//! The application's icon in the system tray or menu bar (`<zebkit/zebkit.h>`):
//! a PNG image, a tooltip, a menu of `app::set_menu`'s items and a click.
//! Engine thread only.

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr};
use std::rc::Rc;

use crate::engine::require_engine_thread;
use crate::{c_string, sys, take_error, Error};

type MenuFunction = RefCell<Option<Box<dyn FnMut(&str)>>>;

#[derive(Default)]
struct Functions {
    menu: MenuFunction,
    click: RefCell<Option<Box<dyn FnMut()>>>,
}

/// A tray icon; dropping it (or [`Tray::close`]) removes it.
pub struct Tray {
    raw: *mut sys::zebkit_tray,
    // Boxed in an Rc: the engine has its address.
    functions: Rc<Functions>,
}

unsafe extern "C" fn on_menu(id: *const c_char, data: *mut c_void) {
    let functions = &*data.cast_const().cast::<Functions>();
    let id = CStr::from_ptr(id).to_string_lossy();
    if let Ok(mut slot) = functions.menu.try_borrow_mut() {
        if let Some(function) = slot.as_mut() {
            function(&id);
        }
    }
}

unsafe extern "C" fn on_click(data: *mut c_void) {
    let functions = &*data.cast_const().cast::<Functions>();
    if let Ok(mut slot) = functions.click.try_borrow_mut() {
        if let Some(function) = slot.as_mut() {
            function();
        }
    }
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
        *self.functions.menu.borrow_mut() = Some(Box::new(function));
        let mut error = std::ptr::null_mut();
        if unsafe { sys::zebkit_tray_set_menu(self.raw, c_string(json).as_ptr(), Some(on_menu), self.data(), &mut error) } {
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
        *self.functions.click.borrow_mut() = Some(Box::new(function));
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
