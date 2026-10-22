//! JavaScript outside any page.

use crate::engine::require_engine_thread;
use crate::{c_string, script_result, sys, Error};
use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::path::Path;

/// A JavaScript context of the application's own, on the engine's thread:
/// V8 with all of `zebkit.*`, with no grants needed (it is the
/// application's code, as an Electron application's main process is),
/// and `zebkit.View` to make views and answer their pages' calls.
/// `docs/API.md` in the repository is the script's reference.
///
/// Dropping it ends the script: its timers stop; its views stay.
pub struct Script {
    raw: *mut sys::zebkit_script,
    /// The closure for [`Script::on_idle`], boxed so that the address the
    /// engine holds stays where it is.
    idle: Box<IdleSlot>,
}

type IdleSlot = RefCell<Option<Box<dyn FnOnce()>>>;

impl Script {
    /// A script that sees `args` as `zebkit.args`.
    pub fn new(args: &[&str]) -> Script {
        require_engine_thread("Script::new()");
        let args: Vec<CString> = args.iter().map(|arg| c_string(arg)).collect();
        let argv: Vec<*const c_char> = args.iter().map(|arg| arg.as_ptr()).collect();
        let raw = unsafe { sys::zebkit_script_new(argv.len() as c_int, argv.as_ptr()) };
        Script { raw, idle: Box::new(RefCell::new(None)) }
    }

    /// Runs the ES module in the file at `path`, with the modules it
    /// imports relative to it, and calls `done` when its evaluation ends
    /// (for a top-level await, when the promise settles).
    pub fn run_module<F: FnOnce(Result<(), Error>) + 'static>(&self, path: &Path, done: F) {
        let done = move |result: Result<String, Error>| done(result.map(|_| ()));
        let trampoline = result_trampoline(&done);
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_script_run_module(self.raw, c_string(&path.to_string_lossy()).as_ptr(), Some(trampoline), data.cast()) }
    }

    /// Evaluates `source` as a classic script; `done` gets its value as
    /// JSON, or what it threw.
    pub fn evaluate<F: FnOnce(Result<String, Error>) + 'static>(&self, source: &str, done: F) {
        let data = Box::into_raw(Box::new(done));
        unsafe { sys::zebkit_script_evaluate(self.raw, c_string(source).as_ptr(), Some(on_result::<F>), data.cast()) }
    }

    /// Calls `function` once nothing of the script is pending: no timer, no
    /// call in progress, no view open, and its module evaluated. `zebkit-js`
    /// quits then, as Node exits.
    pub fn on_idle<F: FnOnce() + 'static>(&self, function: F) {
        *self.idle.borrow_mut() = Some(Box::new(function));
        let data = (&*self.idle as *const IdleSlot).cast_mut().cast::<c_void>();
        unsafe { sys::zebkit_script_on_idle(self.raw, Some(on_idle), data) }
    }
}

impl Drop for Script {
    fn drop(&mut self) {
        // Before the closures it could call go.
        unsafe { sys::zebkit_script_destroy(self.raw) }
    }
}

unsafe extern "C" fn on_result<F: FnOnce(Result<String, Error>)>(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    done(script_result(json, error));
}

fn result_trampoline<F: FnOnce(Result<String, Error>)>(_done: &F) -> sys::zebkit_result_func {
    on_result::<F>
}

unsafe extern "C" fn on_idle(data: *mut c_void) {
    let slot = &*data.cast::<IdleSlot>();
    let function = slot.borrow_mut().take();
    if let Some(function) = function {
        function();
    }
}
