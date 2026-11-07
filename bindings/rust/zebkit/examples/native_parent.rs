//! `View::attach_native` under an X server: a window of plain Xlib as the
//! parent, its tree read back, the parent destroyed first. Run by
//! scripts/engine-test.sh under Xvfb; see tests/engine/native_parent.c.

#[cfg(target_os = "linux")]
mod x11 {
    use std::os::raw::{c_int, c_uint, c_ulong, c_void};
    #[link(name = "X11")]
    extern "C" {
        pub fn XOpenDisplay(name: *const c_void) -> *mut c_void;
        pub fn XDefaultRootWindow(display: *mut c_void) -> c_ulong;
        pub fn XCreateSimpleWindow(display: *mut c_void, parent: c_ulong, x: c_int, y: c_int, width: c_uint, height: c_uint, border: c_uint, border_color: c_ulong, background: c_ulong) -> c_ulong;
        pub fn XMapWindow(display: *mut c_void, window: c_ulong) -> c_int;
        pub fn XDestroyWindow(display: *mut c_void, window: c_ulong) -> c_int;
        pub fn XSync(display: *mut c_void, discard: c_int) -> c_int;
        pub fn XFree(data: *mut c_void) -> c_int;
        pub fn XQueryTree(display: *mut c_void, window: c_ulong, root: *mut c_ulong, parent: *mut c_ulong, children: *mut *mut c_ulong, count: *mut c_uint) -> c_int;
    }
}

#[cfg(target_os = "linux")]
fn main() {
    use std::cell::Cell;
    use std::rc::Rc;
    use std::time::Duration;
    use zebkit::{LoadEvent, View};

    fn children(display: *mut std::os::raw::c_void, window: std::os::raw::c_ulong) -> u32 {
        let (mut root, mut parent, mut listed, mut count) = (0, 0, std::ptr::null_mut(), 0);
        unsafe {
            x11::XQueryTree(display, window, &mut root, &mut parent, &mut listed, &mut count);
            if !listed.is_null() {
                x11::XFree(listed.cast());
            }
        }
        count
    }

    let display = unsafe { x11::XOpenDisplay(std::ptr::null()) };
    assert!(!display.is_null(), "no X11 display");
    let host = unsafe {
        let window = x11::XCreateSimpleWindow(display, x11::XDefaultRootWindow(display), 700, 50, 640, 480, 0, 0, 0x202020);
        x11::XMapWindow(display, window);
        x11::XSync(display, 0);
        window
    };
    let code = zebkit::run(move || {
        let view = View::new(Some("data:text/html,<title>rust</title>"), 640, 480);
        let closed = Rc::new(Cell::new(false));
        let closing = closed.clone();
        view.on_close(move || closing.set(true));
        let attached = view.clone();
        view.on_load(move |event, _| {
            if event != LoadEvent::Finished || children(display, host) != 0 {
                return;
            }
            let refused = attached.attach_native(0x7fff_fff0, 0, 0, 100, 100);
            println!("{} a window that is not there: refused", if refused.is_err() { "ok  " } else { "FAIL" });
            attached.attach_native(host as usize, 10, 10, 300, 200).expect("attach_native");
            assert_eq!(attached.native_window(), Some(zebkit::NativeWindow::X11(host as usize)));
            println!("ok   native_window: containing host window");
            attached.export_native_window(move |handle| {
                let expected = format!("x11:{:#x}", host);
                let ok = handle.as_deref() == Some(expected.as_str());
                println!("{} export_native_window: the portal's x11:<XID> of the host ({handle:?})",
                         if ok { "ok  " } else { "FAIL" });
                if !ok {
                    std::process::exit(1);
                }
            });
            fn wait(tries: u32, what: &'static str, done: impl Fn() -> bool + 'static, then: impl FnOnce() + 'static) {
                if done() {
                    println!("ok   {what}");
                    then();
                } else if tries == 0 {
                    println!("FAIL {what}");
                    std::process::exit(1);
                } else {
                    zebkit::after(Duration::from_millis(100), move || wait(tries - 1, what, done, then));
                }
            }
            let closed = closed.clone();
            wait(100, "attach_native: the host's child", move || { unsafe { x11::XSync(display, 0) }; children(display, host) == 1 }, move || {
                unsafe {
                    x11::XDestroyWindow(display, host);
                    x11::XSync(display, 0);
                }
                let closed = closed.clone();
                wait(100, "the parent destroyed first: the view closes", move || closed.get(), zebkit::quit);
            });
        });
    });
    std::process::exit(code);
}

#[cfg(not(target_os = "linux"))]
fn main() {}
