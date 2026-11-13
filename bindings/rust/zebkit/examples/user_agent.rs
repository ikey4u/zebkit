//! A view's user agent and client hints through `View::set_user_agent_override`
//! and `View::set_user_agent`, one mode an engine; run by tests/engine/user_agent.py,
//! which has the modes and the server: ZEBKIT_UA_TEST_URL (the page loaded
//! first), ZEBKIT_UA_BASE (the server), ZEBKIT_UA_CASE (the mode: hints,
//! rejection, worker, completion, cdp-scope). It prints "ok Rust UA/client
//! hints <mode>" ("... and atomic rejection" for rejection) when every check
//! passes.

use std::cell::{Cell, RefCell};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::rc::Rc;
use std::time::{Duration, Instant};

use zebkit::json::{quote, Value};
use zebkit::{LoadEvent, View};

const AGENT: &str = "zebkit-binding-UA/1";
const VALID: &str = r#"{"userAgent":"zebkit-binding-UA/1","acceptLanguage":"en-US","platform":"NativePlatform","userAgentMetadata":{"brands":[{"brand":"Native","version":"1"}],"platform":"Linux","platformVersion":"1","architecture":"x86","model":"","mobile":false}}"#;
const HINTS: &str = "['architecture','platformVersion','model','bitness','wow64','formFactors','fullVersionList','uaFullVersion']";

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
    if wait(condition, 20) { Ok(()) } else { Err(format!("timed out waiting for {what}")) }
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

