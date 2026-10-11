//! Drives a real engine: a page without a window, the CDP endpoint, and the
//! FFmpeg inside the engine. Run with `cargo test`; it needs a built zebkit
//! (see the README).

use std::cell::RefCell;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::rc::Rc;
use std::time::Duration;
use zebkit::vendor::ffmpeg;
use zebkit::{cdp, LoadEvent, MainLoop, WebView};

fn main() {
    // The library inside the engine.
    assert!(
        ffmpeg::version().starts_with(|c: char| c.is_ascii_digit()),
        "FFmpeg version: {:?}",
        ffmpeg::version()
    );
    let decoders: Vec<String> = ffmpeg::decoders().into_iter().map(|codec| codec.name).collect();
    for name in ["h264", "vp9", "aac", "opus"] {
        assert!(
            decoders.iter().any(|decoder| decoder == name),
            "no {name} decoder among {decoders:?}"
        );
        assert!(ffmpeg::has_decoder(name));
    }
    assert!(!ffmpeg::has_decoder("no-such-codec"));
    let encoders: Vec<String> = ffmpeg::encoders().into_iter().map(|codec| codec.name).collect();
    assert!(
        encoders.iter().any(|encoder| encoder == "aac") && !encoders.iter().any(|encoder| encoder.contains("264")),
        "encoders: {encoders:?}"
    );
    println!("ok    ffmpeg {} with {} decoders", ffmpeg::version(), decoders.len());

    // A page, with no window.
    let main_loop = MainLoop::new();
    let view = WebView::headless();
    view.window().expect("a view has a window, shown or not").resize(800, 600);
    let titles = Rc::new(RefCell::new(Vec::new()));
    let events = Rc::new(RefCell::new(Vec::new()));
    {
        let (titles, quit) = (titles.clone(), main_loop.clone());
        view.on_title_changed(move |title| {
            titles.borrow_mut().push(title.to_string());
            if title == "second" {
                quit.quit();
            }
        });
        let events = events.clone();
        view.on_load_changed(move |event| events.borrow_mut().push(event));
    }
    view.load_html(
        "<title>first</title><script>setTimeout(() => document.title = 'second', 50)</script>",
        None,
    );
    let timed_out = Rc::new(RefCell::new(false));
    {
        let (timed_out, quit) = (timed_out.clone(), main_loop.clone());
        zebkit::timeout(Duration::from_secs(30), move || {
            *timed_out.borrow_mut() = true;
            quit.quit();
        });
    }
    main_loop.run();
    assert!(!*timed_out.borrow(), "the page did not set its title; got {:?}", titles.borrow());
    assert_eq!(*titles.borrow(), ["first", "second"]);
    assert_eq!(view.title().as_deref(), Some("second"));
    assert!(
        events.borrow().contains(&LoadEvent::Committed),
        "load events: {:?}",
        events.borrow()
    );
    assert!(!view.can_go_back());
    println!("ok    a page loads and reports its title");

    // The CDP endpoint: it lists the page while the main loop runs.
    let port = cdp::listen(None, 0)
        .or_else(|_| cdp::listen(None, 9387))
        .expect("cannot start the CDP endpoint");
    assert_eq!(cdp::port(), Some(port));
    assert!(cdp::url().is_some_and(|url| url.starts_with(&format!("ws://127.0.0.1:{port}/devtools/browser/"))));
    let answer = Rc::new(RefCell::new(String::new()));
    {
        let (answer, quit) = (answer.clone(), main_loop.clone());
        let client = std::thread::spawn(move || {
            let mut stream = TcpStream::connect(("127.0.0.1", port)).expect("cannot connect to the endpoint");
            stream.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
            write!(
                stream,
                "GET /json/list HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n"
            )
            .unwrap();
            let mut text = String::new();
            let _ = stream.read_to_string(&mut text);
            text
        });
        // The thread only talks to the socket; the engine stays on this one.
        fn poll(client: std::thread::JoinHandle<String>, answer: Rc<RefCell<String>>, quit: MainLoop) {
            if client.is_finished() {
                *answer.borrow_mut() = client.join().unwrap();
                quit.quit();
            } else {
                zebkit::timeout(Duration::from_millis(20), move || poll(client, answer, quit));
            }
        }
        poll(client, answer, quit);
    }
    main_loop.run();
    assert!(
        answer.borrow().contains("\"title\": \"second\"") || answer.borrow().contains("\"title\":\"second\""),
        "the endpoint's list: {}",
        answer.borrow()
    );
    cdp::stop();
    assert_eq!(cdp::port(), None);
    println!("ok    the CDP endpoint lists the page");

    drop(view);
    println!("3 passed, 0 failed");
}
