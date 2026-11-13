//! A page's new windows through the Rust binding (`View::on_window_open`,
//! `on_window_open_event`, `on_popup_blocked`, `set_popup_policy`,
//! `window_open_info`), one mode an engine; run by tests/engine/popup.py,
//! which has the modes and the server (ZEBKIT_POPUP_BASE, ZEBKIT_POPUP_CASE).
//! It ends with "N of M checks passed".
//!
//! The Python program also asks for the snapshot from another thread: a
//! `View` cannot leave its thread in Rust, so that cannot be written.

use std::cell::{Cell, RefCell};
use std::collections::HashSet;
use std::rc::Rc;
use std::time::{Duration, Instant};

use zebkit::json::Value;
use zebkit::{PopupPolicy, View};

const FLAGS: [&str; 5] = ["hasX", "hasY", "hasWidth", "hasHeight", "popup"];

thread_local! {
    static CHECKS: Cell<u32> = const { Cell::new(0) };
    static FAILED: Cell<u32> = const { Cell::new(0) };
}

fn check(ok: bool, what: &str, detail: impl std::fmt::Debug) -> bool {
    CHECKS.with(|checks| checks.set(checks.get() + 1));
    if ok {
        println!("ok   {what}");
    } else {
        FAILED.with(|failed| failed.set(failed.get() + 1));
        println!("FAIL {what}: {detail:?}");
    }
    ok
}

fn wait(condition: impl Fn() -> bool, seconds: u64) -> bool {
    let end = Instant::now() + Duration::from_secs(seconds);
    loop {
        zebkit::pump();
        if condition() {
            return true;
        }
        if Instant::now() >= end {
            return false;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
}

fn must_wait(condition: impl Fn() -> bool, what: &str) -> Result<(), String> {
    if wait(condition, 15) { Ok(()) } else { Err(format!("timed out waiting for {what}")) }
}

fn ev(view: &View, script: &str) -> Result<Value, String> {
    let answer: Rc<RefCell<Option<Result<String, zebkit::Error>>>> = Rc::default();
    let into = answer.clone();
    view.evaluate(script, move |result| *into.borrow_mut() = Some(result));
    must_wait(|| answer.borrow().is_some(), script)?;
    let result = answer.borrow_mut().take().unwrap();
    match result {
        Ok(json) => Value::parse(&json).map_err(|error| error.to_string()),
        Err(error) => Err(format!("{script}: {error}")),
    }
}

fn load(view: &View, base: &str, path: &str) -> Result<(), String> {
    let url = format!("{base}{path}");
    view.load_url(&url);
    must_wait(
        || view.url().as_deref() == Some(url.as_str()) && ev(view, "document.readyState").ok() == Some(Value::from("complete")),
        path,
    )
}

/// JSON equality, objects' members in any order.
fn same(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Object(x), Value::Object(y)) => {
            x.len() == y.len() && x.iter().all(|(key, value)| b.get(key).is_some_and(|other| same(value, other)))
        }
        (Value::Array(x), Value::Array(y)) => x.len() == y.len() && x.iter().zip(y).all(|(x, y)| same(x, y)),
        _ => a == b,
    }
}

fn info(view: &View) -> Option<Value> {
    view.window_open_info().and_then(|json| Value::parse(&json).ok())
}

macro_rules! ensure {
    ($condition:expr, $($detail:tt)+) => {
        if !$condition {
            return Err(format!($($detail)+));
        }
    };
}

type Events = Rc<RefCell<Vec<Value>>>;

fn recording(events: &Events) -> impl FnMut(&str) + 'static {
    let events = events.clone();
    move |json: &str| events.borrow_mut().push(Value::parse(json).unwrap_or_default())
}

fn bounds(x: f64, y: f64, width: f64, height: f64) -> Value {
    Value::object([("x", Value::Number(x)), ("y", Value::Number(y)), ("width", Value::Number(width)), ("height", Value::Number(height))])
}

/// Each window the page opened: its number, its view, its creation snapshot.
type Windows = Rc<RefCell<Vec<(usize, View, Value)>>>;

struct Run {
    base: String,
    first: View,
    other: View,
    closed: Rc<Cell<bool>>,
    events: Events,
    legacy: Events,
    windows: Windows,
    gone: Rc<RefCell<HashSet<usize>>>,
}

