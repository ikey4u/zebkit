//! Drives a real engine: the services, a view without a window, a page's
//! call to the application, script outside any page, and the CDP endpoint.
//! Run with `cargo test`; it needs a built zebkit (see the README).

use std::cell::{Cell, RefCell};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::rc::Rc;
use std::time::Duration;
use zebkit::services::{self, Compression, Digest, ImageFormat, UrlPart};
use zebkit::vendor::ffmpeg;
use zebkit::{cdp, Allow, LoadEvent, Script, View};

thread_local! {
    static FAILED: Cell<u32> = const { Cell::new(0) };
    static PASSED: Cell<u32> = const { Cell::new(0) };
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

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

/// What needs no engine running.
fn services() {
    let text = b"zebkit ".repeat(100);
    for format in [Compression::Gzip, Compression::Zlib, Compression::DeflateRaw, Compression::Brotli, Compression::Zstd] {
        let packed = services::compress(format, &text, None).unwrap();
        let unpacked = services::decompress(format, &packed, None).unwrap();
        check(packed.len() < text.len() && unpacked == text, &format!("{format:?} both ways"), packed.len());
    }
    check(services::decompress(Compression::Gzip, b"not gzip", None).is_err(), "bad gzip is an error", ());
    check(
        hex(&services::digest(Digest::Sha256, b"abc"))
            == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256",
        (),
    );
    check(
        hex(&services::hmac(Digest::Sha256, b"key", b"The quick brown fox jumps over the lazy dog"))
            == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
        "HMAC-SHA-256",
        (),
    );
    check(services::random_bytes(32) != services::random_bytes(32), "random bytes", ());
    let (key, nonce) = (services::random_bytes(32), services::random_bytes(12));
    let mut sealed = services::aes_gcm_seal(&key, &nonce, b"ad", b"secret").unwrap();
    check(services::aes_gcm_open(&key, &nonce, b"ad", &sealed).as_deref() == Ok(b"secret".as_slice()), "AES-GCM", ());
    sealed[0] ^= 1;
    check(services::aes_gcm_open(&key, &nonce, b"ad", &sealed).is_err(), "AES-GCM refuses a change", ());
    check(
        services::url_part("../b?x#y", Some("https://Example.com/a/c"), UrlPart::Spec).as_deref()
            == Some("https://example.com/b?x#y"),
        "URL resolved",
        services::url_part("../b?x#y", Some("https://Example.com/a/c"), UrlPart::Spec),
    );
    check(services::url_part("no url", None, UrlPart::Host).is_none(), "not a URL", ());
    check(
        services::registrable_domain("a.b.example.co.uk").as_deref() == Some("example.co.uk"),
        "registrable domain",
        services::registrable_domain("a.b.example.co.uk"),
    );
    let image = services::Image { width: 2, height: 1, rgba: vec![255, 0, 0, 255, 0, 0, 255, 128] };
    let png = services::encode_image(ImageFormat::Png, &image, 0).unwrap();
    check(services::decode_image(&png).as_ref() == Ok(&image), "PNG both ways", png.len());
    check(
        ffmpeg::has_decoder("h264") && ffmpeg::has_decoder("aac") && ffmpeg::has_decoder("libopus") && !ffmpeg::has_decoder("no-such-codec"),
        &format!("FFmpeg {} decoders", ffmpeg::version()),
        ffmpeg::decoders().len(),
    );
}

/// A page for the view to load, from a server of the test's own.
fn serve(page: &'static str) -> u16 {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let mut reader = BufReader::new(stream);
            let mut line = String::new();
            while reader.read_line(&mut line).is_ok_and(|read| read > 2) {
                line.clear();
            }
            let mut stream = reader.into_inner();
            let _ = write!(
                stream,
                "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{page}",
                page.len()
            );
        }
    });
    port
}

