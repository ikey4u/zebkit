//! HTTP authentication through `View::on_http_auth` and `HttpAuthRequest`, one
//! mode an engine; run by tests/engine/http_auth.py, which has the modes and the
//! server: ZEBKIT_HTTP_AUTH_URL, _USER, _PASSWORD, _CASE (the mode), _REALM,
//! _PORT and, for a proxy, _PROXY=1. The engine's switches are the arguments.
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::json::Value;
use zebkit::{HttpAuthRequest, LoadEvent, View};

/// Modes whose function holds the request for main() to settle.
const HOLD: &[&str] = &["invalid", "timeout", "stop", "navigation", "remove", "replace"];

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

/// Pumps the engine until `predicate` holds or 20 s have passed.
fn wait(predicate: impl Fn() -> bool) -> bool {
    let deadline = Instant::now() + Duration::from_secs(20);
    while !predicate() && Instant::now() < deadline {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
    predicate()
}

fn must_wait(predicate: impl Fn() -> bool, what: &str) -> bool {
    let ok = wait(predicate);
    check(ok, what, "it did not happen within 20 s");
    ok
}

/// The page's value of `script`, or Null when it threw.
fn evaluate(view: &View, script: &str) -> Value {
    let answer: Rc<RefCell<Option<Value>>> = Rc::new(RefCell::new(None));
    let slot = answer.clone();
    view.evaluate(script, move |result| {
        *slot.borrow_mut() = Some(result.ok().and_then(|json| Value::parse(&json).ok()).unwrap_or(Value::Null));
    });
    wait(|| answer.borrow().is_some());
    let value = answer.borrow_mut().take().unwrap_or(Value::Null);
    value
}

struct State {
    requests: RefCell<Vec<HttpAuthRequest>>,
    outcomes: RefCell<Vec<Value>>,
    failed: Cell<bool>,
    closed: Cell<bool>,
}

fn metadata(request: &HttpAuthRequest) -> Value {
    Value::parse(request.metadata()).unwrap_or(Value::Null)
}

fn main() {
    let variable = |name: &str| std::env::var(name).unwrap_or_else(|_| panic!("{name} is the driver's"));
    let base = variable("ZEBKIT_HTTP_AUTH_URL");
    let username = variable("ZEBKIT_HTTP_AUTH_USER");
    let password = variable("ZEBKIT_HTTP_AUTH_PASSWORD");
    let mode = variable("ZEBKIT_HTTP_AUTH_CASE");
    let realm = variable("ZEBKIT_HTTP_AUTH_REALM");
    let port: i64 = variable("ZEBKIT_HTTP_AUTH_PORT").parse().expect("the challenger's port");
    let proxy = std::env::var("ZEBKIT_HTTP_AUTH_PROXY").as_deref() == Ok("1");
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#).expect("initialize");

    let view = View::new(None, 320, 200);
    let state = Rc::new(State {
        requests: RefCell::new(Vec::new()),
        outcomes: RefCell::new(Vec::new()),
        failed: Cell::new(false),
        closed: Cell::new(false),
    });
    let seen = state.clone();
    view.on_load(move |event, _| {
        if event == LoadEvent::Failed {
            seen.failed.set(true);
        }
    });
    let seen = state.clone();
    view.on_close(move || seen.closed.set(true));
    check(!view.set_http_auth_timeout(0) && !view.set_http_auth_timeout(300001),
          "set_http_auth_timeout refuses 0 and 300001", ());
    check(view.set_http_auth_timeout(if mode == "timeout" { 50 } else { 5000 }), "set_http_auth_timeout", ());

    let (seen, owner, behavior) = (state.clone(), view.clone(), mode.clone());
    let (user, secret) = (username.clone(), password.clone());
    let challenge = move |request: HttpAuthRequest| {
        seen.requests.borrow_mut().push(request.clone());
        let fields = metadata(&request);
        let first = seen.requests.borrow().len() == 1;
        if first {
            check(request.info().as_deref() == Some(request.metadata()), "the request: its info is its metadata",
                  request.metadata());
            check(fields["realm"].as_str() == Some(realm.as_str()) && fields["scheme"].as_str() == Some("basic")
                      && fields["proxy"].as_bool() == Some(proxy) && fields["port"].as_i64() == Some(port),
                  "the metadata: realm, scheme, proxy and the challenger's port", request.metadata());
            check(!request.metadata().contains(&user) && !request.metadata().contains(&secret),
                  "no credentials in the metadata", ());
            check(request.provide("bad\0", "x").is_err() && request.provide("user", "bad\0").is_err()
                      && request.provide(&"x".repeat(65537), "x").is_err()
                      && request.provide("x", &"x".repeat(65537)).is_err()
                      && request.info().is_some(),
                  "invalid credentials are refused, and the request stays", ());
        }
        if HOLD.contains(&behavior.as_str()) {
            return;
        }
        match behavior.as_str() {
            "cancel-handler" => {
                owner.clear_http_auth_handler();
                check(request.info().is_none() && !request.provide(&user, &secret).unwrap_or(true),
                      "removed from inside it: the request is gone", ());
            }
            "close-request" => owner.close(),
            "deny" => check(request.cancel(), "cancelled with no credentials", ()),
            _ => {
                let wrong = behavior == "retry" && fields["firstAttempt"].as_bool() == Some(true);
                let (name, word) = if wrong { ("wrong-user", "wrong-password") } else { (user.as_str(), secret.as_str()) };
                let ok = request.provide(name, word).unwrap_or(false);
                if first || behavior != "retry" {
                    check(ok && request.info().is_none() && !request.cancel(),
                          "answered once: the request is gone, a second reply refused", ());
                }
            }
        }
    };
    let (seen, owner, behavior, secret) = (state.clone(), view.clone(), mode.clone(), password.clone());
    let result = move |json: &str| {
        check(!json.contains(&secret), "no password in the outcome", ());
        check(!json.contains("\"provided\":"), "the outcome has no provided field (state says it)", json);
        seen.outcomes.borrow_mut().push(Value::parse(json).unwrap_or(Value::Null));
        if behavior == "close-result" {
            owner.clear_http_auth_result_handler();
            owner.close();
        }
    };
    if mode != "default" {
        view.on_http_auth(challenge);
    }
    view.on_http_auth_result(result);
    let path = match mode.as_str() {
        "fetch" => "/public".to_string(),
        "frame" => "/public-frame".to_string(),
        _ => format!("/auth/rust-{mode}"),
    };
    view.load_url(&format!("{base}{path}"));

    let title = |view: &View| view.title().unwrap_or_default();
    let denied = || state.failed.get() || title(&view) == "auth-denied";
    let states = || state.outcomes.borrow().iter().map(|o| o["state"].as_str().unwrap_or("").to_string()).collect::<Vec<_>>();
    let any_request = |test: &dyn Fn(&Value) -> bool| state.requests.borrow().iter().any(|r| test(&metadata(r)));
    match mode.as_str() {
        "close-request" | "close-result" => {
            must_wait(|| state.closed.get(), "the view closed from the callback");
        }
        "default" | "deny" | "cancel-handler" => {
            must_wait(denied, "the challenge cancelled");
            if mode == "default" {
                check(state.requests.borrow().is_empty(), "no function: nothing asked", ());
            } else {
                must_wait(|| !state.outcomes.borrow().is_empty(), "the outcome");
                check(states().first().map(String::as_str) == Some("cancelled"), "the outcome: cancelled", states());
            }
        }
        "allow" | "retry" | "proxy" => {
            must_wait(|| title(&view) == "auth-ok" && !state.outcomes.borrow().is_empty(), "authenticated");
            check(states().last().map(String::as_str) == Some("provided"), "the outcome: provided", states());
            if mode == "retry" {
                check(any_request(&|m| m["firstAttempt"].as_bool() == Some(false)), "retry: asked again", ());
            }
            if mode == "proxy" {
                check(any_request(&|m| {
                    m["proxy"].as_bool() == Some(true) && m["mainFrame"].as_bool() == Some(true)
                        && m["navigation"].as_bool() == Some(true)
                }), "proxy: the main frame's navigation", ());
            }
        }
        "fetch" => {
            must_wait(|| title(&view) == "public", "the public page");
            let answer = evaluate(&view, "fetch('/auth/fetch').then(r => r.text())");
            check(answer.as_str().is_some_and(|text| text.contains("auth-ok")), "fetch: authenticated", &answer);
            check(any_request(&|m| m["navigation"].as_bool() == Some(false) && m["mainFrame"].as_bool() == Some(false)),
                  "fetch: a request that is not a navigation", ());
        }
        "frame" => {
            must_wait(|| title(&view) == "public", "the public page");
            must_wait(|| evaluate(&view, "childAuthenticated").as_bool() == Some(true), "the frame authenticated");
            check(any_request(&|m| m["navigation"].as_bool() == Some(true) && m["mainFrame"].as_bool() == Some(false)),
                  "frame: a subframe's navigation", ());
        }
        _ => {
            if must_wait(|| !state.requests.borrow().is_empty(), "the request, held") {
                let request = state.requests.borrow()[0].clone();
                let id = metadata(&request)["id"].clone();
                match mode.as_str() {
                    "invalid" => {
                        check(request.provide("bad\0", "x").is_err() && request.info().is_some(),
                              "an invalid reply later: refused, the request stays", ());
                        check(request.provide(&username, &password).unwrap_or(false), "then a valid reply", ());
                        must_wait(|| title(&view) == "auth-ok", "a valid reply after invalid ones");
                    }
                    "timeout" => {
                        must_wait(|| request.info().is_none(), "the deadline");
                        check(!request.provide(&username, &password).unwrap_or(true), "no reply after it", ());
                        must_wait(|| state.outcomes.borrow().iter().any(|o| o["id"] == id && o["reason"].as_str() == Some("timeout")),
                                  "the timeout outcome");
                    }
                    _ => {
                        match mode.as_str() {
                            "stop" => view.stop(),
                            "navigation" => view.load_url("about:blank"),
                            "remove" => view.clear_http_auth_handler(),
                            "replace" => view.on_http_auth(|_| {}),
                            other => panic!("unknown mode {other}"),
                        }
                        must_wait(|| request.info().is_none(), "the request cancelled");
                        if mode == "remove" || mode == "replace" {
                            must_wait(|| state.outcomes.borrow().iter().any(|o| o["reason"].as_str() == Some("handler-changed")),
                                      "the handler-changed outcome");
                        }
                    }
                }
            }
        }
    }
    check(state.requests.borrow().iter().all(|r| r.info().is_none() && !r.cancel()),
          &format!("rust {mode}: every request settled, none answerable"), ());
    if !state.closed.get() {
        view.close();
        check(wait(|| state.closed.get()), "the view closes", ());
    }
    assert!(zebkit::shutdown());
    let (passed, failed) = (PASSED.with(Cell::get), FAILED.with(Cell::get));
    println!("{passed} of {} checks passed", passed + failed);
    if failed != 0 {
        std::process::exit(1);
    }
    println!("ok Rust HTTP auth {mode}");
}