macro_rules! ensure {
    ($condition:expr, $($detail:tt)+) => {
        if !$condition {
            return Err(format!($($detail)+));
        }
    };
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

type Answers = Rc<RefCell<Vec<Result<String, String>>>>;

/// A view, what it loaded and whether it closed.
struct Watched {
    view: View,
    loads: Rc<RefCell<Vec<String>>>,
    closed: Rc<Cell<bool>>,
}

impl Watched {
    fn new() -> Watched {
        let view = View::new(None, 320, 200);
        let (loads, closed): (Rc<RefCell<Vec<String>>>, Rc<Cell<bool>>) = Default::default();
        let finished = loads.clone();
        view.on_load(move |event, url| {
            if event == LoadEvent::Finished {
                finished.borrow_mut().push(url.to_string());
            }
        });
        let signal = closed.clone();
        view.on_close(move || signal.set(true));
        Watched { view, loads, closed }
    }

    /// `set_user_agent_override()`: whether it was queued, and its answers;
    /// `then` runs with each.
    fn override_with(&self, options: &str, then: impl FnOnce(&Result<String, String>) + 'static) -> (bool, Answers) {
        let answers: Answers = Rc::default();
        let into = answers.clone();
        let queued = self.view.set_user_agent_override(options, move |result| {
            let result = result.map_err(|error| error.to_string());
            then(&result);
            into.borrow_mut().push(result);
        });
        (queued, answers)
    }

    fn at(&self, url: &str) -> Result<(), String> {
        must_wait(|| self.loads.borrow().iter().any(|loaded| loaded == url), url)
    }
}

/// The override in the page: agent, platform and two high-entropy hints.
fn verify(view: &View) -> Result<(), String> {
    let ok = ev(
        view,
        "navigator.userAgentData.getHighEntropyValues(['platformVersion','architecture']).then(h=>\
         navigator.userAgent==='zebkit-binding-UA/1'&&navigator.platform==='NativePlatform'&&\
         h.architecture==='x86'&&h.platformVersion==='1')",
    )?;
    ensure!(ok == Value::from(true), "the UA state: {ok:?}");
    Ok(())
}

/// The override queued before the first load, which waits for it.
fn first_load(watched: &Watched, page: &str) -> Result<(), String> {
    let (queued, answers) = watched.override_with(VALID, |_| {});
    ensure!(queued, "the override was not queued");
    watched.view.load_url(page);
    watched.at(page)?;
    ensure!(*answers.borrow() == [Ok("{}".to_string())], "{:?}", answers.borrow());
    verify(&watched.view)
}

fn rejection(watched: &Watched, base: &str) -> Result<(), String> {
    let invalid = [
        r#"{"userAgent":"bad\r\nagent"}"#,
        r#"{"userAgent":42}"#,
        r#"{"userAgent":"changed","unknown":true}"#,
        "{",
        "literal\0nul",
    ];
    for (index, options) in invalid.iter().enumerate() {
        let (queued, answers) = watched.override_with(options, |_| {});
        // The engine's rule: taken (true), and the error told once, after
        // the call has returned; false is only for a closed view.
        ensure!(queued && answers.borrow().is_empty(), "{index}: {queued} {:?}", answers.borrow());
        must_wait(|| !answers.borrow().is_empty(), "the refusal")?;
        ensure!(answers.borrow().len() == 1 && answers.borrow()[0].is_err(), "{index}: {queued} {:?}", answers.borrow());
        verify(&watched.view)?;
        let url = format!("{base}/after-rejection-{index}");
        watched.view.load_url(&url);
        watched.at(&url)?;
        verify(&watched.view)?;
    }
    Ok(())
}

fn worker(view: &View) -> Result<(), String> {
    let hints = ev(view, &format!("navigator.userAgentData.getHighEntropyValues({HINTS})"))?;
    let worker = ev(
        view,
        "new Promise((resolve,reject)=>{const w=new Worker('/worker.js');\
         w.onmessage=e=>{w.terminate();resolve(e.data)};w.onerror=reject})",
    )?;
    ensure!(worker["ua"].as_str() == Some(AGENT), "the worker's agent: {worker:?}");
    let told = worker["hints"].as_object().unwrap_or_default();
    ensure!(
        !told.is_empty() && told.iter().all(|(key, value)| hints.get(key).is_some_and(|page| same(page, value))),
        "the worker's hints {told:?}, the page's {hints:?}"
    );
    Ok(())
}

fn completion(watched: &Watched, base: &str) -> Result<(), String> {
    let done = Ok("{}".to_string());
    let (queued, answers) = watched.override_with(VALID, |_| {});
    ensure!(queued && wait(|| !answers.borrow().is_empty(), 20) && *answers.borrow() == [done.clone()], "the override completes with {{}}: {:?}", answers.borrow());
    let (queued, cleared) = watched.override_with(r#"{"userAgent":""}"#, |_| {});
    ensure!(queued && wait(|| !cleared.borrow().is_empty(), 20) && *cleared.borrow() == [done.clone()], "cleared with an empty agent: {:?}", cleared.borrow());
    let agent = ev(&watched.view, "navigator.userAgent")?;
    ensure!(agent.as_str().is_some_and(|agent| agent != AGENT), "the engine's agent again: {agent:?}");

    // A load from the completion supersedes the one waiting for it.
    let redirected = Watched::new();
    let errors: Rc<RefCell<Vec<String>>> = Rc::default();
    let (into, view, target) = (errors.clone(), redirected.view.clone(), format!("{base}/from-completion"));
    let (queued, _) = redirected.override_with(VALID, move |result| match result {
        Err(error) => into.borrow_mut().push(error.clone()),
        Ok(_) => view.load_url(&target),
    });
    redirected.view.load_url(&format!("{base}/superseded-deferred"));
    redirected.at(&format!("{base}/from-completion"))?;
    ensure!(queued && errors.borrow().is_empty(), "a load from the completion: {:?}", errors.borrow());
    let superseded = format!("{base}/superseded-deferred");
    ensure!(!redirected.loads.borrow().contains(&superseded), "the superseded load finished: {:?}", redirected.loads.borrow());
    redirected.view.close();

    let closing = Watched::new();
    let view = closing.view.clone();
    let (queued, answers) = closing.override_with(VALID, move |_| view.close());
    ensure!(
        queued && wait(|| closing.closed.get(), 20) && *answers.borrow() == [done.clone()],
        "the view closed from the completion: {:?}",
        answers.borrow()
    );

    let cancelled = Watched::new();
    let (queued, answers) = cancelled.override_with(VALID, |_| {});
    cancelled.view.close();
    ensure!(
        queued && wait(|| cancelled.closed.get(), 20) && answers.borrow().len() == 1
            && (answers.borrow()[0] == done || answers.borrow()[0].is_err()),
        "closed while one is pending: answered once, before the close: {:?}",
        answers.borrow()
    );
    ensure!(wait(|| redirected.closed.get(), 20), "the other views close");
    Ok(())
}

/// A minimal CDP client: one WebSocket, text frames, answers by id.
struct Cdp {
    stream: TcpStream,
    next: u64,
}

impl Cdp {
    fn connect(endpoint: &str) -> Result<Cdp, String> {
        let rest = endpoint.strip_prefix("ws://").ok_or("not a ws:// address")?;
        let (host, path) = rest.split_once('/').ok_or("no path")?;
        let mut stream = TcpStream::connect(host).map_err(|error| error.to_string())?;
        stream.set_read_timeout(Some(Duration::from_secs(30))).map_err(|error| error.to_string())?;
        write!(
            stream,
            "GET /{path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
             Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n"
        )
        .map_err(|error| error.to_string())?;
        let mut head = Vec::new();
        let mut byte = [0u8];
        while !head.ends_with(b"\r\n\r\n") {
            stream.read_exact(&mut byte).map_err(|error| error.to_string())?;
            head.push(byte[0]);
        }
        let head = String::from_utf8_lossy(&head);
        if !head.starts_with("HTTP/1.1 101") {
            return Err(format!("no WebSocket: {head}"));
        }
        Ok(Cdp { stream, next: 0 })
    }

    fn send(&mut self, text: &str) -> Result<(), String> {
        let bytes = text.as_bytes();
        let mut frame = vec![0x81u8];
        match bytes.len() {
            length if length < 126 => frame.push(0x80 | length as u8),
            length if length < 65536 => {
                frame.push(0x80 | 126);
                frame.extend_from_slice(&(length as u16).to_be_bytes());
            }
            length => {
                frame.push(0x80 | 127);
                frame.extend_from_slice(&(length as u64).to_be_bytes());
            }
        }
        let mask = [0x12u8, 0x34, 0x56, 0x78];
        frame.extend_from_slice(&mask);
        frame.extend(bytes.iter().enumerate().map(|(index, byte)| byte ^ mask[index % 4]));
        self.stream.write_all(&frame).map_err(|error| error.to_string())
    }

    fn receive(&mut self) -> Result<String, String> {
        let mut message = Vec::new();
        loop {
            let mut head = [0u8; 2];
            self.stream.read_exact(&mut head).map_err(|error| error.to_string())?;
            let length = match head[1] & 0x7f {
                126 => {
                    let mut size = [0u8; 2];
                    self.stream.read_exact(&mut size).map_err(|error| error.to_string())?;
                    u64::from(u16::from_be_bytes(size))
                }
                127 => {
                    let mut size = [0u8; 8];
                    self.stream.read_exact(&mut size).map_err(|error| error.to_string())?;
                    u64::from_be_bytes(size)
                }
                length => u64::from(length),
            };
            let mut payload = vec![0u8; usize::try_from(length).map_err(|_| "too long")?];
            self.stream.read_exact(&mut payload).map_err(|error| error.to_string())?;
            match head[0] & 0x0f {
                0x8 => return Err("the endpoint closed".into()),
                0x9 | 0xa => continue,
                _ => message.extend_from_slice(&payload),
            }
            if head[0] & 0x80 != 0 {
                return Ok(String::from_utf8_lossy(&message).into_owned());
            }
        }
    }

    /// The result of `method`, its params JSON, in `session` when given.
    fn call(&mut self, method: &str, params: &str, session: Option<&str>) -> Result<Value, String> {
        self.next += 1;
        let id = self.next;
        let session = session.map(|session| format!(",\"sessionId\":{}", quote(session))).unwrap_or_default();
        self.send(&format!("{{\"id\":{id},\"method\":{},\"params\":{params}{session}}}", quote(method)))?;
        loop {
            let message = Value::parse(&self.receive()?).map_err(|error| error.to_string())?;
            if message["id"].as_f64() == Some(id as f64) {
                if !message["error"].is_null() {
                    return Err(format!("{method}: {:?}", message["error"]));
                }
                return Ok(message["result"].clone());
            }
        }
    }
}

/// `work` on another thread, the engine pumped here meanwhile.
fn external<T: Send>(work: impl FnOnce() -> T + Send) -> T {
    std::thread::scope(|scope| {
        let handle = scope.spawn(work);
        while !handle.is_finished() {
            zebkit::pump();
            std::thread::sleep(Duration::from_millis(5));
        }
        handle.join().expect("the CDP client panicked")
    })
}

/// The binding's set_user_agent and a CDP client's override: Chromium keeps
/// the client's in its session, the binding's comes back once it detaches.
fn scope(base: &str) -> Result<(), String> {
    zebkit::cdp::listen(Some("127.0.0.1"), 0).map_err(|error| format!("CDP served: {error}"))?;
    let watched = Watched::new();
    let view = watched.view.clone();
    let result = (|| {
        view.set_user_agent(Some("zebkit-native-before/1"));
        let initial = format!("{base}/initial");
        view.load_url(&initial);
        watched.at(&initial)?;
        let endpoint = zebkit::cdp::url().ok_or("no CDP address")?;
        let target = view.target_id().ok_or("no target id")?;
        let mut cdp = external(|| Cdp::connect(&endpoint))?;
        let attach = format!("{{\"targetId\":{},\"flatten\":true}}", quote(&target));
        let attached = external(|| cdp.call("Target.attachToTarget", &attach, None))?;
        let session = attached["sessionId"].as_str().ok_or("no session")?.to_string();
        external(|| cdp.call("Emulation.setUserAgentOverride", r#"{"userAgent":"zebkit-external/2"}"#, Some(&session)))?;
        let navigate = |path: &str, expected: &str| -> Result<(), String> {
            let url = format!("{base}{path}");
            view.load_url(&url);
            watched.at(&url)?;
            let snapshot = ev(&view, "({current:navigator.userAgent,first:window.firstUserAgent})")?;
            ensure!(
                snapshot["current"].as_str() == Some(expected) && snapshot["first"].as_str() == Some(expected),
                "{path}: {snapshot:?}"
            );
            Ok(())
        };
        let agent = ev(&view, "navigator.userAgent")?;
        ensure!(agent.as_str() == Some("zebkit-external/2"), "{agent:?}");
        navigate("/external", "zebkit-external/2")?;
        view.set_user_agent(Some("zebkit-native-after/3"));
        // A later call of the binding's does not remove the session's override.
        navigate("/native-while-external", "zebkit-external/2")?;
        let detach = format!("{{\"sessionId\":{}}}", quote(&session));
        external(|| cdp.call("Target.detachFromTarget", &detach, None))?;
        navigate("/detached", "zebkit-native-after/3")?;
        Ok(())
    })();
    view.close();
    let closes = wait(|| watched.closed.get(), 20);
    zebkit::cdp::stop();
    result?;
    ensure!(closes, "the view closes");
    Ok(())
}

fn run(mode: &str, page: &str, base: &str) -> Result<(), String> {
    if mode == "cdp-scope" {
        return scope(base);
    }
    let watched = Watched::new();
    if mode != "completion" {
        first_load(&watched, page)?;
    }
    match mode {
        "hints" => {}
        "rejection" => rejection(&watched, base)?,
        "worker" => worker(&watched.view)?,
        "completion" => completion(&watched, base)?,
        _ => return Err(format!("no such mode: {mode}")),
    }
    if !watched.closed.get() {
        watched.view.close();
    }
    ensure!(wait(|| watched.closed.get(), 20), "the view closes");
    Ok(())
}

fn main() {
    let page = std::env::var("ZEBKIT_UA_TEST_URL").expect("ZEBKIT_UA_TEST_URL");
    let base = std::env::var("ZEBKIT_UA_BASE").unwrap_or_else(|_| page.rsplit_once('/').map_or(page.clone(), |(base, _)| base.to_string()));
    let mode = std::env::var("ZEBKIT_UA_CASE").unwrap_or_else(|_| "rejection".into());
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#).expect("initialize");
    let result = run(&mode, &page, &base);
    assert!(zebkit::shutdown());
    match result {
        Ok(()) if mode == "rejection" => println!("ok Rust UA/client hints and atomic rejection"),
        Ok(()) => println!("ok Rust UA/client hints {mode}"),
        Err(error) => {
            println!("FAIL Rust UA/client hints {mode}: {error}");
            std::process::exit(1);
        }
    }
}
