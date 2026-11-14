//! `<zebkit/library.h>`: JavaScript libraries as normal libraries
//! (`docs/JS_LIBRARIES.md`). Load and close on the engine's thread; call and
//! cancel from any thread; every callback on the engine's thread.

use std::os::raw::{c_char, c_int, c_void};

/// A loaded library, only ever behind a pointer.
#[repr(C)]
pub struct zebkit_library {
    _private: [u8; 0],
}

/// Bytes beside the JSON: `{"$part": n}` in it is `parts[n]`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct zebkit_part {
    pub data: *const u8,
    pub size: usize,
}

pub type zebkit_library_code = c_int;
pub const ZEBKIT_LIBRARY_OK: zebkit_library_code = 0;
pub const ZEBKIT_LIBRARY_THROWN: zebkit_library_code = 1;
pub const ZEBKIT_LIBRARY_LOAD_FAILED: zebkit_library_code = 2;
pub const ZEBKIT_LIBRARY_NO_FUNCTION: zebkit_library_code = 3;
pub const ZEBKIT_LIBRARY_BAD_ARGUMENTS: zebkit_library_code = 4;
pub const ZEBKIT_LIBRARY_BAD_RESULT: zebkit_library_code = 5;
pub const ZEBKIT_LIBRARY_TIMEOUT: zebkit_library_code = 6;
pub const ZEBKIT_LIBRARY_CANCELLED: zebkit_library_code = 7;
pub const ZEBKIT_LIBRARY_QUEUE_FULL: zebkit_library_code = 8;
pub const ZEBKIT_LIBRARY_OUT_OF_MEMORY: zebkit_library_code = 9;
pub const ZEBKIT_LIBRARY_GONE: zebkit_library_code = 10;

/// A call's outcome, or the load's: `json` the value when OK, else
/// `{"message", "name", "stack"}`; it and `parts` valid for the call.
pub type zebkit_library_func = unsafe extern "C" fn(
    library: *mut zebkit_library,
    code: zebkit_library_code,
    json: *const c_char,
    parts: *const zebkit_part,
    part_count: usize,
    data: *mut c_void,
);

/// The library's `host.call(name, ...args)`: answer `request` exactly once
/// with `zebkit_library_answer()`, from any thread.
pub type zebkit_library_host_func = unsafe extern "C" fn(
    library: *mut zebkit_library,
    request: u64,
    name: *const c_char,
    args_json: *const c_char,
    parts: *const zebkit_part,
    part_count: usize,
    data: *mut c_void,
);

extern "C" {
    pub fn zebkit_library_load(
        path: *const c_char,
        options_json: *const c_char,
        ready: Option<zebkit_library_func>,
        data: *mut c_void,
        error: *mut *mut c_char,
    ) -> *mut zebkit_library;
    pub fn zebkit_library_call(
        library: *mut zebkit_library,
        function: *const c_char,
        args_json: *const c_char,
        parts: *const zebkit_part,
        part_count: usize,
        timeout_ms: i64,
        func: Option<zebkit_library_func>,
        data: *mut c_void,
    ) -> u64;
    pub fn zebkit_library_cancel(library: *mut zebkit_library, call: u64);
    pub fn zebkit_library_set_host(library: *mut zebkit_library, func: Option<zebkit_library_host_func>, data: *mut c_void);
    pub fn zebkit_library_answer(
        library: *mut zebkit_library,
        request: u64,
        code: zebkit_library_code,
        json: *const c_char,
        parts: *const zebkit_part,
        part_count: usize,
    ) -> c_int;
    pub fn zebkit_library_close(library: *mut zebkit_library);
}
