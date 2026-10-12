use crate::sys::glib;
use crate::sys::{gboolean, gpointer};
use std::marker::PhantomData;
use std::time::Duration;

/// The loop in which the engine works: loading, drawing, input, and every
/// closure registered with this crate run inside [`MainLoop::run`].
///
/// Clones are handles to the same loop, so one can be moved into a closure
/// that ends it.
pub struct MainLoop {
    raw: *mut glib::GMainLoop,
    _one_thread: PhantomData<*mut ()>,
}

impl MainLoop {
    pub fn new() -> Self {
        let raw = unsafe { glib::g_main_loop_new(std::ptr::null_mut(), 0) };
        Self {
            raw,
            _one_thread: PhantomData,
        }
    }

    /// Runs until [`MainLoop::quit`] is called.
    pub fn run(&self) {
        unsafe { glib::g_main_loop_run(self.raw) }
    }

    /// Makes [`MainLoop::run`] return. Does nothing if it is not running.
    pub fn quit(&self) {
        unsafe { glib::g_main_loop_quit(self.raw) }
    }
}

impl Default for MainLoop {
    fn default() -> Self {
        Self::new()
    }
}

impl Clone for MainLoop {
    fn clone(&self) -> Self {
        Self {
            raw: unsafe { glib::g_main_loop_ref(self.raw) },
            _one_thread: PhantomData,
        }
    }
}

impl Drop for MainLoop {
    fn drop(&mut self) {
        unsafe { glib::g_main_loop_unref(self.raw) }
    }
}

/// Calls `function` once, from the main loop, after `delay`.
pub fn timeout(delay: Duration, function: impl FnOnce() + 'static) {
    unsafe extern "C" fn call(data: gpointer) -> gboolean {
        let slot = &mut *(data as *mut Option<Box<dyn FnOnce()>>);
        if let Some(function) = slot.take() {
            function();
        }
        0 // G_SOURCE_REMOVE: once
    }
    unsafe extern "C" fn release(data: gpointer) {
        drop(Box::from_raw(data as *mut Option<Box<dyn FnOnce()>>));
    }
    let slot: Box<Option<Box<dyn FnOnce()>>> = Box::new(Some(Box::new(function)));
    let milliseconds = delay.as_millis().min(u32::MAX as u128) as u32;
    unsafe {
        glib::g_timeout_add_full(
            glib::G_PRIORITY_DEFAULT,
            milliseconds,
            call,
            Box::into_raw(slot) as gpointer,
            Some(release),
        );
    }
}
