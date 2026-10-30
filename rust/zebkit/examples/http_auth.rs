//! Set ZEBKIT_HTTP_AUTH_URL/USER/PASSWORD for a private test server.
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::{HttpAuthRequest, LoadEvent, View};

fn wait(predicate: impl Fn() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !predicate() && Instant::now() < deadline {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
    assert!(predicate(), "HTTP authentication did not complete");
}
fn main() {
    let base = std::env::var("ZEBKIT_HTTP_AUTH_URL").expect("local HTTP fixture URL");
    let username = std::env::var("ZEBKIT_HTTP_AUTH_USER").expect("fixture username");
    let password = std::env::var("ZEBKIT_HTTP_AUTH_PASSWORD").expect("fixture password");
    let mode = std::env::var("ZEBKIT_HTTP_AUTH_CASE").unwrap_or_else(|_| "allow".into());
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#).expect("initialize");
    let view = View::new(None, 320, 200);
    assert!(!view.set_http_auth_timeout(0));
    assert!(view.set_http_auth_timeout(5000));
    let closed = Rc::new(Cell::new(false));
    let signal = closed.clone();
    view.on_close(move || signal.set(true));
    let failed = Rc::new(Cell::new(false));
    let signal = failed.clone();
    view.on_load(move |phase, _| {
        if phase == LoadEvent::Failed {
            signal.set(true)
        }
    });
    let kept: Rc<RefCell<Option<HttpAuthRequest>>> = Rc::new(RefCell::new(None));
    let pending = kept.clone();
    let owner = view.clone();
    let behavior = mode.clone();
    view.on_http_auth(move |request| {
        assert_eq!(request.info().as_deref(), Some(request.metadata()));
        assert!(!request.metadata().contains(&password));
        assert!(request.provide("bad\0", "x").is_err());
        assert!(request.provide(&"x".repeat(65537), "x").is_err());
        assert!(request.info().is_some());
        *pending.borrow_mut() = Some(request.clone());
        if behavior == "cancel-handler" {
            owner.clear_http_auth_handler();
            assert!(request.info().is_none());
            assert!(!request.cancel());
        } else if behavior == "close-request" {
            owner.close();
        } else {
            assert!(request.provide(&username, &password).expect("valid credentials"));
            assert!(!request.cancel());
            assert!(request.info().is_none());
            owner.clear_http_auth_handler();
        }
    });
    let received = Rc::new(Cell::new(false));
    let signal = received.clone();
    let owner = view.clone();
    let behavior = mode.clone();
    view.on_http_auth_result(move |metadata| {
        assert!(metadata.contains(if behavior == "cancel-handler" || behavior == "close-request" {
            "\"provided\":false"
        } else {
            "\"provided\":true"
        }));
        owner.clear_http_auth_result_handler();
        signal.set(true);
        if behavior == "close-result" {
            owner.close();
        }
    });
    view.load_url(&format!("{base}/auth/rust-{mode}"));
    if mode == "close-request" || mode == "close-result" {
        wait(|| closed.get());
    } else if mode == "cancel-handler" {
        wait(|| received.get() && (failed.get() || view.title().as_deref() == Some("auth-denied")));
    } else {
        wait(|| received.get() && view.title().as_deref() == Some("auth-ok"));
    }
    let request = kept.borrow().as_ref().expect("HTTP challenge").clone();
    assert!(request.info().is_none());
    assert!(!request.cancel());
    view.close();
    wait(|| closed.get());
    assert!(zebkit::shutdown());
    println!("ok Rust HTTP auth {mode}");
}
