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
use zebkit::offscreen::{Buffers, FrameBuffer, Modifiers, MouseButton, MouseEvent, PixelFormat};
use zebkit::{cdp, scheme, Allow, LoadEvent, Script, View, PermissionState, NavigationAction};

thread_local! {
    static OFFSCREEN_DONE: Cell<bool> = const { Cell::new(false) };
    static CONTEXTS_DONE: Cell<bool> = const { Cell::new(false) };
    static MEDIA_DONE: Cell<bool> = const { Cell::new(false) };
    static NAVIGATION_DONE: Cell<bool> = const { Cell::new(false) };
    static CONSOLE_DONE: Cell<bool> = const { Cell::new(false) };
    static WINDOW_DONE: Cell<bool> = const { Cell::new(false) };
    static PERMISSION_DONE: Cell<bool> = const { Cell::new(false) };
    static AUTHENTICATION_DONE: Cell<bool> = const { Cell::new(false) };
    static HANDLERS_DONE: Cell<bool> = const { Cell::new(false) };
    static AUTOMATION_DONE: Cell<bool> = const { Cell::new(false) };
    static FIND_DONE: Cell<bool> = const { Cell::new(false) };
    static NEW_API_DONE: Cell<bool> = const { Cell::new(false) };
    static LIBRARY_DONE: Cell<bool> = const { Cell::new(false) };
    static CONNECTIONS_DONE: Cell<bool> = const { Cell::new(false) };
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

/// The engine's FFmpeg on a file, with no engine running.
fn media_tools() {
    use zebkit::media_tools::{self, Completion, VideoOptions};
    let clip = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/engine/pages/media/clip.webm");
    let clip = clip.to_string_lossy();
    let probed = media_tools::probe(&clip);
    check(probed.as_ref().is_ok_and(|json| json.contains("\"streams\"") && json.contains("vp9")), "media_tools::probe", &probed);
    check(media_tools::probe("/nothing/here.webm").is_err(), "media_tools::probe: no file is an error", ());
    let out = std::env::temp_dir().join(format!("zebkit-rust-frames-{}.rgba", std::process::id()));
    let options = VideoOptions { size: Some((16, 8)), frames: Some(2), overwrite: true, ..Default::default() };
    let done = media_tools::extract_video_with(&clip, &out.to_string_lossy(), &options, |_, _| true);
    let size = std::fs::metadata(&out).map(|data| data.len()).unwrap_or(0);
    check(done == Ok(Completion::Completed) && size == 16 * 8 * 4 * 2, "media_tools::extract_video: raw RGBA frames", (&done, size));
    let _ = std::fs::remove_file(&out);
}

/// What needs no engine running.
fn services() {
    media_tools();
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
    let image = services::Image { width: 2, height: 1, rgba: vec![255, 0, 0, 255, 0, 0, 255, 128].into() };
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
    document.set_outline(r#"[{"title":"Rust book π","page":0,"open":true,"children":[{"title":"Child","url":"https://example.com"}]}]"#).unwrap();
    let outline = document.outline().unwrap();
    check(document.set_outline(r#"[{"title":"Invalid","page":9}]"#).is_err()
        && document.outline().unwrap() == outline, "pdf: invalid outline replacement is atomic", ());
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
    check(again.outline().unwrap() == outline, "pdf: edited outline persists", again.outline());
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

/// tests/engine/pages over HTTP, and `/download.bin` as an attachment.
fn serve_pages() -> u16 {
    let pages = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/engine/pages");
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let port = listener.local_addr().unwrap().port();
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            let pages = pages.clone();
            std::thread::spawn(move || {
                let mut reader = BufReader::new(stream);
                let mut first = String::new();
                if reader.read_line(&mut first).is_err() {
                    return;
                }
                let mut line = String::new();
                while reader.read_line(&mut line).is_ok_and(|read| read > 2) {
                    line.clear();
                }
                let path = first.split(' ').nth(1).unwrap_or("/").split(['?', '#']).next().unwrap_or("/").to_string();
                let mut stream = reader.into_inner();
                let (status, kind, extra, body) = if path == "/download.bin" {
                    ("200 OK", "application/octet-stream", "Content-Disposition: attachment; filename=\"report.bin\"\r\n", b"zebkit download".to_vec())
                } else {
                    let file = pages.join(path.trim_start_matches('/'));
                    let kind = match file.extension().and_then(|e| e.to_str()) {
                        Some("html") => "text/html; charset=utf-8",
                        Some("json") => "application/json",
                        Some("js") => "text/javascript",
                        _ => "application/octet-stream",
                    };
                    match std::fs::read(&file) {
                        Ok(body) if file.starts_with(&pages) => ("200 OK", kind, "", body),
                        _ => ("404 Not Found", "text/plain", "", b"not found".to_vec()),
                    }
                };
                let _ = write!(stream, "HTTP/1.1 {status}\r\nContent-Type: {kind}\r\n{extra}Content-Length: {}\r\nConnection: close\r\n\r\n", body.len());
                let _ = stream.write_all(&body);
            });
        }
    });
    port
}

type Next = Box<dyn FnOnce()>;
type Step = Box<dyn FnOnce(zebkit::automation::Page, Next)>;

/// Runs `steps` one after the other, each with the page and what comes next.
fn chain(page: zebkit::automation::Page, steps: Vec<Step>, end: impl FnOnce() + 'static) {
    let mut next: Next = Box::new(end);
    for step in steps.into_iter().rev() {
        let (page, after) = (page.clone(), next);
        next = Box::new(move || step(page, after));
    }
    next();
}

/// zebkit::automation, as tests/engine/automation_python.py drives
/// zebkit.automation, on tests/engine/pages/automation.html: locators,
/// actions, expect, handles, routes and requests, responses, dialogs,
/// exposed functions, the clock, screenshots, workers, tracing, downloads,
/// file choosers, the console, contexts of their own and the endpoint.
fn automation_api(port: u16) {
    use zebkit::automation::{self as auto, expect, EventKind, Filter, PageEvent, RoleOptions, SelectOption, Text};
    use zebkit::json::Value;
    let base = format!("http://127.0.0.1:{port}");
    let page = match auto::Page::open(1280, 800) {
        Ok(page) => page,
        Err(error) => {
            check(false, "automation: Page::open", error);
            AUTOMATION_DONE.with(|done| done.set(true));
            return;
        }
    };
    let requests = Rc::new(RefCell::new(Vec::<String>::new()));
    let finished = Rc::new(RefCell::new(Vec::<String>::new()));
    {
        let (requests, finished) = (requests.clone(), finished.clone());
        page.on(EventKind::Request, move |event| {
            if let PageEvent::Request(request) = event {
                requests.borrow_mut().push(format!("{} {}", request.method(), request.url()));
            }
        });
        page.on(EventKind::RequestFinished, move |event| {
            if let PageEvent::RequestFinished(request) = event {
                finished.borrow_mut().push(request.url().to_string());
            }
        });
    }
    let url = format!("{base}/automation.html");
    let data_url = format!("{base}/data.json");
    let download_url = format!("{base}/download.bin");
    let context_base = base.clone();
    let steps: Vec<Step> = vec![
        Box::new(move |page, next| {
            let reading = page.clone();
            page.goto(&url, move |response| {
                let response = response.ok().flatten();
                check(response.as_ref().is_some_and(|r| r.status() == 200 && r.ok()), "automation: goto's response", response.as_ref().map(|r| r.status()));
                let titled = reading.clone();
                let Some(response) = response else { return next() };
                response.text(move |body| {
                    check(body.as_ref().is_ok_and(|body| body.contains("<title>Automation</title>")), "automation: Response::text", body.as_ref().map(|b| b.len()));
                    titled.title(move |title| {
                        check(title.as_deref() == Ok("Automation"), "automation: title", &title);
                        next();
                    });
                });
            });
        }),
        Box::new(|page, next| {
            let out = page.locator("#out");
            page.get_by_role("button", RoleOptions::name("Go")).click(move |clicked| {
                out.text_content(move |text| {
                    check(clicked.is_ok() && text.as_ref().is_ok_and(|t| t.as_deref() == Some("clicked 1")), "automation: get_by_role().click()", &text);
                    next();
                });
            });
        }),
        Box::new(|page, next| {
            let again = page.clone();
            expect(&page.locator("#out")).to_have_text("clicked 1", move |held| {
                check(held.is_ok(), "automation: expect().to_have_text()", &held);
                expect(&again.locator("#out")).timeout(300.0).to_have_text("nope", move |failed| {
                    check(failed.as_ref().is_err_and(|e| e.message().contains("timed out")), "automation: a failing expect is an error", &failed);
                    next();
                });
            });
        }),
        Box::new(|page, next| {
            let name = page.locator("#name");
            page.get_by_placeholder("Name", false).fill("Grace", move |_| {
                let typed = name.clone();
                name.press("End", move |_| {
                    let read = typed.clone();
                    typed.press_sequentially(" H", move |_| {
                        read.input_value(move |value| {
                            check(value.as_deref() == Ok("Grace H"), "automation: fill(), press(), press_sequentially()", &value);
                            next();
                        });
                    });
                });
            });
        }),
        Box::new(|page, next| {
            let items = page.locator(".item");
            let (texts, filtered, last) = (items.clone(), items.filter(&Filter { has_text: Some("two".into()), ..Default::default() }), items.last());
            let agree = page.get_by_text("I agree", false);
            let checked = page.locator("#agree");
            items.count(move |count| {
                check(count == Ok(3), "automation: count()", &count);
                texts.all_text_contents(move |all| {
                    check(all.as_ref().is_ok_and(|all| all == &["one", "two", "three"]), "automation: all_text_contents()", &all);
                    filtered.text_content(move |two| {
                        check(two == Ok(Some("two".to_string())), "automation: filter(has_text)", &two);
                        last.text_content(move |three| {
                            check(three == Ok(Some("three".to_string())), "automation: last()", &three);
                            let checking = agree.clone();
                            agree.is_visible(move |visible| {
                                check(visible == Ok(true), "automation: get_by_text().is_visible()", &visible);
                                checking.check(move |_| {
                                    checked.is_checked(move |is| {
                                        check(is == Ok(true), "automation: check() through its label", &is);
                                        next();
                                    });
                                });
                            });
                        });
                    });
                });
            });
        }),
        Box::new(|page, next| {
            let deep = page.frame_locator("#inner").locator("#deep");
            let also = page.locator("#inner").content_frame().locator("#deep");
            page.locator("#color").select_option(&[SelectOption::Label("Blue".into())], move |values| {
                check(values.as_ref().is_ok_and(|v| v == &["b"]), "automation: select_option(label)", &values);
                let reading = deep.clone();
                deep.click(move |_| {
                    reading.text_content(move |text| {
                        check(text == Ok(Some("pressed".to_string())), "automation: frame_locator().click()", &text);
                        also.text_content(move |same| {
                            check(same == Ok(Some("pressed".to_string())), "automation: Locator::content_frame()", &same);
                            next();
                        });
                    });
                });
            });
        }),
        Box::new(|page, next| {
            let evaluating = page.clone();
            page.query_selector("#go", move |handle| {
                let Ok(Some(handle)) = handle else {
                    check(false, "automation: query_selector()", handle.err());
                    return next();
                };
                let (boxed, arg) = (handle.clone(), handle.as_arg());
                handle.text_content(move |text| {
                    check(text == Ok(Some("Go".to_string())), "automation: ElementHandle::text_content()", &text);
                    boxed.bounding_box(move |found| {
                        check(found.as_ref().is_ok_and(|b| b.is_some_and(|b| b.width > 0.0)), "automation: ElementHandle::bounding_box()", &found);
                        evaluating.evaluate("(b) => b.tagName", arg, move |tag| {
                            check(tag.as_ref().is_ok_and(|t| t.as_str() == Some("BUTTON")), "automation: evaluate() with a handle", &tag);
                            next();
                        });
                    });
                });
            });
        }),
        Box::new(|page, next| {
            page.locator("#nothing").with_timeout(300.0).click(move |result| {
                check(result.as_ref().is_err_and(|e| e.is_timeout() && e.message().contains("Timeout 300ms exceeded")), "automation: a timeout is Error::is_timeout()", &result);
                next();
            });
        }),
        Box::new(|page, next| {
            let seen = Rc::new(RefCell::new(Vec::new()));
            let (seen2, listening) = (seen.clone(), page.clone());
            let id = page.on(EventKind::Dialog, move |event| {
                if let PageEvent::Dialog(dialog) = event {
                    seen2.borrow_mut().push((dialog.kind().to_string(), dialog.message().to_string()));
                    dialog.accept(None, |_| {});
                }
            });
            page.evaluate("() => new Promise(r => setTimeout(() => r(confirm('Proceed?')), 0))", Value::Null, move |answer| {
                check(answer == Ok(Value::Bool(true)) && *seen.borrow() == [("confirm".to_string(), "Proceed?".to_string())],
                    "automation: on(Dialog) and Dialog::accept()", (&answer, seen.borrow()));
                listening.remove_listener(id);
                next();
            });
        }),
        Box::new(|page, next| {
            // The only listener gone (wait_for_event's after its event, one
            // removed, one cancelled): a dialog no longer holds the page.
            let answered = Rc::new(Cell::new(false));
            let (seen, after) = (answered.clone(), page.clone());
            page.wait_for_event(EventKind::Dialog, |_| true, move |event| {
                if let PageEvent::Dialog(dialog) = event {
                    dialog.accept(None, |_| {});
                }
                seen.set(true);
            });
            let cancelled = page.wait_for_event(EventKind::FileChooser, |_| true, |_| {});
            page.remove_listener(cancelled);
            page.evaluate("() => { setTimeout(() => alert('one'), 0); }", Value::Null, move |_| {
                when(move || answered.get(), 100, move |got| {
                    check(got, "automation: wait_for_event(Dialog) returns", ());
                    let result = Rc::new(RefCell::new(None));
                    let into = result.clone();
                    after.evaluate("() => { alert('two'); const c = confirm('three'); return 'after ' + c; }", Value::Null, move |answer| {
                        *into.borrow_mut() = Some(answer);
                    });
                    let chosen = Rc::new(RefCell::new(None));
                    let (into, choosing) = (chosen.clone(), after.clone());
                    let polled = result.clone();
                    when(move || polled.borrow().is_some(), 100, move |_| {
                        let answer = result.borrow_mut().take();
                        check(answer == Some(Ok(Value::from("after false"))), "automation: a dialog after the last listener went is dismissed", &answer);
                        let clicking = choosing.clone();
                        choosing.evaluate("() => { const i = document.querySelector('#upload'); window.__closed = null; i.oncancel = () => window.__closed = 'cancel'; i.onchange = () => window.__closed = 'change'; }", Value::Null, move |_| {
                            clicking.locator("#upload").click(|_| {});
                            let asking = clicking.clone();
                            poll_value(asking, "() => window.__closed", 100, move |answer| {
                                *into.borrow_mut() = Some(answer);
                            });
                        });
                        let polled = chosen.clone();
                        when(move || polled.borrow().is_some(), 150, move |_| {
                            let answer = chosen.borrow_mut().take();
                            check(answer.as_ref().is_some_and(|a| !a.is_null()), "automation: a file chooser after its listener was cancelled is not held", &answer);
                            next();
                        });
                    });
                });
            });
        }),
        Box::new(move |page, next| {
            let routed = Rc::new(RefCell::new(None));
            let seen = routed.clone();
            let fetching = page.clone();
            page.route(
                "**/data.json",
                move |route, request| {
                    *seen.borrow_mut() = Some(format!("{} {}", request.method(), request.url()));
                    route.fulfill_json(&Value::object([("from", "route".into())]), |_| {});
                },
                move |set| {
                    check(set.is_ok(), "automation: route()", &set);
                    fetching.evaluate("() => fetch('data.json').then(r => r.json())", Value::Null, move |got| {
                        check(got.as_ref().is_ok_and(|got| got["from"].as_str() == Some("route")), "automation: route() and Route::fulfill_json()", &got);
                        check(routed.borrow().as_deref() == Some(&format!("GET {data_url}")[..]), "automation: the route's Request", routed.borrow());
                        next();
                    });
                },
            );
        }),
        Box::new(|page, next| {
            let calling = page.clone();
            page.expose_function(
                "add",
                |args| Ok(Value::from(args.iter().filter_map(Value::as_f64).sum::<f64>())),
                move |_| {
                    calling.evaluate("() => add(20, 22)", Value::Null, move |total| {
                        check(total.as_ref().is_ok_and(|t| t.as_i64() == Some(42)), "automation: expose_function()", &total);
                        next();
                    });
                },
            );
        }),
        Box::new(|page, next| {
            let (clock, reading) = (page.clock(), page.clone());
            page.clock().install(Some(0.0.into()), move |_| {
                clock.set_fixed_time(5000.0, move |_| {
                    reading.evaluate("() => Date.now()", Value::Null, move |now| {
                        check(now.as_ref().is_ok_and(|n| n.as_i64() == Some(5000)), "automation: Clock::set_fixed_time()", &now);
                        next();
                    });
                });
            });
        }),
        Box::new(|page, next| {
            let list = page.locator("ul");
            let expecting = list.clone();
            list.aria_snapshot(move |snapshot| {
                check(snapshot.as_ref().is_ok_and(|s| s.contains("- listitem: one")), "automation: aria_snapshot()", &snapshot);
                expect(&expecting).to_match_aria_snapshot("- list:\n  - listitem: one\n  - listitem: two\n  - listitem: three", move |matched| {
                    check(matched.is_ok(), "automation: to_match_aria_snapshot()", &matched);
                    next();
                });
            });
        }),
        Box::new(|page, next| {
            let (jpeg, pixels, element, boxed) = (page.clone(), page.clone(), page.locator("#go"), page.locator("#go"));
            page.screenshot(Value::Null, move |png| {
                check(png.as_ref().is_ok_and(|png| png.starts_with(b"\x89PNG\r\n\x1a\n")), "automation: screenshot()", png.as_ref().map(Vec::len));
                jpeg.screenshot(Value::object([("type", "jpeg".into()), ("quality", 60.into())]), move |jpeg| {
                    check(jpeg.as_ref().is_ok_and(|jpeg| jpeg.starts_with(b"\xff\xd8\xff")), "automation: screenshot(type jpeg)", jpeg.as_ref().map(Vec::len));
                    pixels.screenshot_pixels(Value::Null, move |pixels| {
                        check(pixels.as_ref().is_ok_and(|p| (p.width, p.height, p.stride, p.data.len()) == (1280, 800, 1280 * 4, 1280 * 800 * 4)),
                            "automation: screenshot_pixels()", pixels.as_ref().map(|p| (p.width, p.height)));
                        element.screenshot_pixels(Value::Null, move |shot| {
                            let size = shot.as_ref().map(|p| (p.width, p.height)).ok();
                            boxed.bounding_box(move |found| {
                                // The box's enclosing pixels, as screenshotter.ts takes an element's.
                                let enclosing = found.ok().flatten().map(|b| (((b.x + b.width).ceil() - b.x.floor()) as u32, ((b.y + b.height).ceil() - b.y.floor()) as u32));
                                check(size.is_some() && size == enclosing, "automation: Locator::screenshot_pixels() is the element's", (size, enclosing));
                                next();
                            });
                        });
                    });
                });
            });
        }),
        Box::new(|page, next| {
            page.wait_for_event(EventKind::Worker, |_| true, move |event| {
                let PageEvent::Worker(worker) = event else { return next() };
                let closed = Rc::new(Cell::new(false));
                let seen = closed.clone();
                worker.on_close(move |_| seen.set(true));
                worker.evaluate("self.x * 6", Value::Null, move |value| {
                    check(value.as_ref().is_ok_and(|v| v.as_i64() == Some(42)), "automation: the Worker event, Worker::evaluate()", &value);
                    let _ = closed;
                    next();
                });
            });
            page.evaluate("() => { window.w = new Worker(URL.createObjectURL(new Blob(['self.x = 7']))); }", Value::Null, |_| {});
        }),
        Box::new(|page, next| {
            let (tracing, clicking) = (page.tracing(), page.locator("#go"));
            let zip = std::env::temp_dir().join(format!("zebkit-rust-trace-{}.zip", std::process::id()));
            page.tracing().start(None, None, true, false, move |started| {
                check(started.is_ok(), "automation: Tracing::start()", &started);
                clicking.click(move |_| {
                    let path = zip.to_string_lossy().into_owned();
                    tracing.stop(Some(&path), move |written| {
                        let head = std::fs::read(&zip).map(|bytes| bytes[..2.min(bytes.len())].to_vec()).unwrap_or_default();
                        let _ = std::fs::remove_file(&zip);
                        check(written.as_ref().is_ok_and(Option::is_some) && head == b"PK", "automation: Tracing::stop() writes the zip", &written);
                        next();
                    });
                });
            });
        }),
        Box::new(|page, next| {
            let messages = Rc::new(RefCell::new(Vec::new()));
            let seen = messages.clone();
            page.on(EventKind::Console, move |event| {
                if let PageEvent::Console(message) = event {
                    seen.borrow_mut().push((message.kind().to_string(), message.text().to_string()));
                }
            });
            page.evaluate("() => console.warn('from Rust', 7)", Value::Null, move |_| {
                when(move || messages.borrow().iter().any(|(kind, text)| kind == "warning" && text == "from Rust 7"), 100, move |found| {
                    check(found, "automation: on(Console), ConsoleMessage", ());
                    next();
                });
            });
        }),
        Box::new(|page, next| {
            let chosen = Rc::new(Cell::new(false));
            let seen = chosen.clone();
            let files = page.clone();
            let file = Path::new(env!("CARGO_MANIFEST_DIR")).join("Cargo.toml").to_string_lossy().into_owned();
            let id = page.on(EventKind::FileChooser, move |event| {
                if let PageEvent::FileChooser(chooser) = event {
                    seen.set(chooser.is_multiple());
                    chooser.set_files(&[&file], |_| {});
                }
            });
            page.locator("#upload").click(move |_| {
                when(move || chosen.get(), 100, move |multiple| {
                    let removing = files.clone();
                    files.evaluate("() => [...document.querySelector('#upload').files].map(f => f.name)", Value::Null, move |names| {
                        check(multiple && names.as_ref().is_ok_and(|n| n[0].as_str() == Some("Cargo.toml")), "automation: on(FileChooser), FileChooser::set_files()", &names);
                        removing.remove_listener(id);
                        next();
                    });
                });
            });
        }),
        Box::new(move |page, next| {
            let link = format!("() => {{ const a = document.createElement('a'); a.href = {}; a.download = 'report.bin'; document.body.append(a); a.click(); }}", zebkit::json::quote(&download_url));
            page.wait_for_event(EventKind::Download, |_| true, move |event| {
                let PageEvent::Download(download) = event else { return next() };
                check(download.suggested_filename() == "report.bin" && download.url().ends_with("/download.bin"), "automation: the Download event", (download.suggested_filename(), download.url()));
                let failing = download.clone();
                download.path(move |path| {
                    let body = path.as_ref().ok().cloned().flatten().and_then(|path| std::fs::read(path).ok());
                    check(body.as_deref() == Some(&b"zebkit download"[..]), "automation: Download::path() has the file", &path);
                    failing.failure(move |failure| {
                        check(failure == Ok(None), "automation: Download::failure() of a finished one", &failure);
                        next();
                    });
                });
            });
            page.evaluate(&link, Value::Null, |_| {});
        }),
        Box::new(move |page, next| {
            let mine = page.clone();
            auto::new_context(Value::object([("viewport", Value::object([("width", 640.into()), ("height", 480.into())])), ("userAgent", "zebkit-rust-context".into())]), move |context| {
                let Ok(context) = context else {
                    check(false, "automation: new_context()", context.err());
                    return next();
                };
                check(!context.id().is_empty(), "automation: BrowserContext::id()", context.id());
                let held = context.clone();
                context.new_page(move |other| {
                    let Ok(other) = other else {
                        check(false, "automation: BrowserContext::new_page()", other.err());
                        return next();
                    };
                    other.evaluate("() => navigator.userAgent", Value::Null, move |agent| {
                        check(agent.as_ref().is_ok_and(|a| a.as_str() == Some("zebkit-rust-context")), "automation: new_context(userAgent)", &agent);
                        let cookie = Value::Array(vec![Value::object([("name", "only".into()), ("value", "x".into()), ("url", format!("{context_base}/").into())])]);
                        let reading = held.clone();
                        held.add_cookies(cookie, move |_| {
                            let url = format!("{context_base}/");
                            let url2 = url.clone();
                            let asking = reading.clone();
                            asking.cookies(&[&url], move |theirs| {
                                mine.cookies(&[&url2], move |ours| {
                                    let has = |list: &Result<Vec<Value>, zebkit::Error>| list.as_ref().is_ok_and(|l| l.iter().any(|c| c["name"].as_str() == Some("only")));
                                    check(has(&theirs) && !has(&ours) && reading.pages().len() == 1, "automation: a context's cookies are its own", ());
                                    reading.close(move |closed| {
                                        check(closed.is_ok(), "automation: BrowserContext::close()", &closed);
                                        next();
                                    });
                                });
                            });
                        });
                    });
                });
            });
        }),
        // 3dfb35a: a dialog a native click opens while the application holds
        // the view's dialogs stays the application's, told to the automation
        // client too; the click is done once the application has answered.
        Box::new(|_, next| {
            let Ok(page) = auto::Page::open(320, 200) else {
                check(false, "automation: a page for the held dialog", ());
                return next();
            };
            let told = Rc::new(RefCell::new(Vec::new()));
            let seen = told.clone();
            page.on_protocol_event(move |event, params| {
                if event == "BrowserContext.dialog" {
                    seen.borrow_mut().push(params["message"].as_str().unwrap_or_default().to_string());
                }
            });
            let held = Rc::new(Cell::new(false));
            let (answering, holding) = (page.view().clone(), held.clone());
            page.view().on_dialog(move |dialog| {
                holding.set(dialog.held);
                let (view, id) = (answering.clone(), dialog.id);
                zebkit::after(Duration::from_millis(100), move || {
                    view.respond_dialog(id, true, None);
                });
            });
            let (clicking, reading, closing) = (page.locator("#confirm"), page.locator("#out"), page.view().clone());
            page.set_content("<button id=confirm onclick=\"out.textContent = confirm('Proceed?') ? 'yes' : 'no'\">Confirm</button><p id=out></p>", Value::Null, move |_| {
                clicking.click(move |clicked| {
                    reading.text_content(move |text| {
                        check(clicked.is_ok() && held.get() && text == Ok(Some("yes".to_string())) && *told.borrow() == ["Proceed?"],
                            "automation: a click's dialog held for the application, told to the client", (&clicked, held.get(), &text, told.borrow()));
                        closing.close();
                        next();
                    });
                });
            });
        }),
        Box::new(|_, next| {
            auto::listen(0, move |url| {
                check(url.as_ref().is_ok_and(|u| u.starts_with("ws://127.0.0.1:") && u.rsplit('/').next().is_some_and(|t| t.len() >= 32)),
                    "automation: listen() serves Playwright's protocol, with its token", &url);
                next();
            });
        }),
    ];
    let (closing, requests_seen, finished_seen) = (page.clone(), requests.clone(), finished.clone());
    chain(page, steps, move || {
        check(requests_seen.borrow().iter().any(|r| r.starts_with("GET ") && r.ends_with("/automation.html")), "automation: on(Request)", requests_seen.borrow().len());
        check(finished_seen.borrow().iter().any(|url| url.ends_with("/automation.html")), "automation: on(RequestFinished) names the request", finished_seen.borrow().len());
        check(Text::regex("a/b", "i") != Text::from("x"), "automation: Text", ());
        closing.view().close();
        AUTOMATION_DONE.with(|done| done.set(true));
    });
}

/// `expression`'s value once it is not null, polling, or null after `tries`.
fn poll_value(page: zebkit::automation::Page, expression: &'static str, tries: u32, then: impl FnOnce(zebkit::json::Value) + 'static) {
    let again = page.clone();
    page.evaluate(expression, zebkit::json::Value::Null, move |value| {
        let value = value.unwrap_or_default();
        if !value.is_null() || tries == 0 {
            then(value);
            return;
        }
        zebkit::after(Duration::from_millis(30), move || poll_value(again, expression, tries - 1, then));
    });
}

/// Calls `then(true)` once `condition` holds, polling, or `then(false)` after `tries`.
fn when(condition: impl Fn() -> bool + 'static, tries: u32, then: impl FnOnce(bool) + 'static) {
    if condition() || tries == 0 {
        then(condition());
        return;
    }
    zebkit::after(Duration::from_millis(30), move || when(condition, tries - 1, then));
}

/// Find in page (3dfb35a): the count and the active match, next and
/// backward, the last result, stopping; and a download's report: complete,
/// with the saved path.
fn find_and_downloads(port: u16) {
    use zebkit::page::{FindResult, FindStop, FIND_BACKWARD};
    let view = View::new(None, 400, 300);
    let results = Rc::new(RefCell::new(Vec::<FindResult>::new()));
    {
        let results = results.clone();
        view.on_find(move |result| results.borrow_mut().push(result.clone()));
    }
    let reports = Rc::new(RefCell::new(Vec::<zebkit::json::Value>::new()));
    {
        let reports = reports.clone();
        view.on_download_progress(move |json| reports.borrow_mut().push(zebkit::json::Value::parse(json).unwrap_or_default()));
    }
    let save = std::env::temp_dir().join(format!("zebkit-rust-download-{}.bin", std::process::id()));
    {
        let save = save.to_string_lossy().into_owned();
        view.on_download(move |request| {
            let saved = request.try_save(&save);
            check(saved.is_ok(), "downloads: DownloadRequest::try_save", &saved);
        });
    }
    let page = format!("http://127.0.0.1:{port}/page.html");
    let ready = view.clone();
    let mut once = Some((results, reports, save));
    view.on_load(move |event, _| {
        if event != LoadEvent::Finished {
            return;
        }
        let Some((results, reports, save)) = once.take() else { return };
        ready.clear_load_handler();
        let finding = ready.clone();
        ready.evaluate("document.body.innerHTML = '<p>apple pie</p><p>apple tart</p><p>an apple</p>'; true", move |_| {
            let refused = finding.find("app\0le", 0);
            check(refused.as_ref().is_err_and(|e| e.message().contains("NUL")), "find: text with a NUL is an error", &refused);
            let first = finding.find("apple", 0).unwrap_or(0);
            let (seen, view) = (results.clone(), finding.clone());
            poll(move || seen.borrow().iter().any(|r| r.request_id == first && r.final_update), 100, move || {
                let last = results.borrow().iter().rev().find(|r| r.request_id == first).cloned();
                check(first > 0 && last.as_ref().is_some_and(|r| r.matches == 3 && r.active_match == 1 && r.text == "apple" && r.rect.is_some()),
                    "find: three matches, the first active", &last);
                let back = view.find("apple", FIND_BACKWARD).unwrap_or(0);
                let (results, view) = (results.clone(), view.clone());
                poll({ let results = results.clone(); move || results.borrow().iter().any(|r| r.request_id == back && r.final_update) }, 100, move || {
                    let result = view.find_result();
                    check(result.as_ref().is_some_and(|r| r.request_id == back && r.active_match == 3 && !r.stopped),
                        "find: backward goes around; find_result() is the last", &result);
                    view.stop_finding(FindStop::Keep);
                    let stopped = view.clone();
                    poll(move || stopped.find_result().is_some_and(|r| r.stopped), 100, move || {
                        check(view.find_result().is_some_and(|r| r.stopped) && view.find("", 0) == Ok(0), "find: stop_finding(); empty text finds nothing", view.find_result());
                        view.clear_find_handler();
                        downloads(view, port, reports, save);
                    });
                });
            });
        });
    });
    view.load_url(&page);
}

fn downloads(view: View, port: u16, reports: Rc<RefCell<Vec<zebkit::json::Value>>>, save: std::path::PathBuf) {
    view.load_url(&format!("http://127.0.0.1:{port}/download.bin"));
    let seen = reports.clone();
    poll(move || seen.borrow().iter().any(|r| r["outcome"].as_str() == Some("complete")), 200, move || {
        let whole = reports.borrow().iter().find(|r| r["outcome"].as_str() == Some("complete")).cloned().unwrap_or_default();
        let saved = whole["savedPath"].as_str().map(str::to_string).unwrap_or_default();
        check(whole["state"].as_str() == Some("complete") && whole["error"].is_null() && std::fs::read(&saved).ok().as_deref() == Some(&b"zebkit download"[..]),
            "downloads: complete, with savedPath and no error", &whole);
        check(reports.borrow().iter().filter(|r| r["outcome"].as_str() == Some("in-progress")).all(|r| r["savedPath"].is_null()),
            "downloads: no savedPath while in progress", ());
        let _ = std::fs::remove_file(&save);
        view.close();
        FIND_DONE.with(|done| done.set(true));
    });
}

/// Calls `then` once `condition` holds, polling; a failed check after `tries`.
/// The C API's newer functions: a Call kept and answered later, from another
/// thread; one dropped unanswered (zebkit_call_free); the _async forms; the
/// off-screen constructor's error; a process gone; a refused user agent
/// override answered once; the export callback of an off-screen view.
fn new_api() {
    let missing = std::env::temp_dir().join("zebkit-rust-not-there").join("nothing.txt");
    let told = Rc::new(Cell::new(0));
    let (one, two) = (told.clone(), told.clone());
    zebkit::system::open_external_async("javascript:alert(1)", move |result| {
        check(result.is_err(), "system::open_external_async(): a javascript: URL refused", &result);
        one.set(one.get() + 1);
    });
    zebkit::system::show_in_folder_async(&missing.to_string_lossy(), move |result| {
        check(result.is_err(), "system::show_in_folder_async(): a file that is not there", &result);
        two.set(two.get() + 1);
    });
    check(told.get() == 0, "the _async forms tell after they return", told.get());

    let context = zebkit::Context::new().expect("a context");
    context.close();
    let refused = context.new_offscreen_view(None, 10, 10, 1.0);
    check(refused.as_ref().is_err_and(|error| !error.message().is_empty()), "new_offscreen_view(): the engine's error",
        refused.as_ref().err().map(|error| error.message().to_string()));

    let offscreen = View::new_offscreen(None, 40, 40, 1.0).expect("an off-screen view");
    let exported = Rc::new(RefCell::new(Vec::new()));
    let sink = exported.clone();
    offscreen.export_native_window(move |handle| sink.borrow_mut().push(handle));

    let view = View::new(None, 320, 240);
    let answers = Rc::new(Cell::new(0));
    let count = answers.clone();
    // The engine's rule, in every binding: invalid options are taken (true)
    // and told once to done, after the call returns; false, done never
    // called, only for a closed view.
    let returned = Rc::new(Cell::new(false));
    let after_return = returned.clone();
    let queued = view.set_user_agent_override(r#"{"userAgent":42}"#, move |result| {
        check(result.is_err() && after_return.get(), "set_user_agent_override(): an invalid one is an error, told after it returns", &result);
        count.set(count.get() + 1);
    });
    returned.set(true);
    check(queued, "set_user_agent_override(): invalid options are taken (true)", queued);
    let (returned, nul_answers) = (Rc::new(Cell::new(false)), answers.clone());
    let after_return = returned.clone();
    let nul = view.set_user_agent_override("{\"userAgent\":\"a\0b\"}", move |result| {
        check(result.as_ref().is_err_and(|e| e.message().contains("NUL")) && after_return.get(), "set_user_agent_override(): a NUL is an error, told after it returns", &result);
        nul_answers.set(nul_answers.get() + 10);
    });
    returned.set(true);
    check(nul, "set_user_agent_override(): options with a NUL are taken (true)", nul);
    let closed = View::new(None, 20, 20);
    closed.close();
    let closing = closed.clone();
    poll(move || closing.is_closed(), 200, move || {
        let refused = closed.set_user_agent_override(r#"{"userAgent":"x"}"#, |_| check(false, "set_user_agent_override(): a closed view calls done", ()));
        check(!refused, "set_user_agent_override(): a closed view is false", refused);
    });
    view.grant("*", Allow::APP, None);
    let kept: Rc<RefCell<Vec<zebkit::Call>>> = Rc::new(RefCell::new(Vec::new()));
    let sink = kept.clone();
    view.bind("later", move |call| sink.borrow_mut().push(call));
    view.bind("dropped", drop);
    let gone = Rc::new(RefCell::new(String::new()));
    let sink = gone.clone();
    view.on_process_gone(move |json| *sink.borrow_mut() = json.to_string());
    view.on_download_result(|_| {});
    view.on_responsiveness(|_| {});
    let port = serve("<title>new api</title>");
    view.load_url(&format!("http://127.0.0.1:{port}/"));
    let (held, view2) = (kept.clone(), view.clone());
    poll(move || view2.title().as_deref() == Some("new api"), 500, move || {
        let page = view.clone();
        view.evaluate("window.answer = zebkit.app.later(20, 1).then(v => 'later ' + v); \
                       zebkit.app.dropped().then(() => 'resolved', e => 'rejected: ' + e.message)", move |dropped| {
            check(dropped.as_deref() == Ok("\"rejected: the application dropped the call\""),
                "a Call dropped unanswered: zebkit_call_free", &dropped);
            let Some(call) = held.borrow_mut().pop() else {
                check(false, "View::bind: the Call kept", ());
                NEW_API_DONE.with(|done| done.set(true));
                return;
            };
            let sum = call.args().iter().filter_map(zebkit::json::Value::as_i64).sum::<i64>();
            // Send: answered from another thread.
            std::thread::spawn(move || call.resolve(&sum.to_string())).join().unwrap();
            let page2 = page.clone();
            page.evaluate("window.answer", move |answer| {
                check(answer.as_deref() == Ok("\"later 21\""), "a Call answered later, from another thread", &answer);
                page2.load_url("chrome://crash");
                let page3 = page2.clone();
                let watched = gone.clone();
                poll(move || !watched.borrow().is_empty(), 1000, move || {
                    check(gone.borrow().contains("\"reason\":\"crashed\"") && gone.borrow().contains("exitCode"),
                        "on_process_gone(): crashed", gone.borrow().clone());
                    check(answers.get() == 11, "set_user_agent_override(): answered exactly once each", answers.get());
                    check(*exported.borrow() == [None], "export_native_window(): an off-screen view has none",
                        exported.borrow().clone());
                    check(told.get() == 2, "the _async forms told once each", told.get());
                    page3.clear_process_gone_handler();
                    page3.clear_download_result_handler();
                    page3.clear_responsiveness_handler();
                    page3.close();
                    offscreen.close();
                    NEW_API_DONE.with(|done| done.set(true));
                });
            });
        });
    });
}

fn poll(condition: impl Fn() -> bool + 'static, tries: u32, then: impl FnOnce() + 'static) {
    if condition() {
        then();
    } else if tries == 0 {
        check(false, "a condition held in time", ());
        FIND_DONE.with(|done| done.set(true));
        NEW_API_DONE.with(|done| done.set(true));
    } else {
        zebkit::after(Duration::from_millis(30), move || poll(condition, tries - 1, then));
    }
}

/// A future driven to its end on this thread, which sleeps until woken: the
/// library's calls are woken from the engine's thread.
fn block_on<F: std::future::Future>(future: F) -> F::Output {
    struct Unpark(std::thread::Thread);
    impl std::task::Wake for Unpark {
        fn wake(self: std::sync::Arc<Self>) {
            self.0.unpark();
        }
    }
    let waker = std::task::Waker::from(std::sync::Arc::new(Unpark(std::thread::current())));
    let mut context = std::task::Context::from_waker(&waker);
    let mut future = std::pin::pin!(future);
    loop {
        if let std::task::Poll::Ready(value) = future.as_mut().poll(&mut context) {
            return value;
        }
        std::thread::park_timeout(Duration::from_secs(1));
    }
}

/// JavaScript libraries (zebkit::js, <zebkit/library.h>): loaded here, on
/// the engine's thread, and called from another thread, whose futures the
/// engine wakes; each code; fetch() refused, and allowed to a local server.
fn libraries() {
    use zebkit::js::{Code, Library};
    use zebkit::json::Value;
    let here = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../tests/engine");
    let fixture = here.join("library_fixture");
    let refused = Library::load(here.join("no-such-library"), &Value::Null);
    check(refused.is_err(), "js::Library::load: a bad path is an error", refused.as_ref().err());
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", listener.local_addr().unwrap().port());
    std::thread::spawn(move || {
        for mut stream in listener.incoming().flatten() {
            let mut line = String::new();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            while reader.read_line(&mut line).is_ok_and(|read| read > 2) {
                line.clear();
            }
            let _ = write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
        }
    });
    let options = |text: &str| Value::parse(text).unwrap();
    let lib = Library::load(&fixture, &Value::Null).unwrap();
    let net = Library::load(&fixture, &options(&format!(r#"{{"network": ["{base}"]}}"#))).unwrap();
    let small = Library::load(&fixture, &options(r#"{"queue": 1}"#)).unwrap();
    let doomed = Library::load(&fixture, &Value::Null).unwrap();
    let hungry = Library::load(&fixture, &options(r#"{"heapLimitMB": 32}"#)).unwrap();
    let forbidden = Library::load(here.join("library_forbidden"), &Value::Null).unwrap();
    let closing = Library::load(&fixture, &Value::Null).unwrap();
    // .then on the engine's thread.
    lib.call("double", Value::Array(vec![21.into()])).then(|reply| {
        let ok = reply.as_ref().is_ok_and(|reply| reply.value.as_i64() == Some(42));
        zebkit::post(move || check(ok, "js::Call::then: the value, on the engine's thread", ()));
    });
    let worker = std::thread::spawn(move || {
        let mut results: Vec<(bool, String, String)> = Vec::new();
        let mut note = |ok: bool, what: &str, detail: String| results.push((ok, what.to_string(), detail));
        let ready = block_on(lib.ready());
        note(ready.as_ref().is_ok_and(|r| r.value["functions"].as_array().is_some_and(|f| f.iter().any(|n| n.as_str() == Some("echo")))),
             "js: ready, the exported functions", format!("{ready:?}"));
        let args = Value::parse(r#"[{"a": [1, null, true], "b": {"$part": 0}}, "日本語 🦓"]"#).unwrap();
        let echoed = block_on(lib.call_with("echo", &args, &[&[1, 2, 255]], None));
        note(echoed.as_ref().is_ok_and(|r| r.value == args && r.parts == vec![vec![1u8, 2, 255]]),
             "js: JSON, non-ASCII text and a part echoed", format!("{echoed:?}"));
        let bytes = block_on(lib.call("bytes", Value::Array(vec![300.into()])));
        note(bytes.as_ref().is_ok_and(|r| r.value["b"]["$part"].as_i64() == Some(0) && r.parts[0].len() == 300 && r.parts[0][299] == 43),
             "js: a Uint8Array returned is a part", format!("{:?}", bytes.as_ref().map(|r| r.value.clone())));
        let thrown = block_on(lib.call("thrower", Value::Null));
        note(thrown.as_ref().is_err_and(|e| e.code == Code::Thrown && e.name == "RangeError" && e.message == "too far" && e.stack.contains("thrower")),
             "js: THROWN, with the library's name, message and stack", format!("{thrown:?}"));
        let codes = [("rejecter", Code::Thrown), ("missing", Code::NoFunction), ("badResult", Code::BadResult)];
        for (function, code) in codes {
            let outcome = block_on(lib.call(function, Value::Null));
            note(outcome.as_ref().is_err_and(|e| e.code == code), &format!("js: {function} is {}", code.name()), format!("{outcome:?}"));
        }
        let first = lib.call("later", Value::parse("[1, 200]").unwrap());
        let queued = lib.call("double", Value::parse("[1]").unwrap());
        queued.cancel();
        let (queued, first) = (block_on(queued), block_on(first));
        note(queued.as_ref().is_err_and(|e| e.code == Code::Cancelled) && first.is_ok(), "js: Call::cancel of a waiting call", format!("{queued:?}"));
        let slow = lib.call_with("later", &Value::parse("[1, 2000]").unwrap(), &[], Some(Duration::from_millis(100)));
        let next = lib.call("double", Value::parse("[2]").unwrap());
        let (slow, next) = (block_on(slow), block_on(next));
        note(slow.as_ref().is_err_and(|e| e.code == Code::Timeout) && next.as_ref().is_ok_and(|r| r.value.as_i64() == Some(4)),
             "js: a waiting call past its deadline is TIMEOUT, the next runs", format!("{slow:?} {next:?}"));
        let fetched = block_on(lib.call("get", Value::Array(vec![format!("{base}/").into()])));
        note(fetched.as_ref().is_err_and(|e| e.code == Code::Thrown && e.name == "TypeError"), "js: fetch() refused without a grant", format!("{fetched:?}"));
        let fetched = block_on(net.call("get", Value::Array(vec![format!("{base}/").into()])));
        note(fetched.as_ref().is_ok_and(|r| r.value["status"].as_i64() == Some(200) && r.value["body"].as_str() == Some("hello")),
             "js: fetch() to a granted origin", format!("{fetched:?}"));
        block_on(small.ready()).ok();
        let running = small.call("later", Value::parse("[0, 300]").unwrap());
        std::thread::sleep(Duration::from_millis(100));
        let (waiting, full) = (small.call("double", Value::parse("[1]").unwrap()), small.call("double", Value::parse("[1]").unwrap()));
        let full = block_on(full);
        note(full.as_ref().is_err_and(|e| e.code == Code::QueueFull) && block_on(running).is_ok() && block_on(waiting).is_ok(),
             "js: QUEUE_FULL", format!("{full:?}"));
        let spinning = doomed.call_with("spin", &Value::Null, &[], Some(Duration::from_millis(200)));
        let after = doomed.call("double", Value::parse("[1]").unwrap());
        let (spinning, after) = (block_on(spinning), block_on(after));
        note(spinning.as_ref().is_err_and(|e| e.code == Code::Timeout) && after.as_ref().is_err_and(|e| e.code == Code::Gone),
             "js: a running script past its deadline is TIMEOUT, then GONE", format!("{spinning:?} {after:?}"));
        let grown = block_on(hungry.call_with("grow", &Value::Null, &[], Some(Duration::MAX)));
        note(grown.as_ref().is_err_and(|e| e.code == Code::OutOfMemory), "js: heap exhaustion is OUT_OF_MEMORY", format!("{grown:?}"));
        let loaded = block_on(forbidden.ready());
        note(loaded.as_ref().is_err_and(|e| e.code == Code::LoadFailed && e.message.contains("outside")),
             "js: an import outside the folder fails the load", format!("{loaded:?}"));
        block_on(closing.ready()).ok();
        let pending = [closing.call("later", Value::parse("[1, 1000]").unwrap()), closing.call("double", Value::parse("[1]").unwrap())];
        // Dropped off the engine's thread: closed there.
        drop(closing);
        let gone = pending.into_iter().map(block_on).all(|outcome| outcome.is_err_and(|e| e.code == Code::Gone));
        note(gone, "js: a library dropped with calls pending: GONE", String::new());
        drop((lib, net, small, doomed, hungry, forbidden));
        results
    });
    fn finish(worker: std::thread::JoinHandle<Vec<(bool, String, String)>>, tries: u32) {
        if !worker.is_finished() && tries > 0 {
            zebkit::after(Duration::from_millis(50), move || finish(worker, tries - 1));
            return;
        }
        match worker.is_finished().then(|| worker.join()) {
            Some(Ok(results)) => {
                for (ok, what, detail) in results {
                    check(ok, &what, detail);
                }
            }
            _ => check(false, "js: the libraries' thread finished", ()),
        }
        LIBRARY_DONE.with(|done| done.set(true));
    }
    finish(worker, 1200);
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
                .join("../../../tests/engine/pages/media/clip.webm")
                .canonicalize()
                .unwrap_or_default();
            request.respond_file(&clip, "Content-Type: video/webm");
        } else if request.url() == "rs://test/played.html" {
            request.respond(200, "Content-Type: text/html", b"<video src='captured.webm' muted></video>");
        } else if request.url() == "rs://test/captured.webm" {
            let captured = CAPTURED.with(|path| path.borrow().clone());
            request.respond_file(&captured, "Content-Type: video/webm");
        } else if request.url() == "rs://test/context.html" {
            request.respond(200, "Content-Type: text/html", b"<title>context</title>");
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
    contexts();
    media();
    handlers();
    automation_api(serve_pages());
    find_and_downloads(serve_pages());
    new_api();
    libraries();
    connections();
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
    // Headless, the view has no window to put in another's.
    let refused = view.attach_native(1, 0, 0, 100, 100);
    check(
        refused.as_ref().is_err_and(|error| error.message().contains("off-screen")),
        "attach_native: refused without a window, naming off-screen views",
        refused.as_ref().err().map(|error| error.message().to_string()),
    );
    check(!view.detach_native(), "detach_native: nothing to detach", ());
    let itself = view.try_attach(&view, 0, 0, 10, 10);
    check(itself.is_err(), "try_attach: refused for the view as its own parent", itself.err().map(|error| error.message().to_string()));
    check(view.set_settings(r#"{"colorScheme":"dark","reducedMotion":true}"#).is_ok()
        && view.settings().is_some_and(|json| json.contains("dark") && json.contains("true")),
        "view settings: native preferences", view.settings());
    let settings_before = view.settings();
    check(view.set_settings(r#"{"colorScheme":"light","javaScript":5}"#).is_err()
        && view.set_settings("{}\0{}").is_err() && view.settings() == settings_before,
        "view settings: invalid updates are atomic", view.settings());
    check(view.set_settings(r#"{"colorScheme":null,"reducedMotion":null}"#).is_ok()
        && view.settings().as_deref() == Some("{}"), "view settings: reset", view.settings());

    let console = View::new(None, 240, 120);
    let closing_console = console.clone();
    console.on_console(move |json| {
        check(json.contains("\"message\":\"Rust console\"") && json.contains("\"level\":\"warning\""),
            "console: native document message", json);
        closing_console.clear_console_handler();
        closing_console.close();
        CONSOLE_DONE.with(|done| done.set(true));
    });
    console.evaluate("console.warn('Rust console'); true", |_| {});

    let popup = View::new(None, 240, 120);
    check(popup.window_open_info().as_deref() == Some("{}"), "window: ordinary view has no creation snapshot", ());
    popup.set_popup_policy(zebkit::PopupPolicy::Allow);
    popup.on_window_open(move |child, url| {
        let metadata = child.window_open_info().unwrap_or_default();
        check(url == "about:blank" && metadata.contains("\"hasX\":true")
            && metadata.contains("\"hasOpener\":true") && child.target_id().is_some(),
            "window: actual child creation snapshot", &metadata);
        child.close();
    });
    let closing_popup = popup.clone();
    popup.on_window_open_event(move |json| {
        if !json.contains("\"phase\":\"created\"") { return; }
        check(json.contains("\"targetId\"") && json.contains("\"frameName\":\"child\""),
            "window: unified creation observer", json);
        closing_popup.clear_window_open_event_handler();
        closing_popup.close();
        WINDOW_DONE.with(|done| done.set(true));
    });
    let popup_port = serve("<script>window.open('about:blank','child','left=0,top=0,width=320,height=240')</script>");
    popup.load_url(&format!("http://127.0.0.1:{popup_port}/"));

    let navigation = View::new(None, 240, 120);
    check(!navigation.set_navigation_timeout(0) && navigation.set_navigation_timeout(5000),
        "navigation: deadline validation", ());
    let responding = navigation.clone();
    navigation.on_navigation(move |request| {
        check(request.info().as_deref() == Some(request.metadata()), "navigation: request metadata", request.metadata());
        check(request.respond(NavigationAction::Redirect("not a URL")).is_err()
            && request.respond(NavigationAction::Redirect("https://invalid.test/\0suffix")).is_err()
            && request.info().is_some(), "navigation: invalid reply preserves request", ());
        let response = request.metadata().contains("\"phase\":\"response\"");
        check(request.respond(if response { NavigationAction::Cancel } else { NavigationAction::Continue }).is_ok()
            && request.info().is_none() && request.respond(NavigationAction::Continue).is_err(),
            "navigation: reply exactly once", ());
        if response { responding.clear_navigation_handler(); }
    });
    let finished_navigation = navigation.clone();
    navigation.on_navigation_result(move |json| {
        if !json.contains("\"phase\":\"response\"") { return; }
        check(json.contains("\"state\":\"cancelled\"") && json.contains("answered"),
            "navigation: asynchronous outcome", json);
        finished_navigation.clear_navigation_result_handler();
        finished_navigation.close();
        NAVIGATION_DONE.with(|done| done.set(true));
    });
    let navigation_port = serve("<title>Rust navigation response</title>");
    navigation.load_url(&format!("http://127.0.0.1:{navigation_port}/"));

    let permission_origin = format!("http://127.0.0.1:{page_port}");
    check(view.set_permission("geolocation", PermissionState::Denied, Some(&permission_origin)).is_ok()
        && view.permission("geolocation", Some(&permission_origin)) == Some(PermissionState::Denied),
        "permission: Rust native rule", ());
    check(view.set_permission("geolocation\0notifications", PermissionState::Granted, Some(&permission_origin)).is_err()
        && view.permission("geolocation", Some(&permission_origin)) == Some(PermissionState::Denied),
        "permission: reject NUL atomically", ());
    check(view.reset_permissions(None).is_ok() && !view.set_permission_timeout(0)
        && view.set_permission_timeout(5000), "permission: reset and deadline", ());
    let prompt = View::new(None, 200, 100);
    let callback_view = prompt.clone();
    prompt.on_permission(move |request| {
        check(request.info().as_deref() == Some(request.metadata()) && request.metadata().contains("requestingOrigin"),
            "permission: request metadata", request.metadata());
        check(request.respond(PermissionState::Denied, false)
            && !request.respond(PermissionState::Granted, false) && request.info().is_none(),
            "permission: reply exactly once", ());
        callback_view.clear_permission_handler();
    });
    let finished = prompt.clone();
    prompt.on_permission_result(move |json| {
        check(json.contains("answered") && json.contains("denied"), "permission: asynchronous outcome", json);
        finished.clear_permission_result_handler();
        finished.close();
        PERMISSION_DONE.with(|done| done.set(true));
    });
    let permission_port = serve("<script>navigator.geolocation.getCurrentPosition(()=>{},()=>{});</script>");
    prompt.load_url(&format!("http://127.0.0.1:{permission_port}/"));

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
    let view = match View::new_offscreen(Some("rs://test/offscreen.html"), 200, 100, 2.0) {
        Ok(view) => view,
        Err(error) => {
            check(false, "View::new_offscreen", error);
            OFFSCREEN_DONE.with(|done| done.set(true));
            return;
        }
    };
    check(view.is_offscreen(), "View::new_offscreen", ());
    // Pixels this process can read, on every system: a GPU buffer would be
    // accepted unseen.
    view.set_frame_buffers(Buffers::SharedMemory);
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
                    let expected: &[u8] = match frame.format() {
                        PixelFormat::Rgba8 => &[0, 128, 255, 255],
                        _ => &[255, 128, 0, 255],
                    };
                    pixels.get(at..at + 4) == Some(expected)
                }
                // Asked for shared memory: anything else is a failure.
                _ => false,
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
                editing(view2.clone());
            }
        });
    }
}

/// Sets a flag when dropped: shows when a closure the engine held is let go.
struct Dropped(Rc<Cell<bool>>);
impl Drop for Dropped {
    fn drop(&mut self) {
        self.0.set(true);
    }
}

/// Closures the engine holds are let go when they remove themselves, and
/// when their view closes.
fn handlers() {
    let made = View::try_new(None, 200, 100);
    check(made.is_ok(), "View::try_new", made.as_ref().err());
    let Ok(view) = made else {
        HANDLERS_DONE.with(|done| done.set(true));
        return;
    };
    check(zebkit::automation::page_id(&view).is_some_and(|id| !id.is_empty()), "automation::page_id", zebkit::automation::page_id(&view));
    // Kept by closures that capture the view itself: only closing lets them go.
    let state_dropped = Rc::new(Cell::new(false));
    let event_dropped = Rc::new(Cell::new(false));
    {
        let (keep, held) = (Dropped(state_dropped.clone()), view.clone());
        view.on_window_state(move |_| {
            let _ = (&keep, &held);
        });
        let (keep, held) = (Dropped(event_dropped.clone()), view.clone());
        zebkit::automation::on_event(&view, move |_, _| {
            let _ = (&keep, &held);
        });
    }
    let offscreen = View::new_offscreen(Some("rs://test/offscreen.html"), 100, 50, 1.0);
    let Ok(offscreen) = offscreen else {
        check(false, "handlers: an off-screen view", "none");
        HANDLERS_DONE.with(|done| done.set(true));
        return;
    };
    let frame_dropped = Rc::new(Cell::new(false));
    let frames = Rc::new(Cell::new(0));
    {
        let (keep, stopping, counted) = (Dropped(frame_dropped.clone()), offscreen.clone(), frames.clone());
        offscreen.on_frame(move |_| {
            let _ = &keep;
            counted.set(counted.get() + 1);
            // Removing itself while it runs: it is not put back.
            stopping.clear_frame_handler();
        });
    }
    when_true(offscreen.clone(), "document.readyState == 'complete'", 200, move |offscreen| {
        offscreen.request_frame();
        zebkit::after(Duration::from_millis(500), move || {
            check(frames.get() >= 1 && frame_dropped.get(), "a frame function that stops the frames is let go", (frames.get(), frame_dropped.get()));
            offscreen.close();
            check(!state_dropped.get() && !event_dropped.get(), "a view's closures are kept while it is open", ());
            let closing = view.clone();
            view.on_close(move || {
                let _ = &closing;
                // The automation's closure goes in a task after the close.
                zebkit::after(Duration::from_millis(50), move || {
                    check(state_dropped.get(), "closing a view lets its window-state closure go", ());
                    check(event_dropped.get(), "closing a view lets its automation closure go", ());
                    HANDLERS_DONE.with(|done| done.set(true));
                });
            });
            view.close();
        });
    });
}

// Connection reuse (docs/CONNECTION_REUSE.md): preconnect opens
// connections and sends nothing, a context's fetch sends its own cookies, the
// HTTP cache is cleared. A local server counts the connections it accepted.
fn connections() {
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Arc;
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let base = format!("http://127.0.0.1:{}", listener.local_addr().unwrap().port());
    let (accepted, requests) = (Arc::new(AtomicUsize::new(0)), Arc::new(AtomicUsize::new(0)));
    {
        let (accepted, requests) = (accepted.clone(), requests.clone());
        std::thread::spawn(move || {
            for stream in listener.incoming().flatten() {
                accepted.fetch_add(1, Ordering::SeqCst);
                let requests = requests.clone();
                std::thread::spawn(move || {
                    let mut reader = BufReader::new(stream);
                    let mut first = String::new();
                    if reader.read_line(&mut first).unwrap_or(0) == 0 {
                        return;
                    }
                    requests.fetch_add(1, Ordering::SeqCst);
                    let (mut line, mut cookie) = (String::new(), String::new());
                    while reader.read_line(&mut line).is_ok_and(|read| read > 2) {
                        if let Some(value) = line.strip_prefix("Cookie: ") {
                            cookie = value.trim().to_string();
                        }
                        line.clear();
                    }
                    let set = if first.starts_with("GET /set ") { "Set-Cookie: rust=context; Path=/; Max-Age=3600\r\n" } else { "" };
                    let _ = write!(
                        reader.into_inner(),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n{set}Content-Length: {}\r\nConnection: close\r\n\r\n{cookie}",
                        cookie.len()
                    );
                });
            }
        });
    }
    let finish = || CONNECTIONS_DONE.with(|done| done.set(true));
    check(services::preconnect("ftp://127.0.0.1/", 1).is_err(), "services::preconnect(): not http(s) is an error", ());
    check(services::preconnect(&base, 0).is_err() && services::preconnect(&base, 7).is_err(),
          "services::preconnect(): a count out of 1 to 6 is an error", ());
    let preconnected = services::preconnect(&base, 2);
    check(preconnected.is_ok(), "services::preconnect()", &preconnected);
    let context = match zebkit::Context::new() {
        Ok(context) => Rc::new(context),
        Err(error) => {
            check(false, "Context::new", error);
            return finish();
        }
    };
    let preconnected = context.preconnect(&base, 1);
    check(preconnected.is_ok(), "Context::preconnect()", &preconnected);
    fn until(condition: impl Fn() -> bool + 'static, then: impl FnOnce(bool) + 'static, left: u32) {
        if condition() || left == 0 {
            then(condition());
        } else {
            zebkit::after(Duration::from_millis(20), move || until(condition, then, left - 1));
        }
    }
    let (counted, asked) = (accepted.clone(), requests.clone());
    until(move || counted.load(Ordering::SeqCst) >= 3, move |opened| {
        check(opened && asked.load(Ordering::SeqCst) == 0, "preconnect: three connections, no request",
              (accepted.load(Ordering::SeqCst), asked.load(Ordering::SeqCst)));
        let (inside, base2) = (context.clone(), base.clone());
        let set = context.fetch("GET", &format!("{base}/set"), &[], b"", move |response| {
            check(response.status == 200, "Context::fetch() sets a cookie", response.status);
            let (outside, base3) = (inside.clone(), base2.clone());
            let asked = inside.fetch("GET", &format!("{base2}/cookie"), &[], b"", move |response| {
                check(response.body == b"rust=context", "Context::fetch() sends the context's cookie",
                      String::from_utf8_lossy(response.body));
                services::fetch("GET", &format!("{base3}/cookie"), &[], b"", move |response| {
                    check(response.body.is_empty(), "services::fetch() does not send the context's",
                          String::from_utf8_lossy(response.body));
                    let context = outside.clone();
                    let cleared = services::clear_http_cache(move || {
                        check(true, "services::clear_http_cache() is done", ());
                        let closing = context.clone();
                        let cleared = context.clear_http_cache(move || {
                            check(true, "Context::clear_http_cache() is done", ());
                            closing.close();
                            check(closing.preconnect("http://127.0.0.1/", 1).is_err()
                                  && closing.clear_http_cache(|| {}).is_err()
                                  && closing.fetch("GET", "http://127.0.0.1/", &[], b"", |_| {}).is_err(),
                                  "a closed context refuses preconnect, clear_http_cache and fetch", ());
                            CONNECTIONS_DONE.with(|done| done.set(true));
                        });
                        if let Err(error) = cleared {
                            check(false, "Context::clear_http_cache()", error);
                            CONNECTIONS_DONE.with(|done| done.set(true));
                        }
                    });
                    if let Err(error) = cleared {
                        check(false, "services::clear_http_cache()", error);
                        CONNECTIONS_DONE.with(|done| done.set(true));
                    }
                });
            });
            if let Err(error) = asked {
                check(false, "Context::fetch()", error);
                CONNECTIONS_DONE.with(|done| done.set(true));
            }
        });
        if let Err(error) = set {
            check(false, "Context::fetch()", error);
            CONNECTIONS_DONE.with(|done| done.set(true));
        }
    }, 250);
}

// A context's storage is its own: what a page stores there, a page of the
// same origin in the engine's profile does not see.
fn contexts() {
    let context = match zebkit::Context::new() {
        Ok(context) => context,
        Err(error) => {
            check(false, "Context::new", error);
            CONTEXTS_DONE.with(|done| done.set(true));
            return;
        }
    };
    check(context.new_offscreen_view(None, 100, 100, 1.0).is_ok(), "an off-screen view in a context", ());
    let Some(inside) = context.new_view(Some("rs://test/context.html"), 320, 240) else {
        check(false, "Context::new_view", "none");
        CONTEXTS_DONE.with(|done| done.set(true));
        return;
    };
    let inside2 = inside.clone();
    inside.on_title(move |title| {
        if title != "context" {
            return;
        }
        let inside3 = inside2.clone();
        inside2.evaluate("localStorage.setItem('kept', 'in the context'); localStorage.getItem('kept')", move |stored| {
            check(stored.as_deref() == Ok("\"in the context\""), "a context's storage", &stored);
            let outside = View::new(Some("rs://test/context.html"), 320, 240);
            let outside2 = outside.clone();
            let inside4 = inside3.clone();
            outside.on_title(move |title| {
                if title != "context" {
                    return;
                }
                let (outside3, inside5) = (outside2.clone(), inside4.clone());
                outside2.evaluate("localStorage.getItem('kept')", move |seen| {
                    check(seen.as_deref() == Ok("null"), "the engine's profile does not see it", &seen);
                    outside3.close();
                    inside5.close();
                    CONTEXTS_DONE.with(|done| done.set(true));
                });
            });
        });
    });
    // A context lives until it is closed; this one, until the engine stops.
    let _ = context;
}

fn editing(view: View) {
    use zebkit::offscreen::{EditCommand, KeyEvent, KeyEventType};
    view.set_focus(true);
    let prepared = view.clone();
    view.evaluate("(()=>{const t=document.createElement('textarea');t.value='old';document.body.append(t);t.focus();window.editKeys=0;t.addEventListener('keydown',()=>editKeys++);return true})()", move |result| {
        check(result.as_deref() == Ok("true"), "native editing: prepare input", &result);
        let key = KeyEvent { kind: KeyEventType::Down, modifiers: Modifiers::NONE,
            key_code: 0, native_key_code: 0, code: None, key: None, text: None, unmodified_text: None };
        check(prepared.send_key_with_commands(&key, &[EditCommand { name: "SelectAll", value: None },
            EditCommand { name: "InsertText", value: Some("Rust Ω") }]), "native editing: send command batch", ());
        when_true(prepared, "document.querySelector('textarea').value === 'Rust Ω' && window.editKeys === 1", 40, |view| {
            check(true, "native editing: Unicode and one keydown", ());
            page(view);
        });
    });
}

/// A host can disable UI inside an authentication callback, receive its
/// eventual close event, then close the view from that callback safely.
fn authentication_host() {
    let port = serve("<style>body{margin:0}button{width:200px;height:60px}</style><button onclick=\"navigator.credentials.get({publicKey:{challenge:new Uint8Array(32),rpId:'localhost',timeout:10000}}).catch(()=>{})\">Owned test</button>");
    // Off-screen views share the process display scale; keep the scale
    // used by the rendering fixture above.
    let view = View::new_offscreen(None, 240, 120, 2.0).expect("an off-screen host");
    view.set_authentication_ui(true);
    view.set_focus(true);
    let responding = view.clone();
    let disabled = Rc::new(Cell::new(false));
    let installed = view.on_authentication(move |event| {
        if event.contains(r#""state":"closed""#) {
            check(disabled.get() && event.contains(r#""outcome":"disabled""#),
                "authentication: deferred closure after disabling UI inside its handler", ());
            AUTHENTICATION_DONE.with(|done| done.set(true));
            responding.close();
        } else if !disabled.replace(true) {
            responding.set_authentication_ui(false);
        }
    });
    check(installed, "authentication: host handler registered", ());
    let ready = view.clone();
    view.on_load(move |event, url| {
        if event == LoadEvent::Finished && url.starts_with("http://localhost:") {
            ready.send_mouse(MouseEvent::Down, 20.0, 20.0, MouseButton::Left, 1, Modifiers::NONE);
            ready.send_mouse(MouseEvent::Up, 20.0, 20.0, MouseButton::Left, 1, Modifiers::NONE);
        }
    });
    view.load_url(&format!("http://localhost:{port}/"));
}

/// <zebkit/page.h>: a confirm() answered by the application, the page
/// printed to PDF; then the view closes.
fn page(view: View) {
    // zebkit::automation's pixels: those of the PNG Page.screenshot writes.
    let shots = zebkit::automation::Page::new(view.clone());
    let png_path = std::env::temp_dir().join(format!("zebkit-rust-shot-{}.png", std::process::id()));
    let path_text = png_path.to_string_lossy().into_owned();
    let pixels_page = shots.clone();
    shots.screenshot(zebkit::json::Value::object([("path", path_text.as_str().into())]), move |written| {
        let png = std::fs::read(&png_path).unwrap_or_default();
        let _ = std::fs::remove_file(&png_path);
        pixels_page.screenshot_pixels(zebkit::json::Value::Null, move |pixels| {
            let decoded = services::decode_image(&png);
            let same = matches!((&pixels, &decoded), (Ok(p), Ok(i))
                if p.width == i.width && p.height == i.height && p.stride == p.width as usize * 4 && p.data == &i.rgba[..]);
            check(
                written.is_ok() && same,
                "automation: the screenshot's pixels are the PNG's",
                (pixels.as_ref().map(|p| (p.width, p.height, p.stride)), decoded.as_ref().map(|i| (i.width, i.height))),
            );
            page_dialogs(view);
        });
    });
}

fn page_dialogs(view: View) {
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
            check(pdf.as_ref().is_ok_and(|bytes| bytes.starts_with(b"%PDF-")), "page: printed to PDF", pdf.as_ref().map(|bytes| bytes.len()));
            closing.close();
            OFFSCREEN_DONE.with(|done| done.set(true));
            // Keep focus-sensitive fixtures sequential: this test explicitly
            // focuses its own off-screen view to begin authentication.
            authentication_host();
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
            let capture = page.media_capture(&id, &path, "{}");
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
    let updated = zebkit::app::update_menu_item("open", r#"{"enabled": false}"#);
    check(updated.is_ok() && zebkit::app::menu().is_some_and(|menu| menu.contains("open")), "app: a menu item changed in place", &updated);
    check(zebkit::app::set_badge(Some("3")) && zebkit::app::set_badge(None), "app: a badge, then none", ());
    check(zebkit::app::set_progress(Some(0.5)) && !zebkit::app::set_progress(Some(2.0)), "app: a progress; past 1 refused", ());
    let dock = zebkit::app::set_dock_menu(r#"[{"title": "New", "id": "new"}]"#, |_| {});
    check(dock.is_ok() == cfg!(target_os = "macos"), "app: the Dock's menu is macOS's", &dock);
    let tray = zebkit::Tray::new(b"not a png", "nothing");
    check(tray.is_err(), "a tray icon that is not a PNG is refused", tray.as_ref().err());
    let displays = zebkit::system::displays();
    check(displays.as_deref().is_some_and(|list| list.starts_with('[')), "system: the displays", &displays);
    let token = zebkit::system::prevent_sleep("the Rust test", false);
    check(token.is_some_and(zebkit::system::allow_sleep) && token.is_some_and(|token| !zebkit::system::allow_sleep(token)),
          "system: sleep prevented, then allowed once", token);
    let refused = zebkit::system::open_external("javascript:alert(1)");
    check(refused.is_err(), "system: a javascript: URL is not opened", &refused);
    zebkit::system::on_power_event(Some(|_: &str| {}));
    zebkit::system::on_power_event(None::<fn(&str)>);
    zebkit::app::on_open_files(|_| {});
    zebkit::app::on_reopen(|| {});

    let port = cdp::listen(None, 0);
    check(port.is_ok(), "CDP listens", &port);
    let Ok(port) = port else {
        zebkit::quit();
        return;
    };
    check(cdp::port() == Some(port), "CDP port", cdp::port());
    cdp::listen_async(None, port, move |result| {
        check(result.as_ref().is_ok_and(|(same, url)| *same == port && url.starts_with("ws://")),
            "cdp::listen_async(): the endpoint that runs", &result);
    });
    cdp::listen_async(None, port.wrapping_add(1), |result| {
        check(result.is_err(), "cdp::listen_async(): another port while it runs is an error", &result);
    });
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
        if !client.is_finished() || !OFFSCREEN_DONE.with(Cell::get) || !CONTEXTS_DONE.with(Cell::get) || !MEDIA_DONE.with(Cell::get)
            || !AUTHENTICATION_DONE.with(Cell::get) || !PERMISSION_DONE.with(Cell::get) || !NAVIGATION_DONE.with(Cell::get) || !CONSOLE_DONE.with(Cell::get) || !WINDOW_DONE.with(Cell::get)
            || !HANDLERS_DONE.with(Cell::get) || !AUTOMATION_DONE.with(Cell::get) || !FIND_DONE.with(Cell::get) || !NEW_API_DONE.with(Cell::get)
            || !LIBRARY_DONE.with(Cell::get)
            || !CONNECTIONS_DONE.with(Cell::get) {
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
    // A suite gives the profile's place (`cargo test --test engine --
    // --user-data-dir=...`) and removes it; else it is made here and removed.
    let given = std::env::args().find_map(|arg| arg.strip_prefix("--user-data-dir=").map(std::path::PathBuf::from));
    let owned = given.is_none();
    let profile = given.unwrap_or_else(|| std::env::temp_dir().join(&identity));
    let remove_profile = move |profile: &Path| {
        if owned {
            let _ = std::fs::remove_dir_all(profile);
        }
    };
    let args = [
        identity.clone(),
        "--headless".to_string(),
        "--no-sandbox".to_string(),
        "--autoplay-policy=no-user-gesture-required".to_string(),
        format!("--user-data-dir={}", profile.display()),
    ];
    // Ends on its own; a test that hangs is a failure too.
    let watchdog_identity = identity.clone();
    let watchdog_profile = profile.clone();
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(120));
        eprintln!("FAIL  the test did not finish in 120 s");
        remove_profile(&watchdog_profile);
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
    remove_profile(&profile);
    cleanup_test_key(&identity);
    check(code == 0, "the engine's exit code", code);

    let (passed, failed) = (PASSED.with(Cell::get), FAILED.with(Cell::get));
    println!("{passed} passed, {failed} failed");
    std::process::exit(if failed == 0 { 0 } else { 1 });
}
