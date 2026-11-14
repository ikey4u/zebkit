//! JavaScript libraries as normal libraries (`<zebkit/library.h>`,
//! `docs/JS_LIBRARIES.md`): a bundled ES module loaded once, in a V8
//! isolate on a thread of its own, and called as any library is.
//!
//! ```no_run
//! use zebkit::js::Library;
//! use zebkit::json::Value;
//!
//! # async fn render() -> Result<(), Box<dyn std::error::Error>> {
//! let charts = Library::load("resources/libs/echarts", &Value::Null)?;
//! let option = Value::parse(r#"{"width": 400, "height": 300, "option": {}}"#)?;
//! let svg = charts.call("renderSvg", Value::Array(vec![option])).await?.value; // a JSON string
//! # Ok(()) }
//! ```
//!
//! Values are JSON and parts: `{"$part": n}` in the JSON stands for
//! `parts[n]`, a `Uint8Array` in the library. [`Library::load`] and
//! [`Library::close`] are for the engine's thread; calls and cancels for
//! any thread. A [`Call`] is a `Future`, woken from the engine's thread, or
//! told to a closure with [`Call::then`].
//!
//! The library calls the application's functions, given at load as a
//! [`Host`], with `await host.call(name, ...args)` (`import host from
//! 'zebkit:host'`):
//!
//! ```no_run
//! use zebkit::js::{Host, Library, Reply};
//! use zebkit::json::Value;
//!
//! # fn load() -> Result<(), zebkit::Error> {
//! let host = Host::new()
//!     .function("progress", |args: Reply| { println!("{}", args.value[0]); Ok(Value::Null.into()) })
//!     .function_async("ask", |args: Reply| async move { Ok(Reply::from(args.value[0].clone())) });
//! let lib = Library::load_with_host("resources/libs/report", &Value::Null, host)?;
//! # Ok(()) }
//! ```

use crate::engine::{on_engine_thread, require_engine_thread};
use crate::json::Value;
use crate::{c_string, sys, take_error, Error};
use std::ffi::CStr;
use std::fmt;
use std::future::Future;
use std::os::raw::{c_char, c_void};
use std::path::Path;
use std::pin::Pin;
use std::ptr;
use std::sync::{Arc, Mutex, RwLock};
use std::task::{Context, Poll, Waker};
use std::time::Duration;

/// How a call or the load ended, when not well.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The function threw, or its promise was rejected.
    Thrown,
    /// The library did not load.
    LoadFailed,
    /// The library exports no function of that name.
    NoFunction,
    /// Not a JSON array, a bad `{"$part": n}`, too deep or too large.
    BadArguments,
    /// A value JSON and parts cannot carry, or too large.
    BadResult,
    /// Past the call's deadline.
    Timeout,
    /// [`Call::cancel`].
    Cancelled,
    /// As many calls wait already as the load's `"queue"`.
    QueueFull,
    /// The library's heap passed `"heapLimitMB"`: it has ended.
    OutOfMemory,
    /// The library ended, or was closed, first.
    Gone,
}

impl Code {
    fn from_raw(code: sys::zebkit_library_code) -> Code {
        match code {
            sys::ZEBKIT_LIBRARY_THROWN => Code::Thrown,
            sys::ZEBKIT_LIBRARY_LOAD_FAILED => Code::LoadFailed,
            sys::ZEBKIT_LIBRARY_NO_FUNCTION => Code::NoFunction,
            sys::ZEBKIT_LIBRARY_BAD_ARGUMENTS => Code::BadArguments,
            sys::ZEBKIT_LIBRARY_BAD_RESULT => Code::BadResult,
            sys::ZEBKIT_LIBRARY_TIMEOUT => Code::Timeout,
            sys::ZEBKIT_LIBRARY_CANCELLED => Code::Cancelled,
            sys::ZEBKIT_LIBRARY_QUEUE_FULL => Code::QueueFull,
            sys::ZEBKIT_LIBRARY_OUT_OF_MEMORY => Code::OutOfMemory,
            _ => Code::Gone,
        }
    }