/// The engine's part, from inside run(); quits when it is done.
fn engine(page_port: u16) {
    check(zebkit::is_headless(), "headless", ());
    check(zebkit::engine_version().starts_with("Chromium "), "engine version", zebkit::engine_version());

    // A page that calls the application and shows the answer in its title.
    let view = View::new(None, 800, 600);
    let origin = format!("http://127.0.0.1:{page_port}");
    view.grant(&origin, Allow::APP | Allow::CRYPTO, None);
    let calls = Rc::new(RefCell::new(Vec::new()));
    {
        let calls = calls.clone();
        view.bind("add", move |call| {
            calls.borrow_mut().push((call.origin().to_string(), call.args_json().to_string()));
            let numbers: Vec<f64> = call
                .args_json()
                .trim_matches(|c| c == '[' || c == ']')
                .split(',')
                .filter_map(|number| number.trim().parse().ok())
                .collect();
            call.resolve(&numbers.iter().sum::<f64>().to_string());
        });
    }
    view.bind("refuse", |call| call.reject("no"));
    let events = Rc::new(RefCell::new(Vec::new()));
    {
        let events = events.clone();
        view.on_load(move |event, _url| events.borrow_mut().push(event));
    }
    let titles = Rc::new(RefCell::new(Vec::new()));
    {
        let (titles, view2) = (titles.clone(), view.clone());
        let events = events.clone();
        let calls = calls.clone();
        view.on_title(move |title| {
            titles.borrow_mut().push(title.to_string());
            if title != "done" {
                return;
            }
            check(
                *calls.borrow() == [(origin.clone(), "[2,3]".to_string())],
                "zebkit.app.add() called with its arguments",
                calls.borrow(),
            );
            check(
                events.borrow().contains(&LoadEvent::Committed) && events.borrow().contains(&LoadEvent::Finished),
                "load events",
                events.borrow(),
            );
            let view3 = view2.clone();
            view2.evaluate("({sum: window.sum, refused: window.refused, digest: window.digest})", move |result| {
                check(
                    result.as_deref()
                        == Ok(r#"{"digest":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","refused":"no","sum":5}"#),
                    "the page got the answers",
                    &result,
                );
                let view4 = view3.clone();
                view3.evaluate("throw new Error('broken')", move |result| {
                    check(
                        result.as_ref().is_err_and(|error| error.message().contains("broken")),
                        "a script's exception",
                        &result,
                    );
                    script_and_cdp(view4);
                });
            });
        });
    }
    view.load_url(&format!("http://127.0.0.1:{page_port}/"));
}

fn script_and_cdp(view: View) {
    let script = Script::new(&["one", "two"]);
    let script = Rc::new(script);
    let held = script.clone();
    script.evaluate("zebkit.args.join('+') + ' ' + typeof zebkit.View", move |result| {
        check(result.as_deref() == Ok(r#""one+two function""#), "script outside any page", &result);
        drop(held);
    });

    let port = cdp::listen(None, 0);
    check(port.is_ok(), "CDP listens", &port);
    let Ok(port) = port else {
        zebkit::quit();
        return;
    };
    check(cdp::port() == Some(port), "CDP port", cdp::port());
    check(
        cdp::url().is_some_and(|url| url.starts_with(&format!("ws://127.0.0.1:{port}/devtools/browser/"))),
        "CDP browser URL",
        cdp::url(),
    );
    // A client in another thread; the engine stays on this one.
    let client = std::thread::spawn(move || {
        let mut stream = TcpStream::connect(("127.0.0.1", port)).expect("cannot connect to the endpoint");
        stream.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
        write!(stream, "GET /json/list HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n").unwrap();
        let mut text = String::new();
        let _ = stream.read_to_string(&mut text);
        text
    });
    fn wait(client: std::thread::JoinHandle<String>, view: View) {
        if !client.is_finished() {
            zebkit::after(Duration::from_millis(20), move || wait(client, view));
            return;
        }
        let list = client.join().unwrap();
        check(list.contains("\"type\": \"page\"") && list.contains("done"), "CDP lists the page", &list);
        cdp::stop();
        check(cdp::port().is_none(), "CDP stops", cdp::port());
        let closed = Rc::new(Cell::new(false));
        {
            let closed = closed.clone();
            view.on_close(move || {
                closed.set(true);
                zebkit::quit();
            });
        }
        view.close();
        let _ = closed;
    }
    wait(client, view);
}

fn main() {
    services();

    let port = serve(
        "<title>loading</title><script>\
         (async () => {\
           window.sum = await zebkit.app.add(2, 3);\
           try { await zebkit.app.refuse(); } catch (error) { window.refused = error.message; }\
           const digest = await zebkit.crypto.digest('SHA-256', new TextEncoder().encode('abc'));\
           window.digest = [...new Uint8Array(digest)].map(b => b.toString(16).padStart(2, '0')).join('');\
           document.title = 'done';\
         })();\
         </script>",
    );
    let profile = std::env::temp_dir().join(format!("zebkit-rust-test-{}", std::process::id()));
    let args = [
        "zebkit-test".to_string(),
        "--headless".to_string(),
        "--no-sandbox".to_string(),
        format!("--user-data-dir={}", profile.display()),
    ];
    // Ends on its own; a test that hangs is a failure too.
    std::thread::spawn(|| {
        std::thread::sleep(Duration::from_secs(120));
        eprintln!("FAIL  the test did not finish in 120 s");
        std::process::exit(1);
    });
    let code = zebkit::run_with_args(args, move || engine(port));
    let _ = std::fs::remove_dir_all(&profile);
    check(code == 0, "the engine's exit code", code);

    let (passed, failed) = (PASSED.with(Cell::get), FAILED.with(Cell::get));
    println!("{passed} passed, {failed} failed");
    std::process::exit(if failed == 0 { 0 } else { 1 });
}
