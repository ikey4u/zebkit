//! Everything it takes to put zebkit in a GPUI application: the engine runs
//! in GPUI's event loop, and each page is a native child window of GPUI's
//! window. The rest of the example is ordinary GPUI-KIT code.

use futures::{FutureExt as _, StreamExt as _};
use gpui_kit::{App, Bounds, Pixels, Window, WindowCompositionSurface};
use raw_window_handle::{HasWindowHandle as _, RawWindowHandle};
use std::{
    cell::Cell,
    time::{Duration, Instant},
};

/// On Linux the engine reads its X11 connection inside `zebkit::pump()`, so
/// it is pumped at least once a frame; elsewhere GPUI's own loop delivers the
/// engine's window messages and only its timers need waking.
const NATIVE_EVENTS: Option<Duration> = if cfg!(target_os = "linux") { Some(Duration::from_millis(16)) } else { None };

/// Starts the engine on GPUI's main thread and pumps it from GPUI's executor:
/// zebkit asks for a pump (from any thread) through the wakeup, GPUI runs it.
pub fn start_engine(mut args: Vec<String>, cx: &mut App) -> Result<(), String> {
    if cfg!(target_os = "linux") {
        args.push("--ozone-platform=x11".into()); // Pages go in X11 windows, as GPUI's does.
    }
    let (wake, mut woken) = futures::channel::mpsc::unbounded::<Instant>();
    zebkit::set_wakeup(move |delay| drop(wake.unbounded_send(Instant::now() + delay)));
    zebkit::init(args).map_err(|code| format!("the engine did not start ({code})"))?;
    cx.spawn(async move |cx| {
        loop {
            zebkit::pump();
            // Wait for the engine's next deadline, or a wakeup that brings it closer.
            let mut due = NATIVE_EVENTS.map(|every| Instant::now() + every);
            loop {
                let wait = due.map(|due| due.saturating_duration_since(Instant::now()));
                if wait == Some(Duration::ZERO) {
                    break;
                }
                let executor = cx.background_executor().clone();
                let timer = async move {
                    match wait {
                        Some(wait) => executor.timer(wait).await,
                        None => futures::future::pending().await,
                    }
                };
                futures::select_biased! {
                    at = woken.next() => match at {
                        Some(at) => due = Some(due.map_or(at, |due| due.min(at))),
                        None => return,
                    },
                    _ = timer.fuse() => break,
                }
            }
        }
    })
    .detach();
    Ok(())
}

/// Where pages go: a native surface of GPUI's window composition (GPUI Fast)
/// on X11 and macOS, between GPUI's content and its overlays, so that menus,
/// tooltips, notifications and dialogs show over the page. On Windows
/// (whose surfaces are DirectComposition visuals, not windows) GPUI's window
/// itself, where the page covers what GPUI draws in its rectangle.
pub struct PageHost {
    parent: usize,
    surface: Option<WindowCompositionSurface>,
    /// The content area in the parent's units: (x, y, width, height).
    bounds: Cell<(i32, i32, i32, i32)>,
    /// GPUI's own window, which takes the keyboard back from a page.
    #[cfg_attr(target_os = "linux", allow(dead_code))]
    window: usize,
    #[cfg(target_os = "linux")]
    keys: Option<(std::sync::Arc<x11rb::rust_connection::RustConnection>, u32)>,
}

impl PageHost {
    pub fn new(window: &mut Window) -> Self {
        let surface = (!cfg!(target_os = "windows"))
            .then(|| window.enable_window_composition().and_then(|composition| composition.create_native_surface()).ok())
            .flatten();
        let own = window.window_handle().ok().and_then(|handle| native_parent(&handle.as_raw()));
        let parent = match &surface {
            // X11: a window id in a RawWindowHandle; AppKit: the NSView* itself.
            Some(surface) => surface.platform_surface().and_then(|surface| surface.platform_handle()).ok().and_then(|handle| {
                handle.downcast_ref::<RawWindowHandle>().and_then(native_parent).or(handle.downcast_ref::<usize>().copied())
            }),
            None => own,
        };
        let parent = parent.expect("a native window to put pages in (X11, AppKit or Win32)");
        let own = own.unwrap_or(parent);
        PageHost {
            parent,
            surface,
            bounds: Cell::new((0, 0, 1, 1)),
            window: own,
            #[cfg(target_os = "linux")]
            keys: x11_key_window(own as u32),
        }
    }

    /// Puts a page in the window, at the content area.
    pub fn attach(&self, view: &zebkit::View) {
        let (x, y, width, height) = self.bounds.get();
        if let Err(error) = view.attach_native(self.parent, x, y, width, height) {
            eprintln!("gpui-browser: {error}");
        }
    }

