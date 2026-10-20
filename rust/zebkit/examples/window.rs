//! A window with a web page in it.
//!
//!     cargo run --example window -- https://example.com/
//!
//! Chromium's switches work as they do for Chrome: with
//! `--remote-debugging-port=9222` the page can also be driven by Puppeteer
//! and Playwright, and `--headless` keeps it off the screen.

use zebkit::{LoadEvent, View};

fn main() {
    let url = std::env::args()
        .skip(1)
        .find(|arg| !arg.starts_with("--"))
        .unwrap_or_else(|| "https://example.com/".into());
    std::process::exit(zebkit::run(move || {
        let view = View::new(Some(&url), 1024, 768);
        view.on_load(|event, url| {
            if event != LoadEvent::Started {
                eprintln!("{event:?}: {url}")
            }
        });
        view.on_title(|title| eprintln!("title: {title}"));
        // The application ends when its window is closed.
        view.on_close(zebkit::quit);
    }));
}