impl Run {
    fn mode(&self, mode: &str) -> Result<(), String> {
        let (base, first, events, legacy, windows) = (&self.base, &self.first, &self.events, &self.legacy, &self.windows);
        if matches!(mode, "blocked" | "isolation" | "remove" | "frame") {
            first.set_popup_policy(PopupPolicy::Block);
        }
        match mode {
            "blocked" => {
                load(first, base, "/blocked")?;
                must_wait(|| !events.borrow().is_empty() && !legacy.borrow().is_empty(), "the decision and the older event")?;
                let decision = events.borrow().last().cloned().unwrap();
                ensure!(decision["phase"].as_str() == Some("decision") && decision["allowed"].as_bool() == Some(false), "{decision:?}");
                ensure!(
                    decision["url"].as_str() == Some(format!("{base}/popup-blocked").as_str()) && decision["frameName"].as_str() == Some("blocked"),
                    "{decision:?}"
                );
                ensure!(
                    decision["userGesture"].as_bool() == Some(false) && decision["disposition"].as_str() == Some("new-popup"),
                    "{decision:?}"
                );
                let blocked = format!("{base}/blocked");
                ensure!(
                    decision["openerUrl"].as_str() == Some(blocked.as_str()) && decision["topUrl"].as_str() == Some(blocked.as_str()),
                    "{decision:?}"
                );
                ensure!(
                    decision["sourceOrigin"].as_str() == Some(base.as_str()) && decision["openerSuppressed"].as_bool() == Some(false),
                    "{decision:?}"
                );
                let features = &decision["features"];
                ensure!(same(&features["bounds"], &bounds(0.0, 0.0, 420.0, 360.0)), "{features:?}");
                ensure!(
                    FLAGS.iter().all(|flag| features[*flag].as_bool() == Some(true)) && features["alwaysOnTop"].as_bool() == Some(false),
                    "{features:?}"
                );
                let expected = Value::object([
                    ("url", Value::from(format!("{base}/popup-blocked"))),
                    ("frameName", Value::from("blocked")),
                    ("userGesture", Value::from(false)),
                ]);
                ensure!(legacy.borrow().len() == 1 && same(&legacy.borrow()[0], &expected), "{:?}", legacy.borrow());
                ensure!(windows.borrow().is_empty(), "windows opened");
            }
            "isolation" => {
                let other = &self.other;
                other.set_popup_policy(PopupPolicy::Block);
                let other_events: Events = Rc::default();
                other.on_window_open_event(recording(&other_events));
                load(other, base, "/blocked")?;
                must_wait(|| !other_events.borrow().is_empty(), "the other view's decision")?;
                ensure!(events.borrow().is_empty() && legacy.borrow().is_empty(), "{:?} {:?}", events.borrow(), legacy.borrow());
            }
            "created" | "snapshot" => {
                first.set_popup_policy(PopupPolicy::Allow);
                let opened = ev(
                    first,
                    "window.one=window.open('/one','one','left=0,top=0,width=420,height=360,popup=yes');\
                     window.two=window.open('/two','two','left=50,top=60,width=340,height=260,popup=yes');\
                     !!one && !!two",
                )?;
                ensure!(opened == Value::from(true), "{opened:?}");
                must_wait(|| windows.borrow().len() == 2, "two windows")?;
                let created = || events.borrow().iter().filter(|event| event["phase"].as_str() == Some("created")).cloned().collect::<Vec<_>>();
                must_wait(|| created().len() == 2, "two creation events")?;
                let by_url = |url: String| {
                    windows.borrow().iter().find(|(_, _, creation)| creation["url"].as_str() == Some(url.as_str())).map(|(_, child, creation)| (child.clone(), creation.clone())).unwrap()
                };
                let (one_view, one) = by_url(format!("{base}/one"));
                let (_, two) = by_url(format!("{base}/two"));
                if mode == "created" {
                    ensure!(same(&one["features"]["bounds"], &bounds(0.0, 0.0, 420.0, 360.0)), "{one:?}");
                    ensure!(same(&two["features"]["bounds"], &bounds(50.0, 60.0, 340.0, 260.0)), "{two:?}");
                    ensure!(one["targetId"] != two["targetId"], "{one:?} {two:?}");
                    ensure!(
                        one["hasOpener"].as_bool() == Some(true)
                            && one["openerOrigin"].as_str() == Some(base.as_str())
                            && one["topOrigin"].as_str() == Some(base.as_str()),
                        "{one:?}"
                    );
                    ensure!(one["frameName"].as_str() == Some("one") && two["frameName"].as_str() == Some("two"), "{one:?} {two:?}");
                    let told = created();
                    let told_of = |creation: &Value| told.iter().find(|event| event["targetId"] == creation["targetId"]).cloned();
                    ensure!(told_of(&one).is_some_and(|told| same(&told, &one)) && told_of(&two).is_some_and(|told| same(&told, &two)), "{told:?}");
                } else {
                    let moved = format!("{base}/moved");
                    one_view.load_url(&moved);
                    must_wait(|| one_view.url().as_deref() == Some(moved.as_str()), "the window's next page")?;
                    ensure!(info(&one_view).is_some_and(|now| same(&now, &one)), "{:?}", info(&one_view));
                }
            }
            "plain" | "noopener" | "close-creation" => {
                first.set_popup_policy(PopupPolicy::Allow);
                let script = match mode {
                    "plain" => "window.open('/plain','plain'); true",
                    "noopener" => "window.open('/no-opener','_blank','noopener,width=300,height=240'); true",
                    _ => "window.open('/closed-during-creation','close-immediately'); true",
                };
                ev(first, script)?;
                must_wait(|| !windows.borrow().is_empty(), "the window")?;
                let (serial, _, creation) = windows.borrow().last().cloned().unwrap();
                match mode {
                    "plain" => ensure!(!FLAGS.iter().any(|flag| creation["features"][*flag].as_bool() == Some(true)), "{creation:?}"),
                    "noopener" => {
                        ensure!(creation["hasOpener"].as_bool() == Some(false) && creation["openerOrigin"].is_null(), "{creation:?}");
                        let url = format!("{base}/no-opener");
                        must_wait(
                            || {
                                events.borrow().iter().any(|event| {
                                    event["url"].as_str() == Some(url.as_str())
                                        && event["phase"].as_str() == Some("decision")
                                        && event["openerSuppressed"].as_bool() == Some(true)
                                })
                            },
                            "the decision, opener suppressed",
                        )?;
                    }
                    _ => must_wait(|| self.gone.borrow().contains(&serial), "closed from its creation")?,
                }
            }
            "frame" => {
                load(first, base, "/with-frame")?;
                let child_origin = base.replace("127.0.0.1", "localhost");
                let url = format!("{child_origin}/from-frame");
                let find = || events.borrow().iter().find(|event| event["url"].as_str() == Some(url.as_str())).cloned();
                must_wait(|| find().is_some(), "the frame's decision")?;
                let cross = find().unwrap();
                ensure!(
                    cross["sourceOrigin"].as_str() == Some(child_origin.as_str())
                        && cross["openerUrl"].as_str() == Some(format!("{child_origin}/frame").as_str()),
                    "{cross:?}"
                );
                ensure!(
                    cross["topUrl"].as_str() == Some(format!("{base}/with-frame").as_str()) && cross["allowed"].as_bool() == Some(false),
                    "{cross:?}"
                );
            }
            "remove" => {
                first.clear_window_open_event_handler();
                ev(first, "window.open('/removed','removed'); true")?;
                must_wait(|| !legacy.borrow().is_empty(), "the older event")?;
                ensure!(events.borrow().is_empty(), "{:?}", events.borrow());
            }
            "close-event" => {
                let view = first.clone();
                first.on_window_open_event(move |_: &str| view.close());
                first.evaluate("window.open('/close','close'); true", |_| {});
                must_wait(|| self.closed.get(), "closed from the observer")?;
            }
            "adoption" => self.adoption()?,
            _ => return Err(format!("no such mode: {mode}")),
        }
        Ok(())
    }