    /// The content area moved or was resized (GPUI logical pixels): the
    /// surface follows, then every page.
    pub fn layout(&self, area: Bounds<Pixels>, scale: f32, pages: impl Iterator<Item = zebkit::View>) {
        let device = area.to_device_pixels(scale);
        let (width, height) = (device.size.width.0, device.size.height.0);
        let bounds = match &self.surface {
            Some(surface) => {
                if let Ok(native) = surface.platform_surface() {
                    let _ = native.set_bounds(device);
                    let _ = native.set_visible(true);
                }
                // In the surface, in AppKit's points or X11's pixels.
                if cfg!(target_os = "macos") {
                    (0, 0, f32::from(area.size.width) as i32, f32::from(area.size.height) as i32)
                } else {
                    (0, 0, width, height)
                }
            }
            // In the window, in Win32's pixels.
            None => (device.origin.x.0, device.origin.y.0, width, height),
        };
        if bounds != self.bounds.get() {
            self.bounds.set(bounds);
            for page in pages {
                page.set_bounds(bounds.0, bounds.1, bounds.2, bounds.3);
            }
        }
    }

    /// Whether GPUI's overlays (dialogs, menus, notifications) show over pages.
    pub fn overlays_above(&self) -> bool {
        self.surface.is_some()
    }

    /// A page's window takes the keyboard when it is clicked; GPUI's window
    /// takes it back when GPUI is clicked or a GPUI-KIT input should type.
    pub fn focus_gpui(&self) {
        #[cfg(target_os = "linux")]
        if let Some((x11, keys)) = &self.keys {
            use x11rb::{
                connection::Connection as _,
                protocol::xproto::{ConnectionExt as _, InputFocus},
            };
            // By way of no window: GPUI's window is told FocusIn and is active
            // again (it is told nothing of a move between its descendants).
            let _ = x11.set_input_focus(InputFocus::NONE, x11rb::NONE, x11rb::CURRENT_TIME);
            let _ = x11.set_input_focus(InputFocus::PARENT, *keys, x11rb::CURRENT_TIME);
            let _ = x11.flush();
        }
        #[cfg(target_os = "windows")]
        unsafe {
            #[link(name = "user32")]
            unsafe extern "system" {
                fn SetFocus(window: usize) -> usize;
            }
            SetFocus(self.window);
        }
        #[cfg(target_os = "macos")]
        unsafe {
            use objc2::{msg_send, runtime::AnyObject};
            let view = self.window as *mut AnyObject;
            let window: *mut AnyObject = msg_send![view, window];
            let _: bool = msg_send![window, makeFirstResponder: view];
        }
    }
}

/// X11 gives keys to the window under the pointer when the focus is one of
/// its ancestors: with GPUI's window focused, a pointer over the page would
/// give the page the keys. So an input-only child of GPUI's window, beside
/// the page, takes the focus, and its keys are passed on to GPUI's window.
#[cfg(target_os = "linux")]
fn x11_key_window(window: u32) -> Option<(std::sync::Arc<x11rb::rust_connection::RustConnection>, u32)> {
    use x11rb::{
        connection::Connection as _,
        protocol::{Event, xproto::*},
    };
    let (x11, _) = x11rb::connect(None).ok()?;
    let keys = x11.generate_id().ok()?;
    let aux = CreateWindowAux::new().event_mask(EventMask::KEY_PRESS | EventMask::KEY_RELEASE);
    x11.create_window(0, keys, window, 0, 0, 1, 1, 0, WindowClass::INPUT_ONLY, 0, &aux).ok()?;
    x11.map_window(keys).ok()?;
    x11.flush().ok()?;
    let x11 = std::sync::Arc::new(x11);
    let forward = x11.clone();
    std::thread::spawn(move || {
        while let Ok(event) = forward.wait_for_event() {
            let (mut key, mask) = match event {
                Event::KeyPress(key) => (key, EventMask::KEY_PRESS),
                Event::KeyRelease(key) => (key, EventMask::KEY_RELEASE),
                _ => continue,
            };
            (key.event, key.child) = (window, x11rb::NONE);
            let _ = forward.send_event(false, window, mask, key);
            let _ = forward.flush();
        }
    });
    Some((x11, keys))
}

/// The handle zebkit_view_attach_native() takes: an X11 window id, an
/// `NSView*`, an HWND.
fn native_parent(handle: &RawWindowHandle) -> Option<usize> {
    match handle {
        RawWindowHandle::Xcb(handle) => Some(handle.window.get() as usize),
        RawWindowHandle::Xlib(handle) => Some(handle.window as usize),
        RawWindowHandle::AppKit(handle) => Some(handle.ns_view.as_ptr() as usize),
        RawWindowHandle::Win32(handle) => Some(handle.hwnd.get() as usize),
        _ => None,
    }
}
