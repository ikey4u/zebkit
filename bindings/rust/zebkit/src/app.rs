//! The application as the system sees it: its menu, its icon in the Dock or
//! on the taskbar, files it is asked to open, the Dock's reopen, and quit. On macOS the menu is the menu bar;
//! elsewhere, where windows have no menu bar, its keys work in the
//! application's windows.

use std::cell::RefCell;
use std::ffi::{c_char, c_void, CStr};

use crate::engine::require_engine_thread;
use crate::{c_string, sys, take_error, take_string, Error};

thread_local! {
    static ON_MENU: RefCell<Option<Box<dyn FnMut(&str)>>> = const { RefCell::new(None) };
    static ON_DOCK_MENU: RefCell<Option<Box<dyn FnMut(&str)>>> = const { RefCell::new(None) };
    static ON_OPEN: RefCell<Option<Box<dyn FnMut(&str)>>> = const { RefCell::new(None) };
    static ON_REOPEN: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
    static ON_QUIT: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
}

/// Sets the application's menu from `json` (see `zebkit_app_set_menu()` in
/// `zebkit.h`): `[{"title": "File", "items": [{"title": "Open…", "id":
/// "open", "key": "o"}, {"separator": true}, {"role": "quit"}]}, ...]`.
/// `function` gets an item's `id` when it is chosen. On the engine's thread.
pub fn set_menu<F: FnMut(&str) + 'static>(json: &str, function: F) -> Result<(), Error> {
    require_engine_thread("app::set_menu()");
    ON_MENU.with(|slot| *slot.borrow_mut() = Some(Box::new(function)));
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_app_set_menu(c_string(json).as_ptr(), Some(on_menu), std::ptr::null_mut(), &mut error) } {
        Ok(())
    } else {
        Err(unsafe { take_error(error, "the menu was refused") })
    }
}

/// Changes the menu item `id` without a new menu: `json` is `{"enabled",
/// "checked", "title"}`, any of them; checking one of a `radioGroup`
/// unchecks the others.
pub fn update_menu_item(id: &str, json: &str) -> Result<(), Error> {
    require_engine_thread("app::update_menu_item()");
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_app_update_menu_item(c_string(id).as_ptr(), c_string(json).as_ptr(), &mut error) } {
        Ok(())
    } else {
        Err(unsafe { take_error(error, "the menu item was not changed") })
    }
}

/// The menu with its items' state, as JSON.
pub fn menu() -> Option<String> {
    require_engine_thread("app::menu()");
    unsafe { take_string(sys::zebkit_app_get_menu()) }
}

/// The badge on the application's icon in the Dock or on the taskbar (a
/// number on Linux's launcher docks); `None` or "" for none.
pub fn set_badge(text: Option<&str>) -> bool {
    require_engine_thread("app::set_badge()");
    unsafe { sys::zebkit_app_set_badge(c_string(text.unwrap_or("")).as_ptr()) }
}

/// A progress from 0 to 1 on the application's icon; `None` for none.
pub fn set_progress(progress: Option<f64>) -> bool {
    require_engine_thread("app::set_progress()");
    unsafe { sys::zebkit_app_set_progress(progress.unwrap_or(-1.0)) }
}

/// The Dock's bounce (`critical`: until the application is active) or the
/// taskbar button's flash; urgent on Linux's launcher docks.
pub fn request_attention(critical: bool) {
    require_engine_thread("app::request_attention()");
    unsafe { sys::zebkit_app_request_attention(critical) }
}

/// The Dock's menu (macOS only; an error elsewhere): items as [`set_menu`]'s,
/// `function` gets a chosen item's id.
pub fn set_dock_menu<F: FnMut(&str) + 'static>(json: &str, function: F) -> Result<(), Error> {
    require_engine_thread("app::set_dock_menu()");
    ON_DOCK_MENU.with(|slot| *slot.borrow_mut() = Some(Box::new(function)));
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_app_set_dock_menu(c_string(json).as_ptr(), Some(on_dock_menu), std::ptr::null_mut(), &mut error) } {
        Ok(())
    } else {
        Err(unsafe { take_error(error, "the Dock's menu was refused") })
    }
}

/// Files the system asks the running application to open, as a JSON array
/// of paths (macOS: the Finder, the Dock, and those it was started with,
/// kept until this is set). Elsewhere files come as program arguments.
pub fn on_open_files<F: FnMut(&str) + 'static>(function: F) {
    require_engine_thread("app::on_open_files()");
    ON_OPEN.with(|slot| *slot.borrow_mut() = Some(Box::new(function)));
    unsafe { sys::zebkit_app_on_open_files(Some(on_open), std::ptr::null_mut()) }
}

/// The application's icon in the Dock was clicked while no window was shown.
pub fn on_reopen<F: FnMut() + 'static>(function: F) {
    require_engine_thread("app::on_reopen()");
    ON_REOPEN.with(|slot| *slot.borrow_mut() = Some(Box::new(function)));
    unsafe { sys::zebkit_app_on_reopen(Some(on_reopen_call), std::ptr::null_mut()) }
}

/// The user asked the application to quit (⌘Q, the Dock, logging out):
/// `function` decides, calling [`crate::quit`] or not. Without it zebkit
/// quits.
pub fn on_quit_request<F: FnMut() + 'static>(function: F) {
    require_engine_thread("app::on_quit_request()");
    ON_QUIT.with(|slot| *slot.borrow_mut() = Some(Box::new(function)));
    unsafe { sys::zebkit_app_on_quit_request(Some(on_quit), std::ptr::null_mut()) }
}

unsafe extern "C" fn on_menu(id: *const c_char, _data: *mut c_void) {
    let id = CStr::from_ptr(id).to_string_lossy();
    ON_MENU.with(|slot| {
        if let Ok(mut slot) = slot.try_borrow_mut() {
            if let Some(function) = slot.as_mut() {
                function(&id);
            }
        }
    });
}

unsafe extern "C" fn on_dock_menu(id: *const c_char, _data: *mut c_void) {
    let id = CStr::from_ptr(id).to_string_lossy();
    ON_DOCK_MENU.with(|slot| {
        if let Ok(mut slot) = slot.try_borrow_mut() {
            if let Some(function) = slot.as_mut() {
                function(&id);
            }
        }
    });
}

unsafe extern "C" fn on_open(paths: *const c_char, _data: *mut c_void) {
    let paths = CStr::from_ptr(paths).to_string_lossy();
    ON_OPEN.with(|slot| {
        if let Ok(mut slot) = slot.try_borrow_mut() {
            if let Some(function) = slot.as_mut() {
                function(&paths);
            }
        }
    });
}

unsafe extern "C" fn on_reopen_call(_data: *mut c_void) {
    ON_REOPEN.with(|slot| {
        if let Ok(mut slot) = slot.try_borrow_mut() {
            if let Some(function) = slot.as_mut() {
                function();
            }
        }
    });
}

unsafe extern "C" fn on_quit(_data: *mut c_void) {
    ON_QUIT.with(|slot| {
        if let Ok(mut slot) = slot.try_borrow_mut() {
            if let Some(function) = slot.as_mut() {
                function();
            }
        }
    });
}
