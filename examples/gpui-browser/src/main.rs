//! A browser made of GPUI-KIT components around zebkit's pages.
//!
//!     cargo run --release -- [URL...] [--smoke-test out.png] [Chromium switches]
//!
//! The integration with zebkit is `embed.rs`; `page.rs` turns a page's
//! callbacks into events and `browser.rs` is the GPUI-KIT application.

mod browser;
mod embed;
mod page;

use browser::Browser;
use gpui_kit::*;
use std::{path::PathBuf, time::Duration};

const START: &[u8] = include_bytes!("start.html");

// GPUI-KIT's icons, and a few more of its Lucide set.
gpui_kit::assets::icon_assets!(MoreIcons, [Lock, Camera, Download, ShieldAlert, ShieldQuestionMark]);
struct Icons;
impl AssetSource for Icons {
    fn load(&self, path: &str) -> Result<Option<std::borrow::Cow<'static, [u8]>>> {
        match MoreIcons.load(path)? {
            Some(svg) => Ok(Some(svg)),
            None => gpui_kit::assets::Assets.load(path),
        }
    }
    fn list(&self, path: &str) -> Result<Vec<SharedString>> {
        Ok([gpui_kit::assets::Assets.list(path)?, MoreIcons.list(path)?].concat())
    }
}

fn main() {
    // On Windows this executable is also the engine's sandboxed page processes.
    #[cfg(all(windows, target_feature = "crt-static"))]
    if let Some(code) = zebkit::win_startup() {
        std::process::exit(code);
    }
    // Ours: URLs to open and --smoke-test; the rest goes to the engine.
    let mut urls = Vec::new();
    let mut engine_args = vec![std::env::args().next().unwrap_or_default()];
    let mut smoke = None;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        if arg == "--smoke-test" {
            smoke = Some(PathBuf::from(args.next().expect("--smoke-test takes the PNG to write")));
        } else if arg.starts_with('-') {
            engine_args.push(arg);
        } else {
            urls.push(arg);
        }
    }
    if smoke.is_some() && urls.is_empty() {
        urls.push("https://example.com/".into());
    }
    urls.insert(0, browser::START_PAGE.into());

    // The start page comes from a URL scheme of the application's own.
    zebkit::scheme::register("browser", zebkit::scheme::STANDARD | zebkit::scheme::SECURE | zebkit::scheme::FETCH);

    // A native child window shows over GPUI's on Windows only without DirectComposition.
    #[cfg(target_os = "windows")]
    unsafe {
        std::env::set_var("GPUI_DISABLE_DIRECT_COMPOSITION", "true")
    };
    // zebkit attaches pages to X11 windows, not Wayland surfaces: GPUI on X11 (or XWayland).
    #[cfg(target_os = "linux")]
    let app = gpui_kit::platform::linux(WindowingModes::X11);
    #[cfg(not(target_os = "linux"))]
    let app = gpui_kit::application();

    app.with_assets(Icons).run(move |cx| {
        gpui_kit::init(cx);
        browser::bind_keys(cx);
        if let Err(error) = embed::start_engine(engine_args, cx) {
            eprintln!("gpui-browser: {error}");
            std::process::exit(1);
        }
        zebkit::scheme::handle("browser", |request| request.respond(200, "Content-Type: text/html; charset=utf-8", START));
        cx.on_app_quit(|_| async {
            zebkit::shutdown();
        })
        .detach();

        let smoke = smoke.map(|out| Smoke::new(out, urls.last().cloned().unwrap_or_default(), cx));
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(None, size(px(1240.), px(820.)), cx))),
            app_id: Some("zebkit-gpui-browser".into()),
            ..component::TitleBar::window_options()
        };
        gpui_kit::open_window(options, cx, |window, cx| {
            window.on_window_should_close(cx, |_, cx| {
                cx.quit();
                true
            });
            cx.new(|cx| Browser::new(urls, smoke, window, cx))
        })
        .expect("open the window");
        cx.activate(true);
    });
}

/// `--smoke-test out.png`: the page in its tab has loaded and drawn; its
/// picture is written and the program exits, 0 when it is not blank.
pub struct Smoke {
    out: PathBuf,
    url: String,
    started: bool,
}

impl Smoke {
    fn new(out: PathBuf, url: String, cx: &mut App) -> Self {
        cx.spawn(async move |cx| {
            cx.background_executor().timer(Duration::from_secs(90)).await;
            eprintln!("smoke test: the page did not load in 90 s");
            std::process::exit(1);
        })
        .detach();
        Smoke { out, url, started: false }
    }

    fn loaded(&mut self, view: &zebkit::View, url: &str, cx: &mut App) {
        let wanted = self.url.trim_end_matches('/');
        if self.started || !url.trim_end_matches('/').starts_with(wanted) {
            return;
        }
        self.started = true;
        let (view, out) = (view.clone(), self.out.clone());
        let (done, result) = futures::channel::oneshot::channel();
        cx.spawn(async move |cx| {
            cx.background_executor().timer(Duration::from_millis(1500)).await;
            let title = view.title().unwrap_or_default();
            view.capture(move |png| drop(done.send((png.map(<[u8]>::to_vec), title))));
            let Ok((png, title)) = result.await else { return };
            let code =
                match png.map_err(|e| e.to_string()).and_then(|png| check(&png, &out).map(|(w, h, colors)| (png.len(), w, h, colors))) {
                    Ok((bytes, w, h, colors)) => {
                        println!("smoke test: ok, \"{title}\", {w}x{h}, {colors} colors, {bytes} bytes in {}", out.display());
                        0
                    }
                    Err(error) => {
                        eprintln!("smoke test: failed: {error}");
                        1
                    }
                };
            zebkit::shutdown();
            std::process::exit(code);
        })
        .detach();
    }
}

/// The page's picture is the content area's size and not one colour.
fn check(png: &[u8], out: &std::path::Path) -> Result<(u32, u32, usize), String> {
    std::fs::write(out, png).map_err(|e| e.to_string())?;
    let image = zebkit::services::decode_image(png).map_err(|e| e.to_string())?;
    let colors: std::collections::HashSet<&[u8]> = image.rgba.chunks(4).collect();
    if image.width < 200 || image.height < 200 || colors.len() < 16 {
        return Err(format!("{}x{} with {} colors", image.width, image.height, colors.len()));
    }
    Ok((image.width, image.height, colors.len()))
}
