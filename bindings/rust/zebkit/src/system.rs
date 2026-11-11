//! What the system tells of itself and does for the application
//! (`<zebkit/zebkit.h>`): the screens, power and the session's lock, keeping
//! the system awake, opening URLs and folders with the system's handlers,
//! the system's appearance changing, and web notifications. Engine thread
//! only; events are JSON, as the C header gives them.

use std::ffi::{c_char, c_void};

use crate::engine::require_engine_thread;
use crate::{c_string, string_from, sys, take_error, take_string, Error, Slot};

type Event = Slot<dyn FnMut(&str)>;

thread_local! {
    static ON_POWER: Event = const { Slot::new() };
    static ON_DISPLAYS: Event = const { Slot::new() };
    static ON_NOTIFICATION: Event = const { Slot::new() };
    static ON_ENVIRONMENT: Event = const { Slot::new() };
}

// The function is out of its slot while it runs, so that it may replace or remove itself.
fn deliver(slot: &'static std::thread::LocalKey<Event>, json: *const c_char) {
    let json = unsafe { string_from(json) }.unwrap_or_default();
    slot.with(|slot| slot.call(|function| function(&json)));
}

unsafe extern "C" fn on_power(json: *const c_char, _: *mut c_void) {
    deliver(&ON_POWER, json)
}
unsafe extern "C" fn on_displays(json: *const c_char, _: *mut c_void) {
    deliver(&ON_DISPLAYS, json)
}
unsafe extern "C" fn on_notification_call(json: *const c_char, _: *mut c_void) {
    deliver(&ON_NOTIFICATION, json)
}
unsafe extern "C" fn on_environment(json: *const c_char, _: *mut c_void) {
    deliver(&ON_ENVIRONMENT, json)
}

/// Suspend, resume, the power source and the session's lock:
/// `{"event": "suspend" | "resume" | "power-source" | "lock" | "unlock"}`.
/// `None` stops.
pub fn on_power_event<F: FnMut(&str) + 'static>(function: Option<F>) {
    require_engine_thread("system::on_power_event()");
    let set = function.is_some();
    ON_POWER.with(|slot| slot.set(function.map(|f| Box::new(f) as Box<dyn FnMut(&str)>)));
    unsafe { sys::zebkit_on_power_event(set.then_some(on_power as _), std::ptr::null_mut()) }
}

/// The screens, `[{id, bounds, workArea, scale, rotation, primary}]` in DIPs.
pub fn displays() -> Option<String> {
    require_engine_thread("system::displays()");
    unsafe { take_string(sys::zebkit_get_displays()) }
}

/// `{"event": "displays", "displays": [...]}` when the screens change. `None` stops.
pub fn on_displays_changed<F: FnMut(&str) + 'static>(function: Option<F>) {
    require_engine_thread("system::on_displays_changed()");
    let set = function.is_some();
    ON_DISPLAYS.with(|slot| slot.set(function.map(|f| Box::new(f) as Box<dyn FnMut(&str)>)));
    unsafe { sys::zebkit_on_displays_changed(set.then_some(on_displays as _), std::ptr::null_mut()) }
}

/// [`crate::environment`] and [`crate::capabilities`], here beside the
/// functions that tell of their changes.
pub use crate::{capabilities, environment};

/// The system's appearance changing, with what [`crate::environment`] returns. `None` stops.
pub fn on_environment_changed<F: FnMut(&str) + 'static>(function: Option<F>) {
    require_engine_thread("system::on_environment_changed()");
    let set = function.is_some();
    ON_ENVIRONMENT.with(|slot| slot.set(function.map(|f| Box::new(f) as Box<dyn FnMut(&str)>)));
    unsafe { sys::zebkit_on_environment_changed(set.then_some(on_environment as _), std::ptr::null_mut()) }
}

/// Keeps the system from sleeping (and the display on, with
/// `keep_display_on`) until [`allow_sleep`] with the token or the engine's
/// end. `None` when it could not.
pub fn prevent_sleep(reason: &str, keep_display_on: bool) -> Option<u64> {
    require_engine_thread("system::prevent_sleep()");
    let token = unsafe { sys::zebkit_prevent_sleep(keep_display_on, c_string(reason).as_ptr()) };
    (token != 0).then_some(token)
}

/// Ends what [`prevent_sleep`] returned `token` for; false for a token not held.
pub fn allow_sleep(token: u64) -> bool {
    require_engine_thread("system::allow_sleep()");
    unsafe { sys::zebkit_allow_sleep(token) }
}

/// Opens `url` with the system's handler for its scheme, as one argument,
/// never through a shell.
pub fn open_external(url: &str) -> Result<(), Error> {
    require_engine_thread("system::open_external()");
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_open_external(c_string(url).as_ptr(), &mut error) } {
        Ok(())
    } else {
        Err(unsafe { take_error(error, "the URL was not opened") })
    }
}

/// Shows the absolute `path` in the system's file manager.
pub fn show_in_folder(path: &str) -> Result<(), Error> {
    require_engine_thread("system::show_in_folder()");
    let mut error = std::ptr::null_mut();
    if unsafe { sys::zebkit_show_in_folder(c_string(path).as_ptr(), &mut error) } {
        Ok(())
    } else {
        Err(unsafe { take_error(error, "the folder was not shown") })
    }
}

/// The pages' web notifications are the application's to show while this is
/// set: `{"event": "show", "id", "title", "body", ...}` and `{"event":
/// "close", "id"}`; without it the system shows them. `None` stops.
pub fn on_notification<F: FnMut(&str) + 'static>(function: Option<F>) {
    require_engine_thread("system::on_notification()");
    let set = function.is_some();
    ON_NOTIFICATION.with(|slot| slot.set(function.map(|f| Box::new(f) as Box<dyn FnMut(&str)>)));
    unsafe { sys::zebkit_on_notification(set.then_some(on_notification_call as _), std::ptr::null_mut()) }
}

/// The person clicked notification `id` (`action`: one of its actions, or
/// `None` for the notification itself); false for an id not shown.
pub fn notification_clicked(id: &str, action: Option<u32>) -> bool {
    require_engine_thread("system::notification_clicked()");
    let action = action.map_or(-1, |action| action.min(i32::MAX as u32) as i32);
    unsafe { sys::zebkit_notification_clicked(c_string(id).as_ptr(), action) }
}

/// Notification `id` was closed, by the person or not.
pub fn notification_closed(id: &str, by_user: bool) -> bool {
    require_engine_thread("system::notification_closed()");
    unsafe { sys::zebkit_notification_closed(c_string(id).as_ptr(), by_user) }
}