    /// The C API's name: `"THROWN"`, `"TIMEOUT"`, ...
    pub fn name(self) -> &'static str {
        match self {
            Code::Thrown => "THROWN",
            Code::LoadFailed => "LOAD_FAILED",
            Code::NoFunction => "NO_FUNCTION",
            Code::BadArguments => "BAD_ARGUMENTS",
            Code::BadResult => "BAD_RESULT",
            Code::Timeout => "TIMEOUT",
            Code::Cancelled => "CANCELLED",
            Code::QueueFull => "QUEUE_FULL",
            Code::OutOfMemory => "OUT_OF_MEMORY",
            Code::Gone => "GONE",
        }
    }
}

/// Why a call or the load failed: the code, and what the library threw
/// (its `name`, `message` and `stack`) or what the engine says.
#[derive(Debug, Clone, PartialEq)]
pub struct LibraryError {
    pub code: Code,
    pub message: String,
    pub name: String,
    pub stack: String,
}

impl fmt::Display for LibraryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.name.is_empty() {
            write!(formatter, "{}: {}", self.code.name(), self.message)
        } else {
            write!(formatter, "{}: {}: {}", self.code.name(), self.name, self.message)
        }
    }
}

impl std::error::Error for LibraryError {}

/// What a call gave: its value, with `{"$part": n}` for `parts[n]`. Also
/// what a host function gets (the arguments, a JSON array) and answers.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Reply {
    pub value: Value,
    pub parts: Vec<Vec<u8>>,
}

impl From<Value> for Reply {
    fn from(value: Value) -> Reply {
        Reply { value, parts: Vec::new() }
    }
}

/// A host function's outcome: the answer, or an error message the library's
/// `host.call()` is rejected with (an `Error`).
pub type HostResult = Result<Reply, String>;
type HostFuture = Pin<Box<dyn Future<Output = HostResult> + Send>>;

enum HostFunction {
    Now(Box<dyn Fn(Reply) -> HostResult + Send + Sync>),
    Later(Box<dyn Fn(Reply) -> HostFuture + Send + Sync>),
}

/// The application's functions a library may call, by name: what
/// `host.call(name, ...args)` in the library reaches. Each runs on the
/// engine's thread with the arguments (a JSON array, and parts); an async
/// one's future is polled there, and may be woken from any thread.
#[derive(Default)]
pub struct Host {
    functions: Vec<(String, HostFunction)>,
}

impl Host {
    pub fn new() -> Host {
        Host::default()
    }

    /// A function answered with its value as it returns.
    pub fn function<F: Fn(Reply) -> HostResult + Send + Sync + 'static>(mut self, name: &str, function: F) -> Host {
        self.functions.push((name.to_string(), HostFunction::Now(Box::new(function))));
        self
    }

    /// A function answered when its future is ready.
    pub fn function_async<F, R>(mut self, name: &str, function: F) -> Host
    where
        F: Fn(Reply) -> R + Send + Sync + 'static,
        R: Future<Output = HostResult> + Send + 'static,
    {
        self.functions.push((name.to_string(), HostFunction::Later(Box::new(move |args| Box::pin(function(args))))));
        self
    }
}

/// A library's host functions, the engine's `data` for them until close.
struct HostState {
    handle: Arc<Handle>,
    functions: Vec<(String, HostFunction)>,
}

fn answer(handle: &Handle, request: u64, outcome: HostResult) {
    let raw = handle.raw.read().unwrap_or_else(|poisoned| poisoned.into_inner());
    if raw.is_null() {
        return;
    }
    let (code, json, parts) = match outcome {
        Ok(reply) => (sys::ZEBKIT_LIBRARY_OK, reply.value.to_json(), reply.parts),
        Err(message) => {
            let error = Value::object([("message", Value::from(message)), ("name", Value::from("Error"))]);
            (sys::ZEBKIT_LIBRARY_THROWN, error.to_json(), Vec::new())
        }
    };
    let json = c_string(&json);
    let list: Vec<sys::zebkit_part> = parts.iter().map(|part| sys::zebkit_part { data: part.as_ptr(), size: part.len() }).collect();
    unsafe {
        sys::zebkit_library_answer(*raw, request, code, json.as_ptr(), if list.is_empty() { ptr::null() } else { list.as_ptr() }, list.len());
    }
}

/// An async host function's future, polled on the engine's thread.
struct HostTask {
    future: Mutex<Option<HostFuture>>,
    handle: Arc<Handle>,
    request: u64,
}