    /// The window as a view of its own: its opener, a message to it, its
    /// snapshot when it loads another page, closing it; the functions removed
    /// while they run.
    fn adoption(&self) -> Result<(), String> {
        let (parent, base) = (&self.first, &self.base);
        parent.set_popup_policy(PopupPolicy::Allow);
        let children: Rc<RefCell<Vec<View>>> = Rc::default();
        let told: Events = Rc::default();
        let mismatched = Rc::new(Cell::new(false));
        {
            let (children, opener, mismatched) = (children.clone(), parent.clone(), mismatched.clone());
            parent.on_window_open(move |child, url| {
                if info(&child).map(|creation| creation["url"].as_str() == Some(url)) != Some(true) {
                    mismatched.set(true);
                }
                children.borrow_mut().push(child);
                opener.clear_window_open_handler();
            });
            let (told, opener) = (told.clone(), parent.clone());
            parent.on_window_open_event(move |json: &str| {
                let metadata = Value::parse(json).unwrap_or_default();
                let created = metadata["phase"].as_str() == Some("created");
                told.borrow_mut().push(metadata);
                if created {
                    opener.clear_window_open_event_handler();
                }
            });
        }
        let opened = ev(parent, "window.child=window.open('/child','child','left=0,top=0,width=320,height=240'); !!child")?;
        ensure!(opened == Value::from(true), "{opened:?}");
        must_wait(
            || !children.borrow().is_empty() && told.borrow().iter().any(|event| event["phase"].as_str() == Some("created")),
            "the window and its event",
        )?;
        ensure!(!mismatched.get(), "the window's snapshot is not of its URL");
        let child = children.borrow()[0].clone();
        let page = format!("{base}/child");
        must_wait(|| child.url().as_deref() == Some(page.as_str()), "the window's page")?;
        ensure!(ev(&child, "window.opener !== null")? == Value::from(true), "no opener");
        ensure!(ev(&child, "window.opener.fromChild='delivered'; true")? == Value::from(true), "the message");
        ensure!(ev(parent, "window.fromChild")? == Value::from("delivered"), "the message did not arrive");
        let snapshot = info(&child).unwrap_or_default();
        ensure!(snapshot["targetId"].as_str() == child.target_id().as_deref(), "{snapshot:?}");
        let moved = format!("{base}/moved");
        child.load_url(&moved);
        must_wait(|| child.url().as_deref() == Some(moved.as_str()), "the window's next page")?;
        ensure!(info(&child).is_some_and(|now| same(&now, &snapshot)), "{:?}", info(&child));
        let gone = Rc::new(Cell::new(false));
        let signal = gone.clone();
        child.on_close(move || signal.set(true));
        child.close();
        must_wait(|| gone.get(), "the window closes")?;
        // The opener's process hears of it a moment later, as in Chrome.
        must_wait(|| ev(parent, "window.child.closed").ok() == Some(Value::from(true)), "the opener sees it closed")?;
        ensure!(child.window_open_info().is_none(), "a closed window's snapshot");
        Ok(())
    }
}

