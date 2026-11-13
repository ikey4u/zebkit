//! A document's TLS errors through `View::on_certificate` and
//! `CertificateRequest`, one mode an engine; run by tests/engine/certificate.py,
//! which has the modes and the HTTPS server with a certificate nobody trusts:
//! ZEBKIT_CERTIFICATE_TEST_URL (the origin) and ZEBKIT_CERTIFICATE_CASE (the
//! mode). The engine's switches are the arguments.
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::json::Value;
use zebkit::{CertificateRequest, LoadEvent, View};

/// Modes whose function holds the request for main() to settle.
const HOLD: &[&str] = &["hold", "timeout", "navigation", "stop"];

thread_local! {
    static PASSED: Cell<u32> = const { Cell::new(0) };
    static FAILED: Cell<u32> = const { Cell::new(0) };
}

fn check(ok: bool, what: &str, detail: impl std::fmt::Debug) {
    if ok {
        PASSED.with(|passed| passed.set(passed.get() + 1));
        println!("ok    {what}");
    } else {
        FAILED.with(|failed| failed.set(failed.get() + 1));
        println!("FAIL  {what}: {detail:?}");
    }
}

fn wait(predicate: impl Fn() -> bool, seconds: u64) -> bool {
    let deadline = Instant::now() + Duration::from_secs(seconds);
    while !predicate() && Instant::now() < deadline {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
    predicate()
}

fn must_wait(predicate: impl Fn() -> bool, what: &str) -> bool {
    let ok = wait(predicate, 20);
    check(ok, what, "it did not happen within 20 s");
    ok
}

fn metadata(request: &CertificateRequest) -> Value {
    Value::parse(request.metadata()).unwrap_or(Value::Null)
}

/// A view with what its loads, requests, outcomes and closing were.
#[derive(Clone)]
struct Watched {
    view: View,
    loads: Rc<RefCell<Vec<(LoadEvent, String)>>>,
    requests: Rc<RefCell<Vec<CertificateRequest>>>,
    outcomes: Rc<RefCell<Vec<Value>>>,
    closed: Rc<Cell<bool>>,
}

impl Watched {
    fn new() -> Watched {
        let watched = Watched {
            view: View::new(None, 320, 200),
            loads: Rc::default(),
            requests: Rc::default(),
            outcomes: Rc::default(),
            closed: Rc::default(),
        };
        let loads = watched.loads.clone();
        watched.view.on_load(move |event, url| loads.borrow_mut().push((event, url.to_string())));
        let closed = watched.closed.clone();
        watched.view.on_close(move || closed.set(true));
        watched
    }
    fn loaded(&self, event: LoadEvent, url: &str) -> bool {
        self.loads.borrow().iter().any(|(seen, at)| *seen == event && at == url)
    }
    fn outcome(&self, test: impl Fn(&Value) -> bool) -> bool {
        self.outcomes.borrow().iter().any(test)
    }
}

fn main() {
    let base = std::env::var("ZEBKIT_CERTIFICATE_TEST_URL").expect("local TLS fixture URL");
    let mode = std::env::var("ZEBKIT_CERTIFICATE_CASE").expect("the mode");
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#)
        .expect("initialize engine");
    let (first, other) = (Watched::new(), Watched::new());
    let view = first.view.clone();
    check(!view.set_certificate_timeout(0) && !view.set_certificate_timeout(300001),
          "set_certificate_timeout refuses 0 and 300001", ());
    check(view.set_certificate_timeout(if mode == "timeout" { 200 } else { 5000 }), "set_certificate_timeout", ());

    let (watched, behavior) = (first.clone(), mode.clone());
    let certificate = move |request: CertificateRequest| {
        watched.requests.borrow_mut().push(request.clone());
        let fields = metadata(&request);
        let count = watched.requests.borrow().len();
        if count == 1 {
            check(request.info().as_deref() == Some(request.metadata()), "the request: its info is its metadata",
                  request.metadata());
            let number = |key: &str| fields[key].as_f64().unwrap_or(f64::NAN);
            check(fields["state"].as_str() == Some("pending") && number("error") < 0.0
                      && fields["certificateChain"][0].as_str().is_some_and(|pem| pem.starts_with("-----BEGIN CERTIFICATE-----"))
                      && fields["subject"].as_str() == Some("localhost") && fields["issuer"].as_str() == Some("localhost")
                      && number("validFromUnixMs") < number("validUntilUnixMs")
                      && fields["knownRoot"].as_bool() == Some(false) && fields["chainTruncated"].as_bool() == Some(false),
                  "the metadata: the error, the chain, its names and dates", request.metadata());
        }
        if HOLD.contains(&behavior.as_str()) {
            return;
        }
        let view = &watched.view;
        match behavior.as_str() {
            "strict" => {
                check(fields["strictEnforcement"].as_bool() == Some(true) && fields["canIgnore"].as_bool() == Some(false),
                      "a pinned host: strict", request.metadata());
                check(!request.respond(true) && request.info().is_some() && request.respond(false),
                      "its error cannot be allowed, only refused", ());
            }
            "cancel-handler" => {
                view.clear_certificate_handler();
                check(request.info().is_none() && !request.respond(true), "removed from inside it: the request is gone", ());
            }
            "close-request" => view.close(),
            _ if behavior == "allow" || (behavior == "cache" && count == 1) => {
                check(fields["canIgnore"].as_bool() == Some(true) && fields["strictEnforcement"].as_bool() == Some(false),
                      "an error that can be allowed", request.metadata());
                check(request.respond(true) && !request.respond(false) && request.info().is_none(),
                      "allowed once: the request is gone, a second reply refused", ());
            }
            _ => check(request.respond(false) && !request.respond(true) && request.info().is_none(),
                       "refused once: the request is gone, a second reply refused", ()),
        }
    };
    let (watched, behavior) = (first.clone(), mode.clone());
    let result = move |json: &str| {
        watched.outcomes.borrow_mut().push(Value::parse(json).unwrap_or(Value::Null));
        if behavior == "close-result" {
            watched.view.clear_certificate_result_handler();
            watched.view.close();
        }
    };
    if mode != "default" {
        view.on_certificate(certificate);
    }
    view.on_certificate_result(result);
    let url = format!("{base}/rust-{mode}");
    match mode.as_str() {
        "post" => {
            view.load_url(&format!(
                "data:text/html,<form method='post' action='{url}'><input name='value' value='private'></form>\
                 <script>document.forms[0].submit()</script>"
            ));
            must_wait(|| first.requests.borrow().iter().any(|r| metadata(r)["url"].as_str() == Some(url.as_str())),
                      "the POST asked for");
            must_wait(|| first.loaded(LoadEvent::Failed, &url), "the POST refused");
        }
        "cache" => {
            view.load_url(&url);
            must_wait(|| first.loaded(LoadEvent::Finished, &url) && view.title().as_deref() == Some("certificate-ok"),
                      "the allowed, cached page");
            // Another view, its own function refusing: asked again, nothing sent.
            let asked: Rc<RefCell<Vec<String>>> = Rc::default();
            let kept = asked.clone();
            other.view.on_certificate(move |request| {
                kept.borrow_mut().push(metadata(&request)["url"].as_str().unwrap_or("").to_string());
                request.respond(false);
            });
            other.view.load_url(&url);
            must_wait(|| other.loaded(LoadEvent::Failed, &url), "the other view refused");
            check(*asked.borrow() == [url.clone()], "the other view was asked once, for the page", asked.borrow());
        }
        _ => {
            view.load_url(&url);
            if mode == "close-request" || mode == "close-result" {
                must_wait(|| first.closed.get(), "the view closed");
                check(mode == "close-request"
                          || first.outcomes.borrow().first().is_some_and(|o| o["reason"].as_str() == Some("answered")),
                      "the outcome before the close: answered", first.outcomes.borrow());
            } else if mode == "allow" {
                must_wait(|| view.title().as_deref() == Some("certificate-ok"), "the allowed page");
                must_wait(|| first.outcome(|o| o["state"].as_str() == Some("continued")), "the continued outcome");
                check(first.requests.borrow().first().is_some_and(|r| metadata(r)["url"].as_str() == Some(url.as_str())),
                      "the request's url", ());
                // Once only: another view of the profile is refused, without being asked.
                let other_url = format!("{url}-other");
                other.view.load_url(&other_url);
                must_wait(|| other.loaded(LoadEvent::Failed, &other_url), "the other view refused");
                check(first.requests.borrow().len() == 1, "the other view was not asked", first.requests.borrow().len());
            } else if HOLD.contains(&mode.as_str()) {
                if must_wait(|| !first.requests.borrow().is_empty(), "the request, held") {
                    let request = first.requests.borrow()[0].clone();
                    let id = metadata(&request)["id"].clone();
                    if mode == "hold" {
                        // A request is the engine thread's alone (not Send): no
                        // other thread can answer it. Answered later, here.
                        check(request.info().is_some() && request.respond(false), "answered later: refused", ());
                        must_wait(|| first.outcome(|o| o["id"] == id && o["state"].as_str() == Some("denied")),
                                  "the later refusal");
                    } else {
                        match mode.as_str() {
                            "navigation" => view.load_url("about:blank"),
                            "stop" => view.stop(),
                            _ => {}
                        }
                        must_wait(|| request.info().is_none(), "the request ended");
                        check(!request.respond(true), "no reply after it", ());
                        if mode == "timeout" {
                            must_wait(|| first.outcome(|o| o["id"] == id && o["reason"].as_str() == Some("timeout")),
                                      "the timeout outcome");
                        }
                    }
                }
            } else {
                check(wait(|| first.loaded(LoadEvent::Failed, &url), 30), "the page refused", ());
                match mode.as_str() {
                    "default" => check(first.requests.borrow().is_empty(), "no function: nothing asked", ()),
                    "deny" => {
                        must_wait(|| first.outcome(|o| o["reason"].as_str() == Some("answered")), "the outcome");
                    }
                    "cancel-handler" => {
                        must_wait(|| first.outcome(|o| o["reason"].as_str() == Some("handler-changed")),
                                  "the handler-changed outcome");
                    }
                    "strict" => {}
                    other => panic!("unknown mode {other}"),
                }
            }
        }
    }
    check(first.requests.borrow().iter().all(|r| r.info().is_none() && !r.respond(true)),
          &format!("rust {mode}: every request settled, none answerable"), ());
    for watched in [&first, &other] {
        if !watched.closed.get() {
            watched.view.close();
        }
    }
    check(wait(|| first.closed.get() && other.closed.get(), 20), "the views close", ());
    assert!(zebkit::shutdown());
    let (passed, failed) = (PASSED.with(Cell::get), FAILED.with(Cell::get));
    println!("{passed} of {} checks passed", passed + failed);
    if failed != 0 {
        std::process::exit(1);
    }
    println!("ok Rust certificate policy {mode}");
}
