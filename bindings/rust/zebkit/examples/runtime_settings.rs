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
/// For runtime_settings.py, which listens to the local network: when a part began
/// and ended, in milliseconds since the epoch.
fn mark(what: &str) {
    use std::io::Write;
    let Ok(path) = std::env::var("RUNTIME_MARKS") else { return };
    let now = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis();
    let mut file = std::fs::OpenOptions::new().create(true).append(true).open(path).expect("marks");
    writeln!(file, "{what} {now}").expect("marks");
}
fn pump_for(ms: u64) {
    let end = Instant::now() + Duration::from_millis(ms);
    while Instant::now() < end {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
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

    // WebRTC's IP handling policy: what a page's scripts can learn of the machine's
    // addresses, the engine's for every view, one view's own before it.
    let policy = r#""webrtcIpHandlingPolicy":"#;
    // Unset: Chromium's default. The quiet part first: a page that has gathered under
    // "default" goes on announcing its names for seconds after, which no setting takes back.
    assert!(zebkit::engine_settings().unwrap().contains(&format!(r#"{policy}"default""#)));
    mark("quiet-begin");
    zebkit::set_engine_settings(r#"{"webrtcIpHandlingPolicy":"disable_non_proxied_udp"}"#).expect("engine");
    assert_eq!(ev(&view, "rtc()"), "\"none\"", "a page opened before the write");
    assert_eq!(ev(&other, "rtc()"), "\"none\"");
    pump_for(3000);
    mark("quiet-end");
    mark("loud-begin");
    view.set_runtime_settings(r#"{"webrtcIpHandlingPolicy":"default"}"#).expect("view");
    assert_eq!(ev(&view, "rtc()"), "\"host\"", "a view's own goes first");
    pump_for(1500);
    mark("loud-end");
    assert_eq!(ev(&other, "rtc()"), "\"none\"", "the other keeps the engine's");
    assert!(view.runtime_settings().unwrap().contains(&format!(r#"{policy}"default""#)));
    view.load_url(&format!("{base}/top?again"));
    wait("the navigation", || ev(&view, "location.search") == "\"?again\"");
    assert_eq!(ev(&view, "rtc()"), "\"host\"", "a navigation keeps it");
    let (engine_at, view_at) = (zebkit::engine_settings(), view.runtime_settings());
    for bad in [r#"{"webrtcIpHandlingPolicy":"public"}"#, r#"{"dialogs":"accept","webrtcIpHandlingPolicy":5}"#] {
        assert!(zebkit::set_engine_settings(bad).is_err() && view.set_runtime_settings(bad).is_err(), "{bad}");
    }
    assert_eq!((zebkit::engine_settings(), view.runtime_settings()), (engine_at, view_at));
    view.set_runtime_settings(r#"{"webrtcIpHandlingPolicy":null}"#).expect("view null");
    assert_eq!(ev(&view, "rtc()"), "\"none\"", "null: the engine's");
    zebkit::set_engine_settings(r#"{"webrtcIpHandlingPolicy":null}"#).expect("engine null");
    assert_eq!(ev(&view, "rtc()"), "\"host\"", "null: the default");
    assert_eq!(ev(&other, "rtc()"), "\"host\"");

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
