//! A page's console through `View::on_console`, one mode an engine; run by
//! tests/engine/console.py, which has the modes and the server
//! (ZEBKIT_CONSOLE_BASE, ZEBKIT_CONSOLE_CASE). It prints "ok Rust console
//! <mode>" when every check passes.
//!
//! The Python program's "isolation" also sets the function from another
//! thread, and its "validation" gives what is not a function: in Rust a
//! `View` cannot leave its thread and a function is a type, so neither can
//! be written.

use std::cell::RefCell;
use std::rc::Rc;
use std::time::{Duration, Instant};

use zebkit::json::Value;
use zebkit::View;

type Events = Rc<RefCell<Vec<Value>>>;

/// Pumps the engine until `condition` holds or `seconds` pass: whether it held.
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

/// Pumps a while, for checks that something does not happen.
fn settle() {
    wait(|| false, 1);
}

/// The script's value in `view`.
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

macro_rules! ensure {
    ($condition:expr, $($detail:tt)+) => {
        if !$condition {
            return Err(format!($($detail)+));
        }
    };
}

fn recording(events: &Events) -> impl FnMut(&str) + 'static {
    let events = events.clone();
    move |json: &str| events.borrow_mut().push(Value::parse(json).unwrap_or_default())
}

fn messages(events: &Events) -> Vec<String> {
    events.borrow().iter().map(|event| event["message"].as_str().unwrap_or_default().to_string()).collect()
}

fn mode(mode: &str, base: &str, first: &View, other: &View, events: &Events, closed: &Rc<RefCell<bool>>) -> Result<(), String> {
    first.on_console(recording(events));
    match mode {
        "levels" => {
            ev(first, "console.log('中文😀\\u0000tail'); console.warn('warning-text'); console.error('error-text'); \
                       true\n//# sourceURL=native-console-test.js")?;
            must_wait(|| events.borrow().len() >= 3, "three messages")?;
            let events = events.borrow();
            let find = |message: &str| events.iter().find(|event| event["message"].as_str() == Some(message)).cloned();
            let levels: Vec<Option<String>> = ["中文😀\0tail", "warning-text", "error-text"]
                .iter()
                .map(|message| find(message).and_then(|event| event["level"].as_str().map(String::from)))
                .collect();
            ensure!(levels == [Some("info".into()), Some("warning".into()), Some("error".into())], "{events:?}");
            let sample = find("warning-text").unwrap();
            ensure!(sample["origin"].as_str() == Some(base) && sample["mainFrame"].as_bool() == Some(true), "{sample:?}");
            ensure!(
                sample["frameId"].as_str().is_some_and(|id| id.starts_with('f')) && sample["line"].as_f64().is_some_and(|line| line >= 1.0),
                "{sample:?}"
            );
            ensure!(sample["source"].as_str().is_some_and(|source| source.ends_with("native-console-test.js")), "{sample:?}");
            ensure!(sample["stackTrace"].is_null() || sample["stackTrace"].as_str().is_some(), "{sample:?}");
            let dropped = sample["dropped"].as_f64();
            ensure!(
                sample["truncated"].as_bool() == Some(false) && dropped.is_some_and(|dropped| dropped >= 0.0 && dropped.fract() == 0.0),
                "{sample:?}"
            );
        }
        "bounds" => {
            ev(first, "console.log('x'.repeat(65535)+'😀tail'); true")?;
            must_wait(|| events.borrow().iter().any(|event| event["truncated"].as_bool() == Some(true)), "a bounded message")?;
            let events = events.borrow();
            let truncated = events.iter().find(|event| event["truncated"].as_bool() == Some(true)).unwrap();
            ensure!(truncated["message"].as_str() == Some("x".repeat(65535).as_str()), "the bounded message");
        }
        "frame" => {
            ev(first, "console.log('main'); true")?;
            must_wait(|| !events.borrow().is_empty(), "the main frame's message")?;
            load(first, base, "/frame")?;
            must_wait(|| messages(events).iter().any(|message| message == "cross-site child"), "the other site's frame's message")?;
            let events = events.borrow();
            let child = events.iter().find(|event| event["message"].as_str() == Some("cross-site child")).unwrap();
            let origin = base.replace("127.0.0.1", "localhost");
            ensure!(child["mainFrame"].as_bool() == Some(false) && child["origin"].as_str() == Some(origin.as_str()), "{child:?}");
            ensure!(child["frameId"] != events[0]["frameId"], "{child:?}");
        }
        "isolation" => {
            ev(other, "console.warn('other-view'); true")?;
            ev(first, "console.warn('this-view'); true")?;
            must_wait(|| messages(events).iter().any(|message| message == "this-view"), "this view's message")?;
            ensure!(!messages(events).iter().any(|message| message == "other-view"), "{:?}", messages(events));
            ev(first, "console.warn('still'); true")?;
            must_wait(|| messages(events).iter().any(|message| message == "still"), "the function still there")?;
        }
        "replace" => {
            let swapped: Events = Rc::default();
            let (view, into) = (first.clone(), swapped.clone());
            first.on_console(move |_: &str| view.on_console(recording(&into)));
            ev(first, "console.log('swap-now'); true")?;
            ev(first, "console.log('after-swap'); true")?;
            must_wait(|| !swapped.borrow().is_empty(), "the replacement")?;
            ensure!(messages(&swapped) == ["after-swap"], "{:?}", swapped.borrow());
        }
        "remove" => {
            let (view, into) = (first.clone(), events.clone());
            first.on_console(move |json: &str| {
                into.borrow_mut().push(Value::parse(json).unwrap_or_default());
                view.clear_console_handler();
            });
            ev(first, "console.warn('once'); true")?;
            must_wait(|| !events.borrow().is_empty(), "the message")?;
            ev(first, "console.log('after'); true")?;
            settle();
            ensure!(messages(events) == ["once"], "{:?}", messages(events));
        }
        "close" => {
            let view = first.clone();
            first.on_console(move |_: &str| view.close());
            first.evaluate("console.log('close-now'); true", |_| {});
            must_wait(|| *closed.borrow(), "closed from the function")?;
            // A closed view takes no function: it is closed, and setting one does nothing.
            first.on_console(|_: &str| {});
            ensure!(first.is_closed(), "a closed view took a function");
        }
        _ => return Err(format!("no such mode: {mode}")),
    }
    Ok(())
}

