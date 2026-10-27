//! Drives a real engine: the services, a view without a window, a page's
//! call to the application, script outside any page, and the CDP endpoint.
//! Run with `cargo test`; it needs a built zebkit (see the README).

use std::cell::{Cell, RefCell};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::rc::Rc;
use std::time::Duration;
use zebkit::services::{self, Compression, Digest, ImageFormat, UrlPart};
use zebkit::vendor::ffmpeg;
use zebkit::pdf::{Document, RenderOptions};
use zebkit::offscreen::{FrameBuffer, Modifiers, MouseButton, MouseEvent};
use zebkit::{cdp, scheme, Allow, LoadEvent, Script, View};

thread_local! {
    static OFFSCREEN_DONE: Cell<bool> = const { Cell::new(false) };
    static MEDIA_DONE: Cell<bool> = const { Cell::new(false) };
    // The file a capture wrote, served to a page that plays it back.
    static CAPTURED: RefCell<std::path::PathBuf> = RefCell::new(std::path::PathBuf::new());
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
    let pdf = b"%PDF-1.7\n1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n\
2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n\
3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R \
/Resources << /Font << /F1 5 0 R >> >> >> endobj\n\
4 0 obj << >> stream\nBT /F1 18 Tf 10 40 Td (Hello Rust) Tj ET\nendstream endobj\n\
5 0 obj << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> endobj\ntrailer << /Root 1 0 R >>\n%%EOF\n";
    let mut document = Document::open(pdf, None).expect("the PDF opens");
    let image = document.render(0, &RenderOptions { scale: 2.0, ..Default::default() }).unwrap();
    let found = document.search("rust", false, false, 0, 0).unwrap();
    let added = document
        .add_annotation(0, r#"{"type": "square", "rect": [5, 5, 50, 50], "color": [255, 0, 0], "border": 2}"#)
        .unwrap();
    let appearance = document
        .render_annotation(0, added, 0, &RenderOptions { scale: 2.0, background: 0, ..Default::default() })
        .unwrap();
    check(
        appearance.as_ref().is_some_and(|image| (image.width, image.height) == (90, 90))
            && document.render_annotation(0, added, 2, &RenderOptions::default()).unwrap().is_none(),
        "pdf: an annotation's appearance alone",
        appearance.map(|image| (image.width, image.height)),
    );
    let saved = document.save(false).unwrap();
    let mut again = Document::open(&saved, None).unwrap();
    check(
        document.page_count() == 1
            && (image.width, image.height) == (400, 200)
            && found.contains("\"count\":4")
            && added == 0
            && again.text(0).unwrap() == "Hello Rust"
            && again.annotations(0).unwrap().contains("\"square\""),
        "pdf: render, search, annotate, save",
        &found,
    );
    check(Document::open(b"not a pdf", None).is_err(), "pdf: what is not a PDF is refused", ());
    let areas = again.redact(0, Some("[[5, 30, 70, 65]]"), 0x0000_00ff);
    check(
        areas.as_ref().is_ok_and(|n| *n == 1) && again.text(0).is_ok_and(|text| !text.contains("Hello")),
        "pdf: redaction",
        again.text(0),
    );

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

    // A page from a scheme of the application's own, and its fetch().
    scheme::handle("rs", |request| {
        if request.url() == "rs://test/offscreen.html" {
            request.respond(
                200,
                "Content-Type: text/html",
                b"<body style='margin:0;background:rgb(0,128,255)'>\
                  <button style='position:absolute;left:10px;top:10px;width:100px;height:40px' \
                  onclick='document.title = \"clicked\"'>x</button></body>",
            );
        } else if request.url() == "rs://test/media.html" {
            request.respond(200, "Content-Type: text/html", b"<video src='clip.webm' muted></video>");
        } else if request.url() == "rs://test/clip.webm" {
            let clip = Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../tests/engine/pages/media/clip.webm")
                .canonicalize()
                .unwrap_or_default();
            request.respond_file(&clip, "Content-Type: video/webm");
        } else if request.url() == "rs://test/played.html" {
            request.respond(200, "Content-Type: text/html", b"<video src='captured.webm' muted></video>");
        } else if request.url() == "rs://test/captured.webm" {
            let captured = CAPTURED.with(|path| path.borrow().clone());
            request.respond_file(&captured, "Content-Type: video/webm");
        } else if request.url() == "rs://test/index.html" {
            request.respond(
                200,
                "Content-Type: text/html",
                b"<script>fetch('/data').then(r => r.text()).then(t => document.title = t)</script>",
            );
        } else {
            let answer = format!("{} from Rust", request.method());
            request.respond(200, "Content-Type: text/plain", answer.as_bytes());
        }
    });
    offscreen();
    media();
    let scheme_view = View::new(Some("rs://test/index.html"), 320, 240);
    {
        let closing = scheme_view.clone();
        scheme_view.on_title(move |title| {
            if title == "GET from Rust" {
                check(true, "a page from the application's scheme, and its fetch", ());
                closing.close();
            }
        });
    }

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
                view3.capture(|png| {
                    check(png.as_ref().is_ok_and(|png| png.starts_with(b"\x89PNG")), "View::capture: a PNG", ());
                });
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

/// An off-screen view: its frames, shared memory or an IOSurface, and a
/// click sent to it.
fn offscreen() {
    let Some(view) = View::new_offscreen(Some("rs://test/offscreen.html"), 200, 100, 2.0) else {
        check(false, "View::new_offscreen", "none");
        OFFSCREEN_DONE.with(|done| done.set(true));
        return;
    };
    check(view.is_offscreen(), "View::new_offscreen", ());
    let clicked = Rc::new(Cell::new(false));
    {
        let (view2, clicked) = (view.clone(), clicked.clone());
        view.on_frame(move |frame| {
            if clicked.get() {
                return;
            }
            let blue = match frame.buffer() {
                // (150, 45) in CSS pixels, at a scale of 2: B, G, R, A.
                FrameBuffer::SharedMemory { pixels, stride, .. } => {
                    let at = 90 * stride + 300 * 4;
                    pixels.get(at..at + 4) == Some(&[255, 128, 0, 255][..])
                }
                FrameBuffer::IoSurface(surface) => !surface.is_null(),
                FrameBuffer::Dmabuf { fds, .. } => !fds.is_empty(),
            };
            if !blue {
                return;
            }
            check(
                frame.width() == 400 && frame.height() == 200 && frame.scale() == 2.0 && !frame.damage().is_empty(),
                "an off-screen frame, at its scale",
                &frame,
            );
            clicked.set(true);
            view2.send_mouse(MouseEvent::Down, 50.0, 30.0, MouseButton::Left, 1, Modifiers::NONE);
            view2.send_mouse(MouseEvent::Up, 50.0, 30.0, MouseButton::Left, 1, Modifiers::NONE);
        });
    }
    {
        let view2 = view.clone();
        view.on_title(move |title| {
            if title == "clicked" {
                check(true, "a click sent to the off-screen view", ());
                page(view2.clone());
            }
        });
    }
}

/// <zebkit/page.h>: a confirm() answered by the application, the page
/// printed to PDF; then the view closes.
fn page(view: View) {
    {
        let answering = view.clone();
        view.on_dialog(move |dialog| {
            check(
                dialog.kind == zebkit::page::DialogType::Confirm && dialog.message == "rust?" && dialog.held,
                "page: a confirm, held",
                dialog,
            );
            answering.respond_dialog(dialog.id, true, None);
        });
    }
    let printing = view.clone();
    view.evaluate("confirm('rust?')", move |answer| {
        check(answer.as_deref() == Ok("true"), "page: answered OK", &answer);
        let closing = printing.clone();
        printing.print_to_pdf(r#"{"printBackground": true}"#, move |pdf| {
            check(pdf.as_ref().is_ok_and(|bytes| bytes.starts_with(b"%PDF-")), "page: printed to PDF", pdf.as_ref().map(Vec::len));
            closing.close();
            OFFSCREEN_DONE.with(|done| done.set(true));
        });
    });
}

/// Evaluates `script` until it is `"true"`, then runs `then`.
fn when_true(view: View, script: &'static str, tries: u32, then: impl FnOnce(View) + 'static) {
    let again = view.clone();
    view.evaluate(script, move |result| {
        if result.as_deref() == Ok("true") {
            then(again);
        } else if tries == 0 {
            check(false, script, &result);
            MEDIA_DONE.with(|done| done.set(true));
        } else {
            zebkit::after(Duration::from_millis(50), move || when_true(again, script, tries - 1, then));
        }
    });
}

/// <zebkit/media.h>: a WebM the page plays, captured as it plays and
/// written without being decoded.
fn media() {
    let view = View::new(Some("rs://test/media.html"), 320, 240);
    when_true(view, "document.querySelector('video').readyState >= 1", 200, |view| {
        let page = view.clone();
        view.media_list(move |list| {
            let list = list.unwrap_or_default();
            check(list.contains(r#""video":"vp9""#) && list.contains(r#""kind":"file""#), "media: the video is listed", &list);
            let id = list.split(r#""id":""#).nth(1).and_then(|rest| rest.split('"').next()).unwrap_or("").to_string();
            let path = std::env::temp_dir().join(format!("zebkit-rust-media-{}.webm", std::process::id()));
            let capture = page.media_capture_start(&id, &path, "{}");
            check(capture.is_ok(), "media: the capture starts", capture.as_ref().err());
            let Ok(capture) = capture else {
                MEDIA_DONE.with(|done| done.set(true));
                return;
            };
            let progress = Rc::new(Cell::new(0));
            {
                let progress = progress.clone();
                capture.on_progress(move |_| progress.set(progress.get() + 1));
            }
            let capture = Rc::new(capture);
            let held = Rc::new(RefCell::new(Some(capture.clone())));
            capture.on_done(move |done| {
                let written = done.split(r#""path":""#).nth(1).and_then(|rest| rest.split('"').next()).unwrap_or("");
                let size = std::fs::metadata(written).map(|m| m.len()).unwrap_or(0);
                check(
                    !done.contains(r#""error""#) && written.ends_with(".webm") && size > 100_000 && progress.get() > 0,
                    "media: the capture is written, as WebM",
                    (done, size),
                );
                // Dropped in its own done function: what runs is kept.
                held.borrow_mut().take();
                CAPTURED.with(|path| *path.borrow_mut() = written.into());
                play_back();
            });
            page.evaluate("document.querySelector('video').playbackRate = 4; document.querySelector('video').play().then(() => true)", |_| {});
            when_true(page, "document.querySelector('video').ended", 400, move |_| {
                check(capture.status().contains(r#""state":"capturing""#), "media: its status", capture.status());
                capture.stop();
            });
        });
    });
}

/// The capture, played by the engine: its FFmpeg reads it, and decodes all
/// of it.
fn play_back() {
    let view = View::new(Some("rs://test/played.html"), 320, 240);
    when_true(view, "document.querySelector('video').readyState >= 1", 200, |view| {
        let page = view.clone();
        view.evaluate(
            "(() => { const v = document.querySelector('video'); return [v.duration, v.videoWidth, v.videoHeight].join(' '); })()",
            move |result| {
                let result = result.unwrap_or_default();
                let numbers: Vec<f64> = result.trim_matches('"').split(' ').filter_map(|n| n.parse().ok()).collect();
                check(
                    numbers.len() == 3 && numbers[0] > 7.4 && numbers[0] < 8.6 && numbers[1] == 320.0 && numbers[2] == 180.0,
                    "media: the engine reads the capture back, 8 s of 320x180",
                    &result,
                );
                page.evaluate("document.querySelector('video').playbackRate = 4; document.querySelector('video').play().then(() => true)", |_| {});
                when_true(page, "document.querySelector('video').ended", 400, |page| {
                    let closing = page.clone();
                    page.evaluate(
                        "(() => { const v = document.querySelector('video'); \
                         return [v.getVideoPlaybackQuality().totalVideoFrames, v.webkitAudioDecodedByteCount, !!v.error].join(' '); })()",
                        move |result| {
                            let result = result.unwrap_or_default();
                            let parts: Vec<&str> = result.trim_matches('"').split(' ').collect();
                            let frames: u32 = parts.first().and_then(|n| n.parse().ok()).unwrap_or(0);
                            let audio: u64 = parts.get(1).and_then(|n| n.parse().ok()).unwrap_or(0);
                            check(
                                frames >= 180 && audio > 0 && parts.get(2) == Some(&"false"),
                                "media: and plays all of it, video and audio",
                                &result,
                            );
                            let _ = std::fs::remove_file(CAPTURED.with(|path| path.borrow().clone()));
                            closing.close();
                            MEDIA_DONE.with(|done| done.set(true));
                        },
                    );
                });
            },
        );
    });
}

fn script_and_cdp(view: View) {
    let script = Script::new(&["one", "two"]);
    let script = Rc::new(script);
    let held = script.clone();
    script.evaluate("zebkit.args.join('+') + ' ' + typeof zebkit.View", move |result| {
        check(result.as_deref() == Ok(r#""one+two function""#), "script outside any page", &result);
        drop(held);
    });

    let refused = zebkit::app::set_menu(r#"[{"title": "Edit", "items": [{"role": "juggle"}]}]"#, |_| {});
    check(refused.as_ref().is_err_and(|e| e.message().contains("juggle")), "app: a menu with an unknown role is refused", &refused);
    let menu = zebkit::app::set_menu(
        r#"[{"title": "File", "items": [{"title": "Open", "id": "open", "key": "o"}, {"separator": true}, {"role": "quit"}]}]"#,
        |_| {},
    );
    check(menu.is_ok(), "app: the menu", &menu);
    zebkit::app::on_open_files(|_| {});
    zebkit::app::on_reopen(|| {});

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
        if !client.is_finished() || !OFFSCREEN_DONE.with(Cell::get) || !MEDIA_DONE.with(Cell::get) {
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

// Recompiled ad hoc test binaries must not ask to read an older binary's
// Cookie key. Each invocation removes only the throwaway key it owns.
fn cleanup_test_key(identity: &str) {
    #[cfg(target_os = "macos")]
    {
        let _ = std::process::Command::new("/usr/bin/security")
            .args(["delete-generic-password", "-s", &format!("Zebkit Cookies: {identity}"), "-a", identity])
            .stdout(std::process::Stdio::null())
            .stderr(std::process::Stdio::null())
            .status();
    }
    #[cfg(not(target_os = "macos"))]
    let _ = identity;
}

fn main() {
    if std::env::args().any(|arg| arg == "--services-only") {
        services();
        std::process::exit(if FAILED.with(Cell::get) == 0 { 0 } else { 1 });
    }
    // Exercise standalone services separately: Content must register its
    // schemes before this process parses URLs or initializes ICU.
    let standalone = std::process::Command::new(std::env::current_exe().unwrap())
        .arg("--services-only")
        .status()
        .expect("standalone service test starts");
    check(standalone.success(), "standalone services process", standalone);

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
    let nonce = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
    let identity = format!("zebkit-rust-test-{}-{nonce}", std::process::id());
    let profile = std::env::temp_dir().join(&identity);
    let args = [
        identity.clone(),
        "--headless".to_string(),
        "--no-sandbox".to_string(),
        "--autoplay-policy=no-user-gesture-required".to_string(),
        format!("--user-data-dir={}", profile.display()),
    ];
    // Ends on its own; a test that hangs is a failure too.
    let watchdog_identity = identity.clone();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(120));
        eprintln!("FAIL  the test did not finish in 120 s");
        cleanup_test_key(&watchdog_identity);
        std::process::exit(1);
    });
    check(scheme::register("rs", scheme::STANDARD | scheme::FETCH), "scheme::register", ());
    // The media test lists the page's video once it has shown its first
    // frames: kept from the start, its capture still begins there.
    zebkit::media::keep_history(true);
    let code = zebkit::run_with_args(args, move || {
        services();
        engine(port);
    });
    let _ = std::fs::remove_dir_all(&profile);
    cleanup_test_key(&identity);
    check(code == 0, "the engine's exit code", code);

    let (passed, failed) = (PASSED.with(Cell::get), FAILED.with(Cell::get));
    println!("{passed} passed, {failed} failed");
    std::process::exit(if failed == 0 { 0 } else { 1 });
}
