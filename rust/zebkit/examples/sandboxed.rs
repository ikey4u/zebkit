//! A page in the sandbox: on Windows, built with `+crt-static`, this executable is
//! the engine's sandboxed children too. Prints what the page's own script says.
use zebkit::{LoadEvent, View};

fn main() {
    #[cfg(all(windows, target_feature = "crt-static"))]
    if let Some(code) = zebkit::win_startup() {
        std::process::exit(code);
    }
    let code = zebkit::run(|| {
        let view = View::new(Some("data:text/html,<body>sandboxed"), 400, 300);
        let page = view.clone();
        view.on_load(move |event, _| {
            if matches!(event, LoadEvent::Finished) {
                let closing = page.clone();
                page.evaluate("document.body.textContent", move |answer| {
                    println!("the page says {answer:?}");
                    // A moment to look at the children from outside.
                    let hold = std::env::var("ZEBKIT_EXAMPLE_HOLD_MS").ok().and_then(|ms| ms.parse().ok()).unwrap_or(0);
                    std::thread::sleep(std::time::Duration::from_millis(hold));
                    closing.close();
                    zebkit::quit();
                });
            }
        });
    });
    std::process::exit(code);
}