impl HostTask {
    fn poll(self: &Arc<Self>) {
        let waker = Waker::from(self.clone());
        let mut slot = self.future.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        let Some(future) = slot.as_mut() else { return };
        let outcome = match crate::guard(|| future.as_mut().poll(&mut Context::from_waker(&waker))) {
            Some(Poll::Pending) => return,
            Some(Poll::Ready(outcome)) => outcome,
            None => Err("the application's function panicked".into()),
        };
        *slot = None;
        drop(slot);
        answer(&self.handle, self.request, outcome);
    }
}

impl std::task::Wake for HostTask {
    fn wake(self: Arc<Self>) {
        crate::post(move || self.poll());
    }
}

unsafe extern "C" fn host_called(
    _library: *mut sys::zebkit_library,
    request: u64,
    name: *const c_char,
    args_json: *const c_char,
    parts: *const sys::zebkit_part,
    part_count: usize,
    data: *mut c_void,
) {
    let state = &*data.cast::<HostState>();
    let name = if name.is_null() { String::new() } else { CStr::from_ptr(name).to_string_lossy().into_owned() };
    let text = if args_json.is_null() { "[]".into() } else { CStr::from_ptr(args_json).to_string_lossy() };
    let args = Reply { value: Value::parse(&text).unwrap_or_default(), parts: copy_parts(parts, part_count) };
    let Some((_, function)) = state.functions.iter().find(|(known, _)| *known == name) else {
        answer(&state.handle, request, Err(format!("the application has no function \"{name}\"")));
        return;
    };
    match function {
        HostFunction::Now(function) => {
            let outcome = crate::guard(|| function(args)).unwrap_or_else(|| Err("the application's function panicked".into()));
            answer(&state.handle, request, outcome);
        }
        HostFunction::Later(function) => match crate::guard(|| function(args)) {
            Some(future) => Arc::new(HostTask { future: Mutex::new(Some(future)), handle: state.handle.clone(), request }).poll(),
            None => answer(&state.handle, request, Err("the application's function panicked".into())),
        },
    }
}

unsafe fn copy_parts(parts: *const sys::zebkit_part, part_count: usize) -> Vec<Vec<u8>> {
    if parts.is_null() {
        return Vec::new();
    }
    std::slice::from_raw_parts(parts, part_count)
        .iter()
        .map(|part| if part.data.is_null() { Vec::new() } else { std::slice::from_raw_parts(part.data, part.size).to_vec() })
        .collect()
}

/// The handle, shared with the calls so that a cancel after close does
/// nothing (the engine frees the handle once the last call is told).
struct Handle {
    raw: RwLock<*mut sys::zebkit_library>,
}

// SAFETY: the C API lets any thread call and cancel; close is on the
// engine's thread, and the lock keeps a cancel from racing it.
unsafe impl Send for Handle {}
unsafe impl Sync for Handle {}

type Outcome = Result<Reply, LibraryError>;
type Then = Box<dyn FnOnce(Outcome) + Send>;

#[derive(Default)]
struct State {
    outcome: Option<Outcome>,
    wakers: Vec<Waker>,
    thens: Vec<Then>,
}

#[derive(Default)]
struct Shared {
    state: Mutex<State>,
}

impl Shared {
    fn finish(&self, outcome: Outcome) {
        let (wakers, thens) = {
            let mut state = self.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            if state.outcome.is_some() {
                return;
            }
            state.outcome = Some(outcome.clone());
            (std::mem::take(&mut state.wakers), std::mem::take(&mut state.thens))
        };
        for waker in wakers {
            waker.wake();
        }
        for then in thens {
            let outcome = outcome.clone();
            crate::guard(move || then(outcome));
        }
    }
}

/// A call's outcome to come: a `Future` of a [`Reply`], or of the
/// [`LibraryError`] it failed with. Dropping it does not cancel the call.
pub struct Call {
    shared: Arc<Shared>,
    handle: Arc<Handle>,
    id: u64,
}

impl Call {
    /// The call's number (0 for the load's, or a call refused at once).
    pub fn id(&self) -> u64 {
        self.id
    }

    /// Whether it has been told.
    pub fn is_done(&self) -> bool {
        self.shared.state.lock().map(|state| state.outcome.is_some()).unwrap_or(true)
    }