fn main() {
    let base = std::env::var("ZEBKIT_CONSOLE_BASE").expect("ZEBKIT_CONSOLE_BASE");
    let case = std::env::var("ZEBKIT_CONSOLE_CASE").expect("ZEBKIT_CONSOLE_CASE");
    let mut args: Vec<String> = std::env::args().collect();
    args[0] = "console-rust".into();
    if let Err(error) = zebkit::init_with_settings(args, r#"{"logging":{"destination":"none"}}"#) {
        println!("FAIL zebkit::init(): {error}");
        std::process::exit(1);
    }
    let (first, other) = (View::new(None, 0, 0), View::new(None, 0, 0));
    let closed = Rc::new(RefCell::new(false));
    let signal = closed.clone();
    first.on_close(move || *signal.borrow_mut() = true);
    let events: Events = Rc::default();
    let result = load(&first, &base, "/initial")
        .and_then(|()| load(&other, &base, "/other"))
        .and_then(|()| mode(&case, &base, &first, &other, &events, &closed));
    other.close();
    if !*closed.borrow() {
        first.close();
    }
    let closes = wait(|| *closed.borrow(), 10);
    zebkit::shutdown();
    match (result, closes) {
        (Ok(()), true) => println!("ok Rust console {case}"),
        (Err(error), _) => {
            println!("FAIL console {case}: {error}");
            std::process::exit(1);
        }
        (Ok(()), false) => {
            println!("FAIL the views close");
            std::process::exit(1);
        }
    }
}
