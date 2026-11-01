//! A winit-owned window and loop, with an off-screen web view and native PDF.
use softbuffer::{Context, Surface};
use std::{
    num::NonZeroU32,
    path::PathBuf,
    rc::Rc,
    time::{Duration, Instant},
};
use winit::{
    application::ApplicationHandler,
    dpi::{LogicalSize, PhysicalPosition},
    event::{ElementState, Ime, MouseButton, MouseScrollDelta, WindowEvent},
    event_loop::{ActiveEventLoop, ControlFlow, EventLoop, EventLoopProxy, OwnedDisplayHandle},
    keyboard::{Key, ModifiersState, NamedKey, PhysicalKey},
    window::{CursorIcon, Window, WindowId},
};
use zebkit::{
    offscreen::{self, Buffers, FrameBuffer, Layer, Modifiers},
    pdf::{Document, RenderOptions},
    View,
};

struct Bitmap {
    width: u32,
    height: u32,
    pixels: Vec<u32>,
    bounds: offscreen::Rect,
}
enum Event {
    Wake(Instant),
    Frame(Layer, Bitmap),
    Popup(bool),
    Cursor(String),
    Caret(offscreen::Rect),
}
struct App {
    proxy: EventLoopProxy<Event>,
    context: Context<OwnedDisplayHandle>,
    surface: Option<Surface<OwnedDisplayHandle, Rc<Window>>>,
    window: Option<Rc<Window>>,
    view: Option<View>,
    pdf: Option<Document>,
    pane: PdfPane,
    web: Option<Bitmap>,
    popup: Option<Bitmap>,
    popup_shown: bool,
    cursor: PhysicalPosition<f64>,
    modifiers: ModifiersState,
    buttons: Modifiers,
    clicks: u32,
    last_press: Option<(offscreen::MouseButton, std::time::Instant)>,
    deadline: Option<Instant>,
    pdf_tick: Option<Instant>,
    scroll_test: bool,
    scroll_due: Option<Instant>,
    scroll_up: u32,
    smoke_due: Option<Instant>,
    smoke: Option<PathBuf>,
    arguments: Vec<std::ffi::OsString>,
    started: bool,
    frames: usize,
    pumps: usize,
    last_pixels: Vec<u32>,
}
/// The PDF as one continuous scrolling document, as model.h's PdfPane for the C++
/// hosts: pages placed from their sizes, only those in the pane rendered.
#[derive(Default)]
struct PdfPane {
    pages: Vec<PdfPage>,
    rendered: Vec<RenderedPage>,
    canvas: Vec<u32>,
    width: u32,
    height: u32,
    total: f64,
    position: f64,
    target: f64,
    clock: u64,
    renders: usize,
    dirty: bool,
}
#[derive(Default, Clone)]
struct PdfPage {
    width: f64,
    height: f64,
    top: f64,
    pixels_high: f64,
}
struct RenderedPage {
    page: usize,
    width: usize,
    height: usize,
    pixels: Vec<u32>,
    used: u64,
}
impl PdfPane {
    const GAP: f64 = 8.;
    const MARGIN: u32 = 8;
    const CACHE_PIXELS: usize = 128 << 18;
    fn open(&mut self, pdf: &mut Document) {
        let count = pdf.page_count();
        self.pages = (0..count)
            .map(|i| {
                let json = pdf.page(i).unwrap_or_default();
                let number = |key: &str, fallback: f64| {
                    json.find(key)
                        .map(|at| &json[at + key.len()..])
                        .and_then(|rest| {
                            let end = rest
                                .find(|c: char| !(c.is_ascii_digit() || c == '.' || c == '-'))
                                .unwrap_or(rest.len());
                            rest[..end].parse().ok()
                        })
                        .unwrap_or(fallback)
                };
                PdfPage {
                    width: number("\"width\":", 612.),
                    height: number("\"height\":", 792.),
                    ..Default::default()
                }
            })
            .collect();
        self.dirty = true;
    }
    fn pixels_per_point(&self, page: &PdfPage) -> f64 {
        self.width.saturating_sub(2 * Self::MARGIN).max(1) as f64 / page.width
    }
    fn resize(&mut self, width: u32, height: u32) {
        if (width, height) == (self.width, self.height) {
            return;
        }
        let place = if self.total > 0. {
            self.position / self.total
        } else {
            0.
        };
        self.width = width;
        self.height = height;
        self.rendered.clear();
        self.total = Self::GAP;
        for i in 0..self.pages.len() {
            let high = (self.pages[i].height * self.pixels_per_point(&self.pages[i])).ceil();
            self.pages[i].top = self.total;
            self.pages[i].pixels_high = high;
            self.total += high + Self::GAP;
        }
        self.position = self.clamp(place * self.total);
        self.target = self.position;
        self.dirty = true;
    }
    fn clamp(&self, value: f64) -> f64 {
        value.min(self.total - self.height as f64).max(0.)
    }
    fn wheel(&mut self, delta: f64, precise: bool) {
        self.target = self.clamp(self.target + delta);
        if precise {
            self.position = self.target;
        }
        self.dirty = true;
    }
    fn wheel_notches(&mut self, notches: f64) {
        self.wheel(notches * 120., false);
    }
    fn screens(&mut self, count: f64) {
        self.wheel(count * self.height as f64 * 0.9, false);
    }
    fn animating(&self) -> bool {
        self.position != self.target
    }
    fn tick(&mut self) {
        let left = self.target - self.position;
        self.position = if left.abs() < 1. {
            self.target
        } else {
            self.position + left * 0.3
        };
        self.dirty = true;
    }
    fn render(&mut self, pdf: &mut Document, index: usize) -> Option<usize> {
        self.clock += 1;
        if let Some(found) = self.rendered.iter().position(|one| one.page == index) {
            self.rendered[found].used = self.clock;
            return Some(found);
        }
        let scale = self.pixels_per_point(&self.pages[index]) as f32;
        let image = pdf
            .render(
                index,
                &RenderOptions {
                    scale,
                    ..Default::default()
                },
            )
            .ok()?;
        self.renders += 1;
        self.rendered.push(RenderedPage {
            page: index,
            width: image.width as usize,
            height: image.height as usize,
            pixels: image
                .rgba
                .chunks_exact(4)
                .map(|p| (p[0] as u32) << 16 | (p[1] as u32) << 8 | p[2] as u32)
                .collect(),
            used: self.clock,
        });
        Some(self.rendered.len() - 1)
    }
    fn canvas(&mut self, pdf: &mut Document) -> &[u32] {
        if !self.dirty {
            return &self.canvas;
        }
        self.dirty = false;
        let (width, height) = (self.width as usize, self.height as usize);
        self.canvas.clear();
        self.canvas.resize(width * height, 0xc8c8c8);
        let (top, bottom) = (self.position, self.position + height as f64);
        let first = self
            .pages
            .partition_point(|page| page.top + page.pixels_high <= top);
        let mut last = first;
        for index in first..self.pages.len() {
            if self.pages[index].top >= bottom {
                break;
            }
            last = index;
            let Some(found) = self.render(pdf, index) else {
                continue;
            };
            let page = &self.rendered[found];
            let x = (width.saturating_sub(page.width)) / 2;
            let y = (self.pages[index].top - self.position).round() as i64;
            let copy = page.width.min(width - x);
            for row in 0..page.height {
                let to = y + row as i64;
                if to < 0 || to >= height as i64 {
                    continue;
                }
                let to = to as usize * width + x;
                let from = row * page.width;
                self.canvas[to..to + copy].copy_from_slice(&page.pixels[from..from + copy]);
            }
        }
        let mut kept: usize = self.rendered.iter().map(|one| one.pixels.len()).sum();
        while kept > Self::CACHE_PIXELS {
            let oldest = self
                .rendered
                .iter()
                .enumerate()
                .filter(|(_, one)| one.page < first || one.page > last)
                .min_by_key(|(_, one)| one.used)
                .map(|(i, _)| i);
            let Some(oldest) = oldest else { break };
            kept -= self.rendered.swap_remove(oldest).pixels.len();
        }
        if std::env::var_os("NATIVE_HOST_PDF_REPORT").is_some() {
            let sum = self
                .canvas
                .iter()
                .step_by(16)
                .fold(0u32, |sum, p| sum.wrapping_mul(31).wrapping_add(*p));
            println!(
                "pdf-pane: position={:.0} total={:.0} page={} pages={} visible={}-{} renders={} kept={} bytes={} checksum={:08x}",
                self.position,
                self.total,
                self.pages
                    .partition_point(|page| page.top <= self.position + height as f64 / 2.),
                self.pages.len(),
                first + 1,
                last + 1,
                self.renders,
                self.rendered.len(),
                kept * 4,
                sum
            );
        }
        &self.canvas
    }
}
impl App {
    fn resize_web(&self) {
        if let (Some(window), Some(view)) = (&self.window, &self.view) {
            let size = window.inner_size();
            let scale = window.scale_factor();
            view.set_size(
                ((size.width / 2) as f64 / scale).max(1.) as u32,
                (size.height as f64 / scale).max(1.) as u32,
                scale as f32,
            );
        }
    }
    fn paint(&mut self) {
        let Some(window) = &self.window else { return };
        let size = window.inner_size();
        if size.width < 2 || size.height == 0 {
            return;
        }
        let surface = self.surface.as_mut().unwrap();
        surface
            .resize(
                NonZeroU32::new(size.width).unwrap(),
                NonZeroU32::new(size.height).unwrap(),
            )
            .unwrap();
        let mut buffer = surface.buffer_mut().unwrap();
        buffer.fill(0xdddddd);
        let left = size.width / 2;
        let mut draw = |image: &Bitmap, x: i32, y: i32, width: u32, height: u32| {
            if !image.width.eq(&0) && image.height > 0 && width > 0 && height > 0 {
                for dy in 0..height {
                    let py = y + dy as i32;
                    if py < 0 || py >= size.height as i32 {
                        continue;
                    }
                    for dx in 0..width {
                        let px = x + dx as i32;
                        if px < 0 || px >= size.width as i32 {
                            continue;
                        }
                        let source = ((dy as u64 * image.height as u64 / height as u64)
                            * image.width as u64
                            + dx as u64 * image.width as u64 / width as u64)
                            as usize;
                        buffer[py as usize * size.width as usize + px as usize] =
                            image.pixels[source];
                    }
                }
            }
        };
        if let Some(web) = &self.web {
            draw(web, 0, 0, left, size.height);
        }
        if self.popup_shown {
            if let Some(popup) = &self.popup {
                let scale = window.scale_factor();
                draw(
                    popup,
                    (popup.bounds.x as f64 * scale) as i32,
                    (popup.bounds.y as f64 * scale) as i32,
                    popup.width,
                    popup.height,
                );
            }
        }
        if let Some(pdf) = self.pdf.as_mut() {
            self.pane.resize(size.width - left, size.height);
            let canvas = self.pane.canvas(pdf);
            let width = (size.width - left) as usize;
            for y in 0..size.height as usize {
                let to = y * size.width as usize + left as usize;
                buffer[to..to + width].copy_from_slice(&canvas[y * width..(y + 1) * width]);
            }
        }
        self.last_pixels = buffer.to_vec();
        buffer.present().unwrap();
    }
    fn stop(&mut self) {
        if !self.started {
            return;
        }
        if let Some(view) = self.view.take() {
            view.close();
        }
        self.pdf.take();
        assert!(zebkit::shutdown());
        self.started = false;
        println!(
            "winit host: {} frames, {} event-driven pumps, shutdown=ok",
            self.frames, self.pumps
        );
    }
    fn input_modifiers(&self) -> Modifiers {
        let mut flags = self.buttons;
        if self.modifiers.shift_key() {
            flags = flags | Modifiers::SHIFT;
        }
        if self.modifiers.control_key() {
            flags = flags | Modifiers::CONTROL;
        }
        if self.modifiers.alt_key() {
            flags = flags | Modifiers::ALT;
        }
        if self.modifiers.super_key() {
            flags = flags | Modifiers::META;
        }
        flags
    }
    fn logical_cursor(&self) -> (f32, f32) {
        let scale = self.window.as_ref().unwrap().scale_factor();
        (
            (self.cursor.x / scale) as f32,
            (self.cursor.y / scale) as f32,
        )
    }
    fn over_pdf(&self) -> bool {
        self.cursor.x >= (self.window.as_ref().unwrap().inner_size().width / 2) as f64
    }
    fn scroll_pdf(&mut self, change: impl FnOnce(&mut PdfPane)) {
        change(&mut self.pane);
        if self.pane.animating() && self.pdf_tick.is_none() {
            self.pdf_tick = Some(Instant::now() + Duration::from_millis(16));
        }
        self.window.as_ref().unwrap().request_redraw();
    }
}
impl ApplicationHandler<Event> for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.window.is_some() {
            return;
        }
        let window = Rc::new(
            event_loop
                .create_window(
                    Window::default_attributes()
                        .with_title("zebkit — winit web / PDF (the wheel scrolls the PDF)")
                        .with_inner_size(LogicalSize::new(1200., 740.)),
                )
                .unwrap(),
        );
        window.set_ime_allowed(true);
        self.surface = Some(Surface::new(&self.context, window.clone()).unwrap());
        self.window = Some(window.clone());
        let wake = self.proxy.clone();
        assert!(zebkit::set_wakeup(move |delay| {
            let _ = wake.send_event(Event::Wake(Instant::now() + delay));
        }));
        zebkit::init_with_settings(self.arguments.clone(), r#"{"headless":true}"#)
            .expect("initialize engine");
        self.started = true;
        let pdf_path = self
            .arguments
            .iter()
            .filter_map(|v| v.to_str())
            .find_map(|v| v.strip_prefix("--pdf="));
        self.pdf = Some(
            match pdf_path {
                Some(path) => Document::open_file(std::path::Path::new(path), None),
                None => Document::open(include_bytes!("../../sample.pdf"), None),
            }
            .expect("open PDF"),
        );
        self.pane.open(self.pdf.as_mut().unwrap());
        let view = View::new_offscreen(None, 600, 740, window.scale_factor() as f32)
            .expect("off-screen view");
        view.set_frame_buffers(Buffers::SharedMemory);
        let proxy = self.proxy.clone();
        view.on_frame(move |frame| {
            if let FrameBuffer::SharedMemory { pixels, stride, .. } = frame.buffer() {
                let width = frame.width();
                let height = frame.height();
                let mut copy = Vec::with_capacity(width as usize * height as usize);
                for y in 0..height as usize {
                    for pixel in pixels[y * stride..y * stride + width as usize * 4].chunks_exact(4)
                    {
                        copy.push(
                            (pixel[2] as u32) << 16 | (pixel[1] as u32) << 8 | pixel[0] as u32,
                        );
                    }
                }
                let _ = proxy.send_event(Event::Frame(
                    frame.layer(),
                    Bitmap {
                        width,
                        height,
                        pixels: copy,
                        bounds: frame.bounds(),
                    },
                ));
            }
        });
        let proxy = self.proxy.clone();
        view.on_popup(move |bounds| {
            let _ = proxy.send_event(Event::Popup(bounds.is_some()));
        });
        let proxy = self.proxy.clone();
        view.on_cursor(move |name, _| {
            let _ = proxy.send_event(Event::Cursor(name.into()));
        });
        let proxy = self.proxy.clone();
        view.on_caret(move |rect| {
            if let Some(rect) = rect {
                let _ = proxy.send_event(Event::Caret(rect));
            }
        });
        let url = self
            .arguments
            .iter()
            .filter_map(|v| v.to_str())
            .find_map(|v| v.strip_prefix("--url="));
        view.load_url(url.unwrap_or("data:text/html,<!doctype html><meta charset=utf-8><style>body{font:20px sans-serif;padding:24px}input,button,select{font:inherit;margin:8px}</style><h1>Your native window</h1><p>This page is a zebkit off-screen view.</p><input placeholder='Type here'><button onclick=\"this.textContent='Clicked'\">Click me</button><select><option>First</option><option>Second</option></select>"));
        self.view = Some(view);
        self.resize_web();
        self.deadline = Some(Instant::now());
        if self.smoke.is_some() {
            self.smoke_due = Some(Instant::now() + Duration::from_millis(1500));
        }
        if self.arguments.iter().any(|v| v == "--print-bounds") {
            println!("host-web-bounds: 0 0 {}", window.scale_factor());
        }
    }
    fn user_event(&mut self, _: &ActiveEventLoop, event: Event) {
        match event {
            Event::Wake(due) => self.deadline = Some(self.deadline.map_or(due, |old| old.min(due))),
            Event::Frame(layer, image) => {
                self.frames += 1;
                if layer == Layer::Popup {
                    self.popup = Some(image);
                } else {
                    self.web = Some(image);
                }
                self.window.as_ref().unwrap().request_redraw();
            }
            Event::Popup(shown) => {
                self.popup_shown = shown;
                if !shown {
                    self.popup = None;
                }
                self.window.as_ref().unwrap().request_redraw();
            }
            Event::Cursor(name) => self
                .window
                .as_ref()
                .unwrap()
                .set_cursor(match name.as_str() {
                    "pointer" => CursorIcon::Pointer,
                    "text" => CursorIcon::Text,
                    "wait" => CursorIcon::Wait,
                    "crosshair" => CursorIcon::Crosshair,
                    "ew-resize" => CursorIcon::EwResize,
                    "ns-resize" => CursorIcon::NsResize,
                    _ => CursorIcon::Default,
                }),
            Event::Caret(rect) => self.window.as_ref().unwrap().set_ime_cursor_area(
                winit::dpi::LogicalPosition::new(rect.x, rect.y),
                LogicalSize::new(rect.width, rect.height),
            ),
        }
    }
    fn window_event(&mut self, event_loop: &ActiveEventLoop, _: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => {
                self.stop();
                event_loop.exit();
            }
            WindowEvent::Resized(_) | WindowEvent::ScaleFactorChanged { .. } => {
                self.resize_web();
                self.window.as_ref().unwrap().request_redraw();
            }
            WindowEvent::RedrawRequested => self.paint(),
            WindowEvent::Focused(focus) => {
                if let Some(view) = &self.view {
                    view.set_focus(focus && !self.over_pdf());
                }
            }
            WindowEvent::ModifiersChanged(modifiers) => self.modifiers = modifiers.state(),
            WindowEvent::CursorMoved { position, .. } => {
                self.cursor = position;
                if !self.over_pdf() {
                    let (x, y) = self.logical_cursor();
                    if let Some(view) = &self.view {
                        view.send_mouse(
                            offscreen::MouseEvent::Move,
                            x,
                            y,
                            offscreen::MouseButton::None,
                            0,
                            self.input_modifiers(),
                        );
                    }
                }
            }
            WindowEvent::CursorLeft { .. } => {
                if let Some(view) = &self.view {
                    view.send_mouse(
                        offscreen::MouseEvent::Leave,
                        0.,
                        0.,
                        offscreen::MouseButton::None,
                        0,
                        self.input_modifiers(),
                    );
                }
            }
            WindowEvent::MouseInput { state, button, .. } => {
                let (button, flag) = match button {
                    MouseButton::Left => (offscreen::MouseButton::Left, Modifiers::LEFT_BUTTON),
                    MouseButton::Middle => {
                        (offscreen::MouseButton::Middle, Modifiers::MIDDLE_BUTTON)
                    }
                    MouseButton::Right => (offscreen::MouseButton::Right, Modifiers::RIGHT_BUTTON),
                    _ => (offscreen::MouseButton::None, Modifiers::NONE),
                };
                if state == ElementState::Pressed {
                    // A double click is the same button again within 400 ms.
                    let now = std::time::Instant::now();
                    self.clicks = match self.last_press {
                        Some((last, at)) if last == button && now.duration_since(at).as_millis() < 400 => self.clicks + 1,
                        _ => 1,
                    };
                    self.last_press = Some((button, now));
                    self.buttons = self.buttons | flag;
                } else {
                    self.buttons.0 &= !flag.0;
                }
                if let Some(view) = &self.view {
                    view.set_focus(!self.over_pdf());
                    if !self.over_pdf() {
                        let (x, y) = self.logical_cursor();
                        view.send_mouse(
                            if state == ElementState::Pressed {
                                offscreen::MouseEvent::Down
                            } else {
                                offscreen::MouseEvent::Up
                            },
                            x,
                            y,
                            button,
                            self.clicks,
                            self.input_modifiers(),
                        );
                    }
                }
            }
            WindowEvent::MouseWheel { delta, .. } => {
                let (dx, dy, precise) = match delta {
                    MouseScrollDelta::LineDelta(x, y) => (x * 40., y * 40., false),
                    MouseScrollDelta::PixelDelta(p) => {
                        let scale = self.window.as_ref().unwrap().scale_factor() as f32;
                        (p.x as f32 / scale, p.y as f32 / scale, true)
                    }
                };
                if self.over_pdf() {
                    if dy != 0. {
                        // dy is positive up the document here.
                        self.scroll_pdf(|pane| {
                            if precise {
                                pane.wheel(-dy as f64, true)
                            } else {
                                pane.wheel_notches(-dy as f64 / 40.)
                            }
                        });
                    }
                } else if let Some(view) = &self.view {
                    let (x, y) = self.logical_cursor();
                    view.send_wheel(
                        x,
                        y,
                        -dx,
                        -dy,
                        precise,
                        offscreen::WheelPhase::None,
                        self.input_modifiers(),
                    );
                }
            }
            WindowEvent::KeyboardInput { event, .. } => {
                if self.over_pdf() {
                    if event.state == ElementState::Pressed {
                        if event.logical_key == Key::Named(NamedKey::PageDown) {
                            self.scroll_pdf(|pane| pane.screens(1.));
                        } else if event.logical_key == Key::Named(NamedKey::PageUp) {
                            self.scroll_pdf(|pane| pane.screens(-1.));
                        }
                    }
                    return;
                }
                if let Some(view) = &self.view {
                    let code = match &event.logical_key {
                        Key::Character(s) if s.len() == 1 => {
                            s.as_bytes()[0].to_ascii_uppercase() as i32
                        }
                        Key::Named(n) => match n {
                            NamedKey::Enter => 13,
                            NamedKey::Backspace => 8,
                            NamedKey::Tab => 9,
                            NamedKey::Escape => 27,
                            NamedKey::ArrowLeft => 37,
                            NamedKey::ArrowUp => 38,
                            NamedKey::ArrowRight => 39,
                            NamedKey::ArrowDown => 40,
                            NamedKey::Delete => 46,
                            NamedKey::Home => 36,
                            NamedKey::End => 35,
                            _ => 0,
                        },
                        _ => 0,
                    };
                    let physical = match event.physical_key {
                        PhysicalKey::Code(code) => Some(format!("{code:?}")),
                        _ => None,
                    };
                    view.send_key(&offscreen::KeyEvent {
                        kind: if event.state == ElementState::Pressed {
                            offscreen::KeyEventType::Down
                        } else {
                            offscreen::KeyEventType::Up
                        },
                        modifiers: self.input_modifiers()
                            | if event.repeat {
                                Modifiers::AUTO_REPEAT
                            } else {
                                Modifiers::NONE
                            },
                        key_code: code,
                        native_key_code: 0,
                        code: physical.as_deref(),
                        key: None,
                        text: None,
                        unmodified_text: None,
                    });
                    if event.state == ElementState::Pressed
                        && !self.modifiers.control_key()
                        && !self.modifiers.super_key()
                        && !matches!(code, 8 | 9 | 13 | 27 | 46)
                    {
                        if let Some(text) = event.text {
                            view.ime_commit(&text);
                        }
                    }
                }
            }
            WindowEvent::Ime(Ime::Commit(text)) => {
                if let Some(view) = &self.view {
                    view.ime_commit(&text);
                }
            }
            WindowEvent::Ime(Ime::Preedit(text, cursor)) => {
                if let Some(view) = &self.view {
                    if text.is_empty() {
                        view.ime_cancel();
                    } else {
                        view.ime_set_composition(
                            &text,
                            cursor.map(|(a, b)| {
                                (
                                    text[..a].encode_utf16().count() as u32,
                                    text[..b].encode_utf16().count() as u32,
                                )
                            }),
                        );
                    }
                }
            }
            _ => {}
        }
    }
    fn about_to_wait(&mut self, event_loop: &ActiveEventLoop) {
        // winit drops the wheel XTest gives (it is flagged as emulated), so
        // tests/engine/native_host_pdf_scroll.py has this host wheel itself, through what
        // a wheel event calls.
        if self.scroll_test && self.started && self.scroll_due.is_none_or(|due| due <= Instant::now()) {
            self.scroll_due = Some(Instant::now() + Duration::from_millis(20));
            if self.scroll_up > 0 || self.pane.position + self.pane.height as f64 >= self.pane.total - 1. {
                self.scroll_up += 1;
            }
            if self.scroll_up >= 60 {
                self.stop();
                event_loop.exit();
                return;
            }
            let notches = if self.scroll_up > 0 { -1. } else { 12. };
            self.scroll_pdf(|pane| pane.wheel_notches(notches));
        }
        if self.pdf_tick.is_some_and(|due| due <= Instant::now()) {
            self.pane.tick();
            self.pdf_tick = self
                .pane
                .animating()
                .then(|| Instant::now() + Duration::from_millis(16));
            self.window.as_ref().unwrap().request_redraw();
        }
        if self.started && self.deadline.is_some_and(|due| due <= Instant::now()) {
            self.deadline = None;
            self.pumps += 1;
            assert!(zebkit::pump());
        }
        if self.smoke_due.is_some_and(|due| due <= Instant::now())
            && self.frames > 0
            && !self.last_pixels.is_empty()
        {
            let size = self.window.as_ref().unwrap().inner_size();
            let rgba = self
                .last_pixels
                .iter()
                .flat_map(|p| [(p >> 16) as u8, (p >> 8) as u8, *p as u8, 255])
                .collect();
            let image = zebkit::services::Image {
                width: size.width,
                height: size.height,
                rgba,
            };
            let png =
                zebkit::services::encode_image(zebkit::services::ImageFormat::Png, &image, 100)
                    .unwrap();
            std::fs::write(self.smoke.as_ref().unwrap(), png).unwrap();
            self.stop();
            event_loop.exit();
            return;
        }
        let next = [self.deadline, self.smoke_due, self.pdf_tick, self.scroll_due]
            .into_iter()
            .flatten()
            .min();
        event_loop.set_control_flow(next.map_or(ControlFlow::Wait, ControlFlow::WaitUntil));
    }
}
fn main() {
    let arguments: Vec<_> = std::env::args_os().collect();
    let smoke = arguments
        .iter()
        .filter_map(|v| v.to_str())
        .find_map(|v| v.strip_prefix("--smoke-test="))
        .map(PathBuf::from);
    let event_loop = EventLoop::<Event>::with_user_event().build().unwrap();
    let context = Context::new(event_loop.owned_display_handle()).unwrap();
    let mut app = App {
        proxy: event_loop.create_proxy(),
        context,
        surface: None,
        window: None,
        view: None,
        pdf: None,
        pane: PdfPane::default(),
        pdf_tick: None,
        scroll_test: arguments.iter().any(|a| a == "--scroll-test"),
        scroll_due: None,
        scroll_up: 0,
        web: None,
        popup: None,
        popup_shown: false,
        cursor: PhysicalPosition::new(0., 0.),
        modifiers: ModifiersState::empty(),
        buttons: Modifiers::NONE,
        clicks: 1,
        last_press: None,
        deadline: None,
        smoke_due: None,
        smoke,
        arguments,
        started: false,
        frames: 0,
        pumps: 0,
        last_pixels: Vec::new(),
    };
    event_loop.run_app(&mut app).unwrap();
    app.stop();
}
