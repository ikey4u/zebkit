//! Set ZEBKIT_UA_TEST_URL to a private loopback page.
use std::cell::Cell;
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::{LoadEvent, View};

fn wait(predicate: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !predicate() && Instant::now() < deadline {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(predicate(), "UA test did not complete");
}
fn verify(view: &View) {
    let done = Rc::new(Cell::new(false));
    let signal = done.clone();
    view.evaluate("navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture']).then(h=>navigator.userAgent==='zebkit-binding-UA/1'&&navigator.platform==='NativePlatform'&&h.architecture==='x86'&&h.platformVersion==='1')", move |result| {
        assert_eq!(result.expect("UA state"), "true");
        signal.set(true);
    });
    wait(|| done.get());
}
fn main() {
    let url = std::env::var("ZEBKIT_UA_TEST_URL").expect("local fixture URL");
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#).expect("initialize");
    let view = View::new(None, 320, 200);
    let loaded = Rc::new(Cell::new(false));
    let signal = loaded.clone();
    view.on_load(move |phase, _| {
        if phase == LoadEvent::Finished {
            signal.set(true)
        }
    });
    let done = Rc::new(Cell::new(false));
    let signal = done.clone();
    view.set_user_agent_override(r#"{"userAgent":"zebkit-binding-UA/1","acceptLanguage":"en-US","platform":"NativePlatform","userAgentMetadata":{"brands":[{"brand":"Native","version":"1"}],"platform":"Linux","platformVersion":"1","architecture":"x86","model":"","mobile":false}}"#, move |result| {
        assert_eq!(result.expect("valid UA"), "{}");
        signal.set(true);
    });
    view.load_url(&url);
    wait(|| done.get() && loaded.get());
    verify(&view);
    for invalid in [
        r#"{"userAgent":"bad\r\nagent"}"#,
        r#"{"userAgent":42}"#,
        r#"{"userAgent":"changed","unknown":true}"#,
        "{",
        "literal\0nul",
    ] {
        let done = Rc::new(Cell::new(false));
        let signal = done.clone();
        view.set_user_agent_override(invalid, move |result| {
            assert!(result.is_err());
            signal.set(true);
        });
        wait(|| done.get());
        verify(&view);
    }
    let closed = Rc::new(Cell::new(false));
    let signal = closed.clone();
    view.on_close(move || signal.set(true));
    view.close();
    wait(|| closed.get());
    assert!(zebkit::shutdown());
    println!("ok Rust UA/client hints and atomic rejection");
}