fn main() {
    let base = std::env::var("ZEBKIT_POPUP_BASE").expect("ZEBKIT_POPUP_BASE");
    let mode = std::env::var("ZEBKIT_POPUP_CASE").expect("ZEBKIT_POPUP_CASE");
    let mut args: Vec<String> = std::env::args().collect();
    args[0] = "popup-rust".into();
    if let Err(error) = zebkit::init_with_settings(args, r#"{"logging":{"destination":"none"}}"#) {
        println!("FAIL zebkit::init(): {error}");
        std::process::exit(1);
    }
    let run = Run {
        base,
        first: View::new(None, 0, 0),
        other: View::new(None, 0, 0),
        closed: Rc::default(),
        events: Rc::default(),
        legacy: Rc::default(),
        windows: Rc::default(),
        gone: Rc::default(),
    };
    let signal = run.closed.clone();
    run.first.on_close(move || signal.set(true));
    check(info(&run.first) == Some(Value::Object(Vec::new())), "a view of the application's: no creation snapshot", info(&run.first));
    let loaded = load(&run.first, &run.base, "/initial");
    if let Err(error) = loaded {
        check(false, "the first page", error);
    } else {
        run.first.on_window_open_event(recording(&run.events));
        run.first.on_popup_blocked(recording(&run.legacy));
        if mode != "adoption" {
            let (windows, gone) = (run.windows.clone(), run.gone.clone());
            run.first.on_window_open(move |child: View, url: &str| {
                let creation = info(&child).unwrap_or_default();
                check(
                    creation["phase"].as_str() == Some("created") && creation["url"].as_str() == Some(url),
                    &format!("the window's snapshot, made at its creation: {url}"),
                    &creation,
                );
                let serial = windows.borrow().len() + 1;
                let closed = gone.clone();
                child.on_close(move || {
                    closed.borrow_mut().insert(serial);
                });
                let close = creation["frameName"].as_str() == Some("close-immediately");
                windows.borrow_mut().push((serial, child.clone(), creation));
                if close {
                    child.close();
                }
            });
        }
        let result = run.mode(&mode);
        check(result.is_ok(), &format!("popup {mode}"), result.err());
    }
    for (_, child, _) in run.windows.borrow().iter() {
        child.close();
    }
    let all = run.windows.borrow().len();
    check(wait(|| run.gone.borrow().len() == all, 10), "the windows close", ());
    run.other.close();
    if !run.closed.get() {
        run.first.close();
    }
    check(wait(|| run.closed.get(), 10), "the views close", ());
    zebkit::shutdown();
    let (checks, failed) = (CHECKS.with(Cell::get), FAILED.with(Cell::get));
    println!("{} of {checks} checks passed", checks - failed);
    std::process::exit(if failed == 0 { 0 } else { 1 });
}
