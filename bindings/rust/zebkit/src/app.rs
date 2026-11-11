//! The application as the system sees it: its menu, its icon in the Dock or
//! on the taskbar, files it is asked to open, the Dock's reopen, and quit. On macOS the menu is the menu bar;
//! elsewhere, where windows have no menu bar, its keys work in the
//! application's windows.

use std::ffi::{c_char, c_void};

use crate::engine::require_engine_thread;
use crate::{c_string, string_from, sys, take_error, take_string, Error, Slot};

// Out of its slot while it runs: a menu item's function may set a new menu.
thread_local! {
    static ON_MENU: Slot<dyn FnMut(&str)> = const { Slot::new() };
    static ON_DOCK_MENU: Slot<dyn FnMut(&str)> = const { Slot::new() };
    static ON_OPEN: Slot<dyn FnMut(&str)> = const { Slot::new() };
    static ON_REOPEN: Slot<dyn FnMut()> = const { Slot::new() };
    static ON_QUIT: Slot<dyn FnMut()> = const { Slot::new() };
}

/// Sets the application's menu from `json` (see `zebkit_app_set_menu()` in
/// `zebkit.h`): `[{"title": "File", "items": [{"title": "Open…", "id":
/// "open", "key": "o"}, {"separator": true}, {"role": "quit"}]}, ...]`.
/// `function` gets an item's `id` when it is chosen. On the engine's thread.
pub fn set_menu<F: FnMut(&str) + 'static>(json: &str, function: F) -> Result<(), Error> {
    require_engine_thread("app::set_menu()");
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_app_set_menu(c_string(json).as_ptr(), Some(on_menu), std::ptr::null_mut(), &mut error) } {
        // Only now: a refused menu leaves the old one, with its function.
        ON_MENU.with(|slot| slot.set(Some(Box::new(function))));
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
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_app_set_dock_menu(c_string(json).as_ptr(), Some(on_dock_menu), std::ptr::null_mut(), &mut error) } {
        ON_DOCK_MENU.with(|slot| slot.set(Some(Box::new(function))));
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
    ON_OPEN.with(|slot| slot.set(Some(Box::new(function))));
    unsafe { sys::zebkit_app_on_open_files(Some(on_open), std::ptr::null_mut()) }
}

/// Stops [`on_open_files`]; files then wait for the next function.
pub fn clear_open_files() {
    require_engine_thread("app::clear_open_files()");
    unsafe { sys::zebkit_app_on_open_files(None, std::ptr::null_mut()) }
    ON_OPEN.with(|slot| slot.set(None));
}

/// The application's icon in the Dock was clicked while no window was shown.
pub fn on_reopen<F: FnMut() + 'static>(function: F) {
    require_engine_thread("app::on_reopen()");
    ON_REOPEN.with(|slot| slot.set(Some(Box::new(function))));
    unsafe { sys::zebkit_app_on_reopen(Some(on_reopen_call), std::ptr::null_mut()) }
}

/// Stops [`on_reopen`].
pub fn clear_reopen() {
    require_engine_thread("app::clear_reopen()");
    unsafe { sys::zebkit_app_on_reopen(None, std::ptr::null_mut()) }
    ON_REOPEN.with(|slot| slot.set(None));
}

/// The user asked the application to quit (⌘Q, the Dock, logging out):
/// `function` decides, calling [`crate::quit`] or not. Without it zebkit
/// quits.
pub fn on_quit_request<F: FnMut() + 'static>(function: F) {
    require_engine_thread("app::on_quit_request()");
    ON_QUIT.with(|slot| slot.set(Some(Box::new(function))));
    unsafe { sys::zebkit_app_on_quit_request(Some(on_quit), std::ptr::null_mut()) }
}

/// Stops [`on_quit_request`]: zebkit quits when asked to.
pub fn clear_quit_request() {
    require_engine_thread("app::clear_quit_request()");
    unsafe { sys::zebkit_app_on_quit_request(None, std::ptr::null_mut()) }
    ON_QUIT.with(|slot| slot.set(None));
}

unsafe extern "C" fn on_menu(id: *const c_char, _data: *mut c_void) {
    let id = string_from(id).unwrap_or_default();
    ON_MENU.with(|slot| slot.call(|function| function(&id)));
}

unsafe extern "C" fn on_dock_menu(id: *const c_char, _data: *mut c_void) {
    let id = string_from(id).unwrap_or_default();
    ON_DOCK_MENU.with(|slot| slot.call(|function| function(&id)));
}

unsafe extern "C" fn on_open(paths: *const c_char, _data: *mut c_void) {
    let paths = string_from(paths).unwrap_or_else(|| "[]".to_string());
    ON_OPEN.with(|slot| slot.call(|function| function(&paths)));
}

unsafe extern "C" fn on_reopen_call(_data: *mut c_void) {
    ON_REOPEN.with(|slot| slot.call(|function| function()));
}

unsafe extern "C" fn on_quit(_data: *mut c_void) {
    ON_QUIT.with(|slot| slot.call(|function| function()));
}