    /// The call is told [`Code::Cancelled`], unless it was told already.
    /// Any thread.
    pub fn cancel(&self) {
        if self.id == 0 {
            return;
        }
        let raw = self.handle.raw.read().unwrap_or_else(|poisoned| poisoned.into_inner());
        if !raw.is_null() {
            unsafe { sys::zebkit_library_cancel(*raw, self.id) }
        }
    }

    /// `function` gets the outcome on the engine's thread when it is told,
    /// or at once, on this thread, if it was told already.
    pub fn then<F: FnOnce(Result<Reply, LibraryError>) + Send + 'static>(self, function: F) {
        let outcome = {
            let mut state = self.shared.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            match &state.outcome {
                Some(outcome) => outcome.clone(),
                None => {
                    state.thens.push(Box::new(function));
                    return;
                }
            }
        };
        function(outcome);
    }
}

impl Future for Call {
    type Output = Result<Reply, LibraryError>;

    fn poll(self: Pin<&mut Self>, context: &mut Context<'_>) -> Poll<Self::Output> {
        let mut state = self.shared.state.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
        match &state.outcome {
            Some(outcome) => Poll::Ready(outcome.clone()),
            None => {
                if !state.wakers.iter().any(|waker| waker.will_wake(context.waker())) {
                    state.wakers.push(context.waker().clone());
                }
                Poll::Pending
            }
        }
    }
}

unsafe extern "C" fn told(
    _library: *mut sys::zebkit_library,
    code: sys::zebkit_library_code,
    json: *const c_char,
    parts: *const sys::zebkit_part,
    part_count: usize,
    data: *mut c_void,
) {
    let shared = Arc::from_raw(data.cast::<Shared>());
    let text = if json.is_null() { "null".into() } else { CStr::from_ptr(json).to_string_lossy() };
    let outcome = if code == sys::ZEBKIT_LIBRARY_OK {
        let parts = copy_parts(parts, part_count);
        match Value::parse(&text) {
            Ok(value) => Ok(Reply { value, parts }),
            Err(error) => Err(LibraryError { code: Code::BadResult, message: error.message().to_string(), name: String::new(), stack: String::new() }),
        }
    } else {
        let info = Value::parse(&text).unwrap_or_default();
        let field = |name: &str| info[name].as_str().unwrap_or_default().to_string();
        Err(LibraryError { code: Code::from_raw(code), message: field("message"), name: field("name"), stack: field("stack") })
    };
    shared.finish(outcome);
}

/// A loaded JavaScript library. Dropping it closes it (on the engine's
/// thread: from another thread, the close is posted there).
pub struct Library {
    handle: Arc<Handle>,
    ready: Arc<Shared>,
    host: *mut HostState,
}

// SAFETY: `host` is used on the engine's thread only: made at load, freed at
// close (Drop, which posts itself there).
unsafe impl Send for Library {}
unsafe impl Sync for Library {}

impl Library {
    /// Loads the library at `path` (a folder with `index.mjs`, or one `.mjs`
    /// file) on a thread of its own and returns at once; calls wait for the
    /// load. `options` is `zebkit_library_load()`'s object (`timeoutMs`,
    /// `queue`, `heapLimitMB`, `maxBytes`, `network`), `Value::Null` for
    /// none. An error for bad options or a path that is no library. The
    /// engine's thread.
    pub fn load(path: impl AsRef<Path>, options: &Value) -> Result<Library, Error> {
        Library::load_with_host(path, options, Host::new())
    }

