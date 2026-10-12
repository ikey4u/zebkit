use crate::sys::cocoa;
use std::marker::PhantomData;
use std::os::raw::c_void;
use std::time::Duration;

/// The loop in which the engine works: loading, drawing, input, and every
/// closure registered with this crate run inside [`MainLoop::run`].
///
/// Clones are handles to the same loop, so one can be moved into a closure
/// that ends it. On macOS it is the application's loop, of which a process
/// has one: every `MainLoop` is a handle to it.
pub struct MainLoop {
    _one_thread: PhantomData<*mut ()>,
}

impl MainLoop {
    pub fn new() -> Self {
        Self { _one_thread: PhantomData }
    }

    /// Runs until [`MainLoop::quit`] is called.
    pub fn run(&self) {
        unsafe { cocoa::zebkit_shim_run() }
    }

    /// Makes [`MainLoop::run`] return. Does nothing if it is not running.
    pub fn quit(&self) {
        unsafe { cocoa::zebkit_shim_quit() }
    }
}

impl Default for MainLoop {
    fn default() -> Self {
        Self::new()
    }
}

impl Clone for MainLoop {
    fn clone(&self) -> Self {
        Self::new()
    }
}

/// Calls `function` once, from the main loop, after `delay`.
pub fn timeout(delay: Duration, function: impl FnOnce() + 'static) {
    unsafe extern "C" fn call(data: *mut c_void) {
        let slot = &mut *(data as *mut Option<Box<dyn FnOnce()>>);
        if let Some(function) = slot.take() {
            function();
        }
    }
    unsafe extern "C" fn release(data: *mut c_void) {
        drop(Box::from_raw(data as *mut Option<Box<dyn FnOnce()>>));
    }
    let slot: Box<Option<Box<dyn FnOnce()>>> = Box::new(Some(Box::new(function)));
    let milliseconds = delay.as_millis().min(u32::MAX as u128) as u32;
    unsafe { cocoa::zebkit_shim_timeout(milliseconds, call, Box::into_raw(slot).cast(), Some(release)) }
}
