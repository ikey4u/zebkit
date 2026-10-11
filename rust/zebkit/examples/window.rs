//! A window with a web page in it.
//!
//!     cargo run --example window -- https://example.com/
//!
//! With `ZEBKIT_CDP_PORT=9222` in the environment, or the two lines marked
//! below, the page can also be driven by Puppeteer and Playwright.

use zebkit::{MainLoop, WebView};

fn main() {
    let url = std::env::args().nth(1).unwrap_or_else(|| "https://example.com/".into());
    let main_loop = MainLoop::new();
    let view = WebView::new();

    // On a machine without a display the window is never on a screen; the
    // page loads all the same.
    if let Some(window) = view.window() {
        window.resize(1024, 768);
        window.set_title("zebkit");
        view.on_title_changed(move |title| window.set_title(if title.is_empty() { "zebkit" } else { title }));
        let quit = main_loop.clone();
        view.on_close(move || quit.quit());
    }
    view.on_load_changed(|event| eprintln!("load: {event:?}"));

    // For CDP clients:
    //     let port = zebkit::cdp::listen(None, 9222).expect("cannot start the CDP endpoint");
    //     eprintln!("CDP on http://127.0.0.1:{port}");

    view.load_uri(&url);
    main_loop.run();
}