    /// [`Library::load`] with the application's functions the library
    /// calls as `host.call(name, ...args)` (`options`' `"host"` is their
    /// names). A function's answer goes back once; one not answered in the
    /// library's `timeoutMs` is a `TimeoutError` in the library.
    pub fn load_with_host(path: impl AsRef<Path>, options: &Value, host: Host) -> Result<Library, Error> {
        require_engine_thread("Library::load()");
        let path = c_string(&path.as_ref().to_string_lossy());
        let mut options = options.clone();
        if !host.functions.is_empty() {
            options.set("host", Value::Array(host.functions.iter().map(|(name, _)| Value::from(name.as_str())).collect()));
        }
        let options = if options.is_null() { None } else { Some(c_string(&options.to_json())) };
        let ready = Arc::new(Shared::default());
        let data = Arc::into_raw(ready.clone()).cast_mut().cast::<c_void>();
        let mut error: *mut c_char = ptr::null_mut();
        let raw = unsafe {
            sys::zebkit_library_load(path.as_ptr(), options.as_ref().map_or(ptr::null(), |o| o.as_ptr()), Some(told), data, &mut error)
        };
        if raw.is_null() {
            drop(unsafe { Arc::from_raw(data.cast::<Shared>()) });
            return Err(unsafe { take_error(error, "the library was not loaded") });
        }
        let handle = Arc::new(Handle { raw: RwLock::new(raw) });
        let mut state = ptr::null_mut();
        if !host.functions.is_empty() {
            state = Box::into_raw(Box::new(HostState { handle: handle.clone(), functions: host.functions }));
            unsafe { sys::zebkit_library_set_host(raw, Some(host_called), state.cast()) }
        }
        Ok(Library { handle, ready, host: state })
    }

    /// The load's outcome: `{"functions": [...]}`, the exported functions'
    /// names, or why it failed.
    pub fn ready(&self) -> Call {
        Call { shared: self.ready.clone(), handle: self.handle.clone(), id: 0 }
    }

    /// Calls the exported function `function` with `args`, a JSON array
    /// (`Value::Null` is `[]`), no parts and the library's deadline.
    pub fn call(&self, function: &str, args: Value) -> Call {
        self.call_with(function, &args, &[], None)
    }

    /// Calls with `parts` too (`{"$part": n}` in `args` is `parts[n]`), and a
    /// deadline: `None` the library's (`timeoutMs`), `Some(Duration::MAX)` none.
    /// Arguments are copied before it returns. Any thread.
    pub fn call_with(&self, function: &str, args: &Value, parts: &[&[u8]], timeout: Option<Duration>) -> Call {
        let shared = Arc::new(Shared::default());
        let function = c_string(function);
        let args = if args.is_null() { None } else { Some(c_string(&args.to_json())) };
        let list: Vec<sys::zebkit_part> = parts.iter().map(|part| sys::zebkit_part { data: part.as_ptr(), size: part.len() }).collect();
        let timeout_ms = match timeout {
            None => 0,
            Some(Duration::MAX) => -1,
            Some(duration) => duration.as_millis().clamp(1, i64::MAX as u128) as i64,
        };
        let data = Arc::into_raw(shared.clone()).cast_mut().cast::<c_void>();
        let id = {
            let raw = self.handle.raw.read().unwrap_or_else(|poisoned| poisoned.into_inner());
            if raw.is_null() {
                0
            } else {
                unsafe {
                    sys::zebkit_library_call(
                        *raw,
                        function.as_ptr(),
                        args.as_ref().map_or(ptr::null(), |a| a.as_ptr()),
                        if list.is_empty() { ptr::null() } else { list.as_ptr() },
                        list.len(),
                        timeout_ms,
                        Some(told),
                        data,
                    )
                }
            }
        };
        if id == 0 {
            drop(unsafe { Arc::from_raw(data.cast::<Shared>()) });
            shared.finish(Err(LibraryError {
                code: Code::Gone,
                message: "the library is closed".into(),
                name: String::new(),
                stack: String::new(),
            }));
        }
        Call { shared, handle: self.handle.clone(), id }
    }

    /// Ends the library: calls not told yet are told [`Code::Gone`]. The
    /// engine's thread.
    pub fn close(self) {
        require_engine_thread("Library::close()");
        drop(self);
    }
}

impl Drop for Library {
    fn drop(&mut self) {
        let handle = self.handle.clone();
        // The address only: Send, and freed on the engine's thread.
        let host = self.host as usize;
        let close = move || {
            let mut raw = handle.raw.write().unwrap_or_else(|poisoned| poisoned.into_inner());
            if !raw.is_null() {
                unsafe { sys::zebkit_library_close(*raw) }
                *raw = ptr::null_mut();
            }
            drop(raw);
            // No request reaches it after close.
            if host != 0 {
                drop(unsafe { Box::from_raw(host as *mut HostState) });
            }
        };
        if on_engine_thread() {
            close();
        } else {
            crate::post(close);
        }
    }
}
