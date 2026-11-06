//! Runtime settings through the Rust binding; run by tests/engine/runtime_settings.py.
use std::cell::RefCell;
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::{LoadEvent, View};

fn wait(what: &str, test: impl Fn() -> bool) {
    let end = Instant::now() + Duration::from_secs(15);
    while !test() {
        assert!(Instant::now() < end, "timed out: {what}");
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
}
fn ev(view: &View, script: &str) -> String {
    let result: Rc<RefCell<Option<String>>> = Rc::new(RefCell::new(None));
    let slot = result.clone();
    view.evaluate(script, move |value| *slot.borrow_mut() = Some(value.unwrap_or_else(|error| format!("error:{error:?}"))));
    wait(script, || result.borrow().is_some());
    let value = result.borrow().clone().unwrap();
    value
}
fn opened(url: &str) -> View {
    let view = View::new(None, 640, 480);
    let done = Rc::new(std::cell::Cell::new(false));
    let flag = done.clone();
    view.on_load(move |phase, _| if matches!(phase, LoadEvent::Finished) { flag.set(true) });
    view.load_url(url);
    wait(url, || done.get());
    view
}
fn cookies_become(view: &View, expected: &str, name: &mut u32) {
    for _ in 0..60 {
        *name += 1;
        if ev(view, &format!("probe('c{name}')")) == expected {
            return;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    panic!("third-party cookies never became {expected}");
}

fn main() {
    let base = std::env::var("RUNTIME_BASE").expect("RUNTIME_BASE");
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#).expect("initialize");
    let top = format!("{base}/top");
    let (view, other) = (opened(&top), opened(&top));
    let mut name = 0;

    let start = zebkit::engine_settings().expect("settings");
    assert!(start.contains(r#""explicit":{}"#) && start.contains(r#""thirdPartyCookies":"block""#), "{start}");

    // A bad batch changes nothing.
    view.set_runtime_settings(r#"{"dialogs":"accept"}"#).expect("accept");
    let (engine_before, view_before) = (zebkit::engine_settings(), view.runtime_settings());
    for bad in ["not json", "[]", r#"{"nothing":1}"#, r#"{"thirdPartyCookies":true}"#, r#"{"downloads":"deny","newWindows":"nope"}"#] {
        assert!(zebkit::set_engine_settings(bad).is_err(), "{bad}");
        assert!(view.set_runtime_settings(bad).is_err(), "{bad}");
    }
    assert!(view.set_runtime_settings(r#"{"spareRenderer":true}"#).is_err());
    assert_eq!((zebkit::engine_settings(), view.runtime_settings()), (engine_before, view_before));
    view.set_runtime_settings(r#"{"dialogs":null}"#).expect("reset");

    cookies_become(&view, "\"none\"", &mut name);
    zebkit::set_engine_settings(r#"{"thirdPartyCookies":"allow"}"#).expect("allow");
    cookies_become(&view, "\"cookie\"", &mut name);
    zebkit::set_engine_settings(r#"{"thirdPartyCookies":"block"}"#).expect("block");
    cookies_become(&view, "\"none\"", &mut name);
    zebkit::set_engine_settings(r#"{"thirdPartyCookies":null}"#).expect("reset");

    view.set_runtime_settings(r#"{"dialogs":"dismiss"}"#).expect("dismiss");
    other.set_runtime_settings(r#"{"dialogs":"accept"}"#).expect("accept");
    assert_eq!(ev(&view, "confirm('q')"), "false");
    assert_eq!(ev(&other, "confirm('q')"), "true");
    view.set_runtime_settings(r#"{"dialogs":null}"#).expect("reset");
    other.set_runtime_settings(r#"{"dialogs":null}"#).expect("reset");

    let told: Rc<RefCell<Vec<String>>> = Rc::new(RefCell::new(Vec::new()));
    let log = told.clone();
    view.on_download_progress(move |json| log.borrow_mut().push(json.to_string()));
    let outcome = |file: &str, state: &str| {
        let found = |line: &String| line.contains(&format!("name={file}")) && line.contains(r#""done":true"#);
        wait(file, || told.borrow().iter().any(found));
        assert!(told.borrow().iter().any(|line| found(line) && line.contains(state)), "{file}: {:?}", told.borrow());
    };
    view.set_runtime_settings(r#"{"downloads":"deny"}"#).expect("deny");
    ev(&view, "save('denied.bin'); 1");
    outcome("denied.bin", r#""state":"cancelled""#);
    view.set_runtime_settings(r#"{"downloads":null}"#).expect("reset");
    ev(&view, "save('saved.bin'); 1");
    outcome("saved.bin", r#""state":"complete""#);

    view.set_runtime_settings(r#"{"newWindows":"block"}"#).expect("block");
    other.set_runtime_settings(r#"{"newWindows":"allow"}"#).expect("allow");
    assert_eq!(ev(&view, "opens()"), "false");
    assert_eq!(ev(&other, "opens()"), "true");
    view.set_runtime_settings(r#"{"newWindows":null}"#).expect("reset");
    zebkit::set_engine_settings(r#"{"newWindows":"block"}"#).expect("block");
    assert_eq!(ev(&view, "opens()"), "false");
    zebkit::set_engine_settings(r#"{"newWindows":null}"#).expect("reset");
    assert_eq!(ev(&view, "opens()"), "true");

    zebkit::set_engine_settings(r#"{"spareRenderer":true}"#).expect("spare");
    assert!(zebkit::engine_settings().unwrap().contains(r#""spareRendererRunning":true"#));
    zebkit::set_engine_settings(r#"{"spareRenderer":false}"#).expect("no spare");
    assert!(zebkit::engine_settings().unwrap().contains(r#""spareRendererRunning":false"#));
    zebkit::set_engine_settings(r#"{"spareRenderer":null}"#).expect("reset");

    view.close();
    other.close();
    println!("ok rust");
    zebkit::shutdown();
}
