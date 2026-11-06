//! Document-scoped TLS decisions. Set ZEBKIT_CERTIFICATE_TEST_URL to a local
//! HTTPS fixture with an untrusted certificate; run with --headless --no-sandbox.
use std::cell::Cell;
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::{LoadEvent, View};

fn pump_until(predicate: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !predicate() && Instant::now() < deadline {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(predicate(), "certificate operation did not complete");
}

fn main() {
    let base = std::env::var("ZEBKIT_CERTIFICATE_TEST_URL").expect("local TLS fixture URL");
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#)
        .expect("initialize engine");
    let view = View::new(None, 320, 200);
    assert!(!view.set_certificate_timeout(0));
    assert!(view.set_certificate_timeout(5000));
    let requested = Rc::new(Cell::new(false));
    let received = requested.clone();
    let handler_view = view.clone();
    view.on_certificate(move |request| {
        assert_eq!(request.info().as_deref(), Some(request.metadata()));
        assert!(request.metadata().contains("BEGIN CERTIFICATE"));
        assert!(request.metadata().contains("\"canIgnore\":true"));
        assert!(request.respond(true));
        assert!(!request.respond(false));
        assert!(request.info().is_none());
        received.set(true);
        handler_view.clear_certificate_handler();
    });
    let outcome = Rc::new(Cell::new(false));
    let result = outcome.clone();
    let observer_view = view.clone();
    view.on_certificate_result(move |metadata| {
        assert!(metadata.contains("\"state\":\"continued\""));
        observer_view.clear_certificate_result_handler();
        result.set(true);
    });
    view.load_url(&format!("{base}/rust-allowed"));
    pump_until(|| requested.get() && outcome.get() && view.title().as_deref() == Some("certificate-ok"));
    let other = View::new(None, 320, 200);
    let denied = Rc::new(Cell::new(false));
    let failed = denied.clone();
    other.on_load(move |event, _| { if event == LoadEvent::Failed { failed.set(true); } });
    other.load_url(&format!("{base}/rust-other-denied"));
    pump_until(|| denied.get());
    view.close(); other.close();
    assert!(zebkit::shutdown());
    println!("ok Rust certificate policy: native TLS, single-use reply, callback removal and view isolation");
}
