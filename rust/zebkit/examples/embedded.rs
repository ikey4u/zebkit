//! An application's own loop, before, during and after the engine's life.
//! Run with --no-sandbox --headless on a build machine.
use std::cell::Cell;
use std::rc::Rc;
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};
use zebkit::{LoadEvent, PopupPolicy, View};

fn main() {
    let schedule = Arc::new((Mutex::new(None::<Instant>), Condvar::new()));
    let wakeup = schedule.clone();
    assert!(zebkit::set_wakeup(move |delay| {
        let due = Instant::now() + delay;
        let mut pending = wakeup.0.lock().unwrap();
        if pending.is_none_or(|old| due < old) {
            *pending = Some(due);
            wakeup.1.notify_one();
        }
    }));
    assert!(zebkit::init_with_settings(["embedded"], r#"{"gpu":6}"#).is_err());
    zebkit::init_with_settings(std::env::args_os(),
        r#"{"userAgent":"Zebkit Rust Host","logging":{"destination":"none"}}"#)
        .expect("initialize the engine");
    struct ReadyCapture(Rc<Cell<bool>>);
    impl Drop for ReadyCapture {
        fn drop(&mut self) { self.0.set(true); }
    }
    let reclaimed = Rc::new(Cell::new(false));
    let capture = ReadyCapture(reclaimed.clone());
    assert_ne!(zebkit::run_with_args(["embedded"], move || drop(capture)), 0);
    assert!(reclaimed.get(), "rejected startup releases its ready closure");
    let view = View::new(None, 640, 480);
    assert!(view.set_popup_policy(PopupPolicy::Block));
    assert_eq!(view.popup_policy(), Some(PopupPolicy::Block));
    let blocked = Rc::new(Cell::new(false));
    let event = blocked.clone();
    view.on_popup_blocked(move |json| event.set(json.contains("popup.invalid")));
    let evaluated = Rc::new(Cell::new(false));
    let evaluated_in_callback = evaluated.clone();
    let page = view.clone();
    view.on_load(move |event, _| {
        if event == LoadEvent::Finished {
            let result = evaluated_in_callback.clone();
            page.evaluate("[6 * 7, navigator.userAgent]", move |value|
                result.set(matches!(value.as_deref(), Ok(r#"[42,"Zebkit Rust Host"]"#))));
        }
    });
    view.load_url("data:text/html,<script>window.open('https://popup.invalid','target')</script>");
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline && !(evaluated.get() && blocked.get()) {
        let mut pending = schedule.0.lock().unwrap();
        let ready = loop {
            let now = Instant::now();
            if now >= deadline { break false; }
            if pending.is_some_and(|due| due <= now) {
                *pending = None;
                break true;
            }
            let due = pending.unwrap_or(deadline).min(deadline);
            pending = schedule.1.wait_timeout(pending, due - now).unwrap().0;
        };
        drop(pending);
        if ready { assert!(zebkit::pump()); }
    }
    let completed = evaluated.get() && blocked.get();
    assert!(zebkit::shutdown());
    assert!(!zebkit::pump());
    assert!(completed, "the page and its popup event complete");
    println!("ok Rust host loop, popup hook and shutdown");
}
