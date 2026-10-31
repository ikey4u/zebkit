//! The hooks through the Rust binding; run by tests/engine/hook_policies.py.
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::{Duration, Instant};
use zebkit::{ClientCertificateAction, FullscreenPolicy, LoadEvent, View};

fn env(name: &str) -> String { std::env::var(name).unwrap_or_else(|_| panic!("{name}")) }
fn wait(what: &str, test: impl Fn() -> bool) {
    let end = Instant::now() + Duration::from_secs(15);
    while !test() {
        assert!(Instant::now() < end, "timed out: {what}");
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
}
fn pump_for(ms: u64) {
    let end = Instant::now() + Duration::from_millis(ms);
    while Instant::now() < end {
        assert!(zebkit::pump());
        std::thread::sleep(Duration::from_millis(1));
    }
}
type Box2 = Rc<RefCell<Option<String>>>;
fn later(view: &View, script: &str) -> Box2 {
    let result: Box2 = Rc::new(RefCell::new(None));
    let slot = result.clone();
    view.evaluate(script, move |value| *slot.borrow_mut() = Some(value.unwrap_or_else(|error| format!("error:{error:?}"))));
    result
}
fn ev(view: &View, script: &str) -> String {
    let result = later(view, script);
    wait(script, || result.borrow().is_some());
    let value = result.borrow().clone().unwrap();
    value
}
type Log = Rc<RefCell<Vec<String>>>;
fn log() -> Log { Rc::new(RefCell::new(Vec::new())) }
fn logged(log: &Log, text: &str) -> bool { log.borrow().iter().any(|line| line.contains(text)) }

struct Run { mode: String, base: String, tls: String, view: View, closed: Rc<Cell<bool>>, finished: Rc<Cell<bool>>, failed: Rc<Cell<bool>> }
impl Run {
    fn is(&self, mode: &str) -> bool { self.mode == mode }
    fn load(&self, url: &str) {
        self.finished.set(false);
        self.failed.set(false);
        self.view.load_url(url);
    }
    fn open_page(&self) {
        let url = format!("{}/page", self.base);
        self.load(&url);
        wait("page", || self.finished.get() && self.view.url().as_deref() == Some(url.as_str()));
    }
    fn wait_closed(&self) { wait("closed", || self.closed.get()); }
}

fn external(run: &Run) {
    run.open_page();
    let told = log();
    if !run.is("external-none") {
        let (sink, owner, close) = (told.clone(), run.view.clone(), run.is("external-close"));
        run.view.on_external_protocol(move |json| {
            sink.borrow_mut().push(json.to_string());
            if close { owner.close(); }
        });
    }
    ev(&run.view, "location.href = 'zoommtg://join?x=1'; 1");
    if run.is("external-none") {
        pump_for(300);
        assert_eq!(ev(&run.view, "location.pathname"), "\"/page\"");
        run.view.close();
        return;
    }
    wait("told", || !told.borrow().is_empty());
    assert!(logged(&told, "\"scheme\":\"zoommtg\""));
    if run.is("external-close") { run.wait_closed(); return; }
    assert_eq!(ev(&run.view, "location.pathname"), "\"/page\"");
    run.view.clear_external_protocol_handler();
    ev(&run.view, "location.href = 'mailto:a@example.com'; 1");
    pump_for(300);
    assert_eq!(told.borrow().len(), 1);
    run.view.close();
}

fn download(run: &Run) {
    let target = format!("{}/saved.bin", env("ZEBKIT_HOOK_DOWNLOADS"));
    std::fs::write(&target, b"old").unwrap();
    run.open_page();
    let progress = log();
    let sink = progress.clone();
    run.view.on_download_progress(move |json| sink.borrow_mut().push(json.to_string()));
    let kept = Rc::new(RefCell::new(None));
    if !run.is("download-default") {
        let (slot, mode, owner, path) = (kept.clone(), run.mode.clone(), run.view.clone(), target.clone());
        run.view.on_download(move |request: zebkit::DownloadRequest| {
            assert!(request.metadata().contains("\"suggestedName\":\"report.bin\""));
            assert_eq!(request.info().as_deref(), Some(request.metadata()));
            *slot.borrow_mut() = Some(request.clone());
            match mode.as_str() {
                "download-save" => {
                    assert!(!request.save("relative.bin").unwrap());
                    assert!(request.save("bad\0").is_err());
                    assert!(request.save(&path).unwrap());
                    assert!(!request.cancel());
                    assert!(request.info().is_none());
                }
                "download-cancel" => assert!(request.cancel()),
                "download-close" => owner.close(),
                _ => {}
            }
        });
    }
    if run.is("download-timeout") {
        assert!(!run.view.set_download_timeout(0));
        assert!(run.view.set_download_timeout(50));
    }
    ev(&run.view, "location.href = '/download.bin'; 1");
    let size = || std::fs::metadata(&target).unwrap().len();
    let cancelled = || progress.borrow().iter().any(|e| e.contains("\"state\":\"cancelled\"") && e.contains("\"done\":true"));
    if run.is("download-default") {
        wait("saved in the profile", || logged(&progress, "Downloads/report.bin") && logged(&progress, "\"state\":\"complete\""));
        run.view.close();
        return;
    }
    wait("asked", || kept.borrow().is_some());
    let request = kept.borrow().clone().unwrap();
    match run.mode.as_str() {
        "download-save" => { wait("complete", || logged(&progress, "\"state\":\"complete\"")); assert_eq!(size(), 65536); }
        "download-cancel" => { wait("cancelled", cancelled); assert_eq!(size(), 3); }
        "download-timeout" => {
            wait("timeout", || logged(&progress, "\"reason\":\"timeout\""));
            assert!(!request.save(&target).unwrap());
            assert_eq!(size(), 3);
        }
        "download-close" => { run.wait_closed(); assert!(request.info().is_none()); return; }
        "download-pending-close" => {
            run.view.close();
            run.wait_closed();
            assert!(!request.save(&target).unwrap());
            return;
        }
        "download-replace" => {
            run.view.clear_download_handler();
            assert!(request.info().is_none());
            wait("replaced", || logged(&progress, "\"reason\":\"handler-changed\""));
        }
        _ => {}
    }
    run.view.close();
}

fn certificate(run: &Run) {
    let cert = std::fs::read_to_string(env("ZEBKIT_HOOK_CERT")).unwrap();
    let key = std::fs::read_to_string(env("ZEBKIT_HOOK_KEY")).unwrap();
    let other = std::fs::read_to_string(env("ZEBKIT_HOOK_OTHER_KEY")).unwrap();
    run.view.on_certificate(|request| { assert!(request.respond(true)); });
    let results = log();
    let sink = results.clone();
    run.view.on_client_certificate_result(move |json| sink.borrow_mut().push(json.to_string()));
    let kept = Rc::new(RefCell::new(None));
    if !run.is("cert-default") {
        let (slot, mode, owner) = (kept.clone(), run.mode.clone(), run.view.clone());
        run.view.on_client_certificate(move |request: zebkit::ClientCertificateRequest| {
            assert!(request.metadata().contains("\"host\":\"localhost\""));
            *slot.borrow_mut() = Some(request.clone());
            match mode.as_str() {
                "cert-provide" => {
                    assert!(!request.provide("junk", &key).unwrap());
                    assert!(!request.provide(&cert, "junk").unwrap());
                    assert!(!request.provide(&cert, &other).unwrap());
                    assert!(request.provide(&cert, &key).unwrap());
                    assert!(!request.respond(ClientCertificateAction::None));
                }
                "cert-none" => assert!(request.respond(ClientCertificateAction::None)),
                "cert-cancel" => assert!(request.respond(ClientCertificateAction::Cancel)),
                "cert-close" => owner.close(),
                _ => {}
            }
        });
    }
    if run.is("cert-timeout") { assert!(run.view.set_client_certificate_timeout(50)); }
    run.load(&format!("{}/secure", run.tls));
    let title = |text: &str| run.view.title().as_deref() == Some(text);
    match run.mode.as_str() {
        "cert-provide" => wait("with it", || title("cert:zebkit-client")),
        "cert-none" | "cert-default" => wait("without", || title("cert:none")),
        "cert-cancel" | "cert-timeout" => {
            wait("failed", || run.failed.get() && !results.borrow().is_empty());
            assert!(logged(&results, "\"state\":\"cancelled\""));
        }
        "cert-close" => { run.wait_closed(); return; }
        "cert-pending-close" => {
            wait("asked", || kept.borrow().is_some());
            run.view.close();
            run.wait_closed();
            assert!(!kept.borrow().as_ref().unwrap().respond(ClientCertificateAction::None));
            return;
        }
        "cert-replace" => {
            wait("asked", || kept.borrow().is_some());
            run.view.clear_client_certificate_handler();
            assert!(kept.borrow().as_ref().unwrap().info().is_none());
            wait("failed", || run.failed.get() && !results.borrow().is_empty());
            assert!(logged(&results, "\"reason\":\"handler-changed\""));
        }
        _ => {}
    }
    run.view.close();
}

fn world(run: &Run) {
    let all = "var worldVar = 1; addEventListener('zkping', () => document.documentElement.setAttribute('data-all', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));";
    let main = "addEventListener('zkping', () => document.documentElement.setAttribute('data-main', typeof pageVar === 'undefined' ? 'isolated' : 'leak'));";
    assert!(run.view.add_world_script(all, Some("zebkit-test"), false).unwrap());
    assert!(run.view.add_world_script(main, Some("zebkit-main"), true).unwrap());
    assert!(run.view.add_world_script("window.mainWorldVar = 1;", None, true).unwrap());
    assert!(!run.view.add_world_script("1", Some(&"x".repeat(257)), false).unwrap());
    run.load(&format!("{}/frames", run.base));
    wait("loaded", || run.finished.get());
    wait("frames", || ev(&run.view, "window.reports && reports.length") == "2");
    assert_eq!(ev(&run.view, "[report.all, report.main, report.global, report.mainWorld].join()"), "\"isolated,isolated,undefined,number\"");
    assert_eq!(ev(&run.view, "reports.every(r => r.all === 'isolated' && r.main === null && r.global === 'undefined' && r.mainWorld === 'undefined')"), "true");
    run.view.close();
}

fn chooser(run: &Run) {
    let files = env("ZEBKIT_HOOK_FILES");
    run.open_page();
    let results = log();
    let sink = results.clone();
    run.view.on_file_chooser_result(move |json| sink.borrow_mut().push(json.to_string()));
    let kept = Rc::new(RefCell::new(None));
    let (slot, mode, owner, root) = (kept.clone(), run.mode.clone(), run.view.clone(), files.clone());
    run.view.on_file_chooser(move |request: zebkit::FileChooserRequest| {
        *slot.borrow_mut() = Some(request.clone());
        let (one, two) = (format!("{root}/one.txt"), format!("{root}/two.txt"));
        match mode.as_str() {
            "file-open" => {
                assert!(!request.respond(&["relative.txt"]).unwrap());
                assert!(!request.respond(&[&one, &two]).unwrap());
                assert!(request.respond(&[&one]).unwrap());
                assert!(!request.respond(&[]).unwrap());
            }
            "file-multiple" => assert!(request.respond(&[&one, &two]).unwrap()),
            "file-folder" => assert!(request.respond(&[&format!("{root}/folder")]).unwrap()),
            "file-cancel" => assert!(request.respond(&[]).unwrap()),
            "file-close" => owner.close(),
            _ => {}
        }
    });
    if run.is("file-timeout") { assert!(run.view.set_file_chooser_timeout(50)); }
    let element = match run.mode.as_str() { "file-multiple" => "m", "file-folder" => "d", _ => "i" };
    ev(&run.view, &format!("{element}.addEventListener('cancel', () => window.cancelled = 1); {element}.click(); 1"));
    wait("asked", || kept.borrow().is_some());
    let count = |n: &str| wait("files", || ev(&run.view, &format!("{element}.files.length")) == n);
    match run.mode.as_str() {
        "file-open" => { count("1"); assert_eq!(ev(&run.view, "i.files[0].text()"), "\"first file\""); }
        "file-multiple" => { count("2"); assert_eq!(ev(&run.view, "m.files[1].text()"), "\"second file\""); }
        "file-folder" => { count("2"); assert_eq!(ev(&run.view, "[...d.files].map(f => f.webkitRelativePath).sort().join()"), "\"folder/a.txt,folder/sub/b.txt\""); }
        "file-cancel" | "file-timeout" => {
            wait("told", || !results.borrow().is_empty());
            assert!(logged(&results, "\"state\":\"cancelled\""));
            wait("page told", || ev(&run.view, "window.cancelled === 1") == "true");
        }
        "file-close" => { run.wait_closed(); return; }
        "file-pending-close" => {
            run.view.close();
            run.wait_closed();
            assert!(!kept.borrow().as_ref().unwrap().respond(&[]).unwrap());
            return;
        }
        "file-replace" => {
            run.view.clear_file_chooser_handler();
            assert!(kept.borrow().as_ref().unwrap().info().is_none());
            wait("told", || logged(&results, "\"reason\":\"handler-changed\""));
        }
        _ => {}
    }
    run.view.close();
}

fn media(run: &Run) {
    run.open_page();
    let results = log();
    let sink = results.clone();
    run.view.on_media_access_result(move |json| sink.borrow_mut().push(json.to_string()));
    let kept = Rc::new(RefCell::new(None));
    let (slot, mode, owner) = (kept.clone(), run.mode.clone(), run.view.clone());
    run.view.on_media_access(move |request: zebkit::MediaAccessRequest| {
        assert!(request.metadata().contains("\"video\":true"));
        *slot.borrow_mut() = Some(request.clone());
        match mode.as_str() {
            "media-allow" => {
                assert!(request.respond(true, None, None).unwrap());
                assert!(!request.respond(false, None, None).unwrap());
            }
            "media-deny" => {
                assert!(!request.respond(false, None, Some("device")).unwrap());
                assert!(request.respond(false, None, None).unwrap());
            }
            "media-close" => owner.close(),
            _ => {}
        }
    });
    if run.is("media-timeout") { assert!(run.view.set_media_access_timeout(50)); }
    let answer = later(&run.view, "navigator.mediaDevices.getUserMedia({video: true}).then(s => { const n = s.getTracks().length; s.getTracks().forEach(t => t.stop()); return 'ok' + n; }, e => e.name)");
    wait("asked", || kept.borrow().is_some());
    let answered = || { wait("answered", || answer.borrow().is_some()); answer.borrow().clone().unwrap() };
    match run.mode.as_str() {
        "media-allow" => assert_eq!(answered(), "\"ok1\""),
        "media-deny" | "media-timeout" => {
            assert_eq!(answered(), "\"NotAllowedError\"");
            wait("told", || logged(&results, "\"state\":\"denied\""));
        }
        "media-close" => { run.wait_closed(); return; }
        "media-pending-close" => {
            run.view.close();
            run.wait_closed();
            assert!(!kept.borrow().as_ref().unwrap().respond(true, None, None).unwrap());
            return;
        }
        "media-replace" => {
            run.view.clear_media_access_handler();
            assert!(kept.borrow().as_ref().unwrap().info().is_none());
            assert_eq!(answered(), "\"NotAllowedError\"");
        }
        _ => {}
    }
    run.view.close();
}

fn keys(run: &Run) {
    assert!(!run.view.set_reserved_keys("[\"Ctrl+Nonsense\"]").unwrap());
    assert!(!run.view.set_reserved_keys("{}").unwrap());
    assert!(run.view.set_reserved_keys("[\"Ctrl+KeyK\"]").unwrap());
    let told = log();
    let sink = told.clone();
    run.view.on_reserved_key(move |json| sink.borrow_mut().push(json.to_string()));
    run.load(&format!("{}/input", run.base));
    wait("told", || !told.borrow().is_empty());
    assert!(logged(&told, "\"combo\":\"Ctrl+KeyK\""));
    wait("other key", || ev(&run.view, "keys.length > 0") == "true");
    assert_eq!(ev(&run.view, "keys.every(k => k === 'KeyB')"), "true");
    run.view.close();
}

fn menu(run: &Run) {
    let told = log();
    let sink = told.clone();
    run.view.on_context_menu(move |json| sink.borrow_mut().push(json.to_string()));
    run.load(&format!("{}/input", run.base));
    wait("told", || !told.borrow().is_empty());
    assert!(logged(&told, "\"x\":20") && logged(&told, "\"y\":30") && logged(&told, "/page\""));
    assert_eq!(ev(&run.view, "window.menu"), "1");
    run.view.close();
}

fn fullscreen(run: &Run) {
    const FULL: &str = "document.documentElement.requestFullscreen().then(() => 'in', e => e.name)";
    run.open_page();
    let told = log();
    let sink = told.clone();
    run.view.on_fullscreen(move |json| sink.borrow_mut().push(json.to_string()));
    if run.is("fullscreen-deny") {
        assert!(run.view.set_fullscreen_policy(FullscreenPolicy::Deny));
        assert_eq!(ev(&run.view, FULL), "\"TypeError\"");
        pump_for(200);
        assert!(told.borrow().is_empty());
        assert!(run.view.set_fullscreen_policy(FullscreenPolicy::Allow));
    }
    assert_eq!(ev(&run.view, FULL), "\"in\"");
    wait("entered", || logged(&told, "\"entered\":true"));
    if run.is("fullscreen-deny") { assert!(run.view.set_fullscreen_policy(FullscreenPolicy::Deny)); }
    else { ev(&run.view, "document.exitFullscreen().then(() => 1)"); }
    wait("left", || logged(&told, "\"entered\":false"));
    run.view.close();
}

fn network(run: &Run) {
    run.open_page();
    let rules = match run.mode.as_str() {
        "net-response" | "net-response-block" => r#"[{"url":"**/echo","stage":"response"}]"#,
        "net-observe" => r#"[{"url":"**/echo","mode":"observe"},{"url":"**/echo","mode":"observe","stage":"response"}]"#,
        "net-none" => "[]",
        "net-filter" => r#"[{"url":"**/echo","resourceTypes":["image"]},{"url":"**/echo","frames":"sub"}]"#,
        "net-server-redirect" | "net-post" | "net-headers" => r#"[{"url":"**/echo"}]"#,
        "net-worker" => r#"[{"url":"**/echo?worker"}]"#,
        "net-navigation" => r#"[{"url":"**/held","frames":"main","resourceTypes":["document"]}]"#,
        _ => r#"[{"url":"**/held"}]"#,
    };
    assert!(!run.view.set_network_filter(r#"[{"mode":"sometimes"}]"#).unwrap());
    assert!(run.view.set_network_filter(rules).unwrap());
    let events = log();
    let sink = events.clone();
    run.view.on_network_event(move |json| sink.borrow_mut().push(json.to_string()));
    let kept: Rc<RefCell<Option<zebkit::NetworkRequest>>> = Rc::new(RefCell::new(None));
    let asked = Rc::new(Cell::new(0));
    let (slot, count, mode, owner, base) = (kept.clone(), asked.clone(), run.mode.clone(), run.view.clone(), run.base.clone());
    run.view.on_network(move |request: zebkit::NetworkRequest| {
        count.set(count.get() + 1);
        *slot.borrow_mut() = Some(request.clone());
        let m = request.metadata().to_string();
        assert_eq!(request.info().as_deref(), Some(m.as_str()));
        let reply = |json: &str, body: &[u8]| request.respond(json, body).unwrap();
        match mode.as_str() {
            "net-block" => {
                assert!(m.contains("\"stage\":\"request\"") && m.contains("\"resourceType\":\"fetch\""));
                assert!(!reply(r#"{"action":"nonsense"}"#, b"") && !reply("not json", b""));
                assert!(!reply(r#"{"action":"continue"}"#, b"x"));
                assert!(!reply(r#"{"action":"redirect","url":"file:///etc/passwd"}"#, b""));
                assert!(reply(r#"{"action":"block"}"#, b"") && !reply(r#"{"action":"continue"}"#, b""));
                assert!(request.info().is_none());
            }
            "net-headers" => {
                assert!(!reply(r#"{"action":"continue","headers":[{"name":"Trailer","value":"x"}]}"#, b""));
                assert!(reply(r#"{"action":"continue","headers":[{"name":"X-Zebkit","value":"yes"}]}"#, b""));
            }
            "net-redirect" => assert!(reply(&format!(r#"{{"action":"redirect","url":"{base}/echo?redirected"}}"#), b"")),
            "net-fulfill" => {
                assert!(!reply(r#"{"action":"fulfill","status":302}"#, b""));
                let bytes: Vec<u8> = (0..768u32).map(|i| i as u8).collect();
                assert!(reply(r#"{"action":"fulfill","status":201,"headers":[{"name":"Content-Type","value":"application/octet-stream"},{"name":"X-Added","value":"by-host"}]}"#, &bytes));
            }
            "net-navigation" => {
                assert!(m.contains("\"navigation\":true") && m.contains("\"mainFrame\":true"));
                assert!(reply(r#"{"action":"fulfill","headers":[{"name":"Content-Type","value":"text/html"}]}"#, b"<title>from-host</title>"));
            }
            "net-response" => {
                assert!(m.contains("\"stage\":\"response\"") && m.contains("\"status\":200"));
                assert!(reply(r#"{"action":"continue","headers":[{"name":"Content-Type","value":"application/json"},{"name":"X-Added","value":"late"}]}"#, b""));
            }
            "net-response-block" => assert!(reply(r#"{"action":"block","error":"failed"}"#, b"")),
            "net-close" => owner.close(),
            "net-server-redirect" => { assert!(m.contains("/echo\"")); assert!(reply(r#"{"action":"block"}"#, b"")); }
            "net-worker" => {
                assert!(m.contains("\"worker\":true"));
                assert!(reply(r#"{"action":"fulfill","headers":[{"name":"Content-Type","value":"text/plain"}]}"#, b"from-host"));
            }
            "net-post" => {
                assert!(m.contains("\"method\":\"POST\"") && m.contains("\"bodySize\":6") && m.contains("\"body\":\"cG9zdGVk\""));
                assert!(reply(r#"{"action":"continue"}"#, b""));
            }
            _ => {}
        }
    });
    if run.is("net-timeout") { assert!(run.view.set_network_timeout(50)); }
    if run.is("net-navigation") {
        run.load(&format!("{}/held", run.base));
        wait("answered", || run.view.title().as_deref() == Some("from-host"));
        run.view.close();
        return;
    }
    let url = match run.mode.as_str() {
        "net-none" | "net-observe" | "net-response" | "net-response-block" | "net-filter" | "net-headers" | "net-post" => "/echo",
        "net-server-redirect" => "/server-redirect",
        _ => "/held",
    };
    let script = if run.is("net-worker") {
        "new Promise(ok => { const w = new Worker('/net-worker'); w.onmessage = e => ok(e.data); })".to_string()
    } else {
        let post = if run.is("net-post") { ", {method: 'POST', body: 'posted'}" } else { "" };
        format!("fetch('{url}'{post}).then(async r => [r.status, r.redirected, r.headers.get('x-added'), await r.text()].join('|'), e => 'failed')")
    };
    let answer = later(&run.view, &script);
    if run.is("net-close") { run.wait_closed(); assert!(kept.borrow().as_ref().unwrap().info().is_none()); return; }
    if run.is("net-pending-close") {
        wait("asked", || kept.borrow().is_some());
        run.view.close();
        run.wait_closed();
        let request = kept.borrow().clone().unwrap();
        assert!(request.info().is_none() && !request.respond(r#"{"action":"continue"}"#, b"").unwrap());
        return;
    }
    if run.is("net-replace") {
        wait("asked", || kept.borrow().is_some());
        run.view.clear_network_handler();
        assert!(kept.borrow().as_ref().unwrap().info().is_none());
        wait("replaced", || logged(&events, "\"reason\":\"handler-changed\""));
    }
    wait("fetch ended", || answer.borrow().is_some());
    let text = answer.borrow().clone().unwrap();
    match run.mode.as_str() {
        "net-none" | "net-filter" => assert!(text.starts_with("\"200|") && asked.get() == 0, "{text}"),
        "net-block" | "net-response-block" | "net-server-redirect" => assert!(text == "\"failed\"" && asked.get() == 1, "{text}"),
        "net-headers" => assert!(text.contains("x-zebkit"), "{text}"),
        "net-redirect" => assert!(text.starts_with("\"200|true|"), "{text}"),
        "net-fulfill" => assert!(text.starts_with("\"201|false|by-host|"), "{text}"),
        "net-response" => assert!(text.starts_with("\"200|false|late|"), "{text}"),
        "net-worker" => assert_eq!(text, "\"from-host\""),
        "net-observe" => {
            assert_eq!(asked.get(), 0);
            wait("told", || ["request", "response", "finished"].iter().all(|kind| logged(&events, &format!("\"kind\":\"{kind}\""))));
        }
        "net-timeout" => {
            wait("timeout", || logged(&events, "\"reason\":\"timeout\""));
            assert!(kept.borrow().as_ref().unwrap().info().is_none());
        }
        _ => {}
    }
    run.view.close();
}

fn main() {
    let mode = env("ZEBKIT_HOOK_MODE");
    zebkit::init_with_settings(std::env::args_os(), r#"{"logging":{"destination":"none"}}"#).expect("initialize");
    let view = View::new(None, 640, 480);
    let (closed, finished, failed) = (Rc::new(Cell::new(false)), Rc::new(Cell::new(false)), Rc::new(Cell::new(false)));
    let signal = closed.clone();
    view.on_close(move || signal.set(true));
    let (done, bad) = (finished.clone(), failed.clone());
    view.on_load(move |phase, _| match phase {
        LoadEvent::Finished => done.set(true),
        LoadEvent::Failed => bad.set(true),
        _ => {}
    });
    let run = Run { mode: mode.clone(), base: env("ZEBKIT_HOOK_BASE"), tls: env("ZEBKIT_HOOK_TLS"), view, closed, finished, failed };
    match mode.split('-').next().unwrap() {
        "external" => external(&run),
        "download" => download(&run),
        "cert" => certificate(&run),
        "world" => world(&run),
        "file" => chooser(&run),
        "media" => media(&run),
        "keys" => keys(&run),
        "menu" => menu(&run),
        "fullscreen" => fullscreen(&run),
        "net" => network(&run),
        other => panic!("{other}"),
    }
    pump_for(50);
    println!("ok rust {mode}");
}
