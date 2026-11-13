//! Playwright's API for a view's page, natively (`<zebkit/automation.h>`,
//! docs/AUTOMATION.md): each command is one of Playwright's protocol, run
//! inside the engine without CDP or a driver process. The names are those
//! of Playwright's API (and of `zebkit.automation` in Python), in Rust's
//! case; each command calls its `done` with the result once it has one.
//!
//! ```no_run
//! use zebkit::automation::{expect, Page, RoleOptions};
//! use zebkit::View;
//!
//! std::process::exit(zebkit::run(|| {
//!     let page = Page::new(View::new(None, 1280, 720));
//!     let next = page.clone();
//!     page.goto("https://example.com/", move |_| {
//!         let link = next.get_by_role("link", RoleOptions::name("More information"));
//!         link.click(move |result| {
//!             println!("clicked: {result:?}");
//!             expect(&next.locator("h1")).to_have_text("Example Domains", |result| {
//!                 println!("{result:?}");
//!                 zebkit::quit();
//!             });
//!         });
//!     });
//! }));
//! ```
//!
//! Values are [`Value`]s (as the protocol has them, with zebkit's
//! differences: plain JSON values, handles as `{"handle": "h3", ...}`);
//! [`call`] and the `command` methods run any command of [`commands`] with
//! its params as the protocol names them. Options a method takes as a
//! [`Value`] are the protocol's too (`{"timeout": 300.0, "force": true}`);
//! `Value::Null` is none.

use crate::json::{self, Value};
use crate::{c_string, script_result, sys, take_string, Error, View};
use std::cell::{Cell, RefCell};
use std::collections::{HashMap, VecDeque};
use std::os::raw::{c_char, c_int, c_void};
use std::rc::Rc;

/// The commands the engine has, by their protocol names.
pub fn commands() -> Vec<String> {
    let text = unsafe { take_string(sys::zebkit_automation_commands()) }.unwrap_or_default();
    Value::parse(&text)
        .ok()
        .and_then(|value| value.as_array().map(|names| names.iter().filter_map(|name| name.as_str().map(str::to_string)).collect()))
        .unwrap_or_default()
}

/// Runs Playwright's command `method` on `view`'s page with `params` (a
/// JSON object) and calls `done` with its result as JSON or the error.
pub fn call<F: FnOnce(Result<String, Error>) + 'static>(view: &View, method: &str, params: &str, done: F) {
    let Some(raw) = view.raw() else {
        done(Err(Error::new("the view is closed")));
        return;
    };
    send(raw, method, params, done);
}

/// Runs a command that names no view: "Browser.newContext" and a
/// context's ("BrowserContext.*" with its "context", "APIRequestContext.fetch").
pub fn call_global<F: FnOnce(Result<String, Error>) + 'static>(method: &str, params: &str, done: F) {
    send(std::ptr::null_mut(), method, params, done);
}

fn send<F: FnOnce(Result<String, Error>) + 'static>(raw: *mut sys::zebkit_view, method: &str, params: &str, done: F) {
    let data = Box::into_raw(Box::new(done));
    unsafe {
        sys::zebkit_automation_call(raw, c_string(method).as_ptr(), c_string(params).as_ptr(), Some(on_done::<F>), data.cast())
    }
}

unsafe extern "C" fn on_done<F: FnOnce(Result<String, Error>)>(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    let result = script_result(json, error);
    crate::guard(|| done(result));
}

/// A command's result, read: `{}` for none.
type Answer = Result<Value, Error>;

/// Runs `method` with `params` (an object, its null members left out) on
/// `view`'s page, or on none.
fn run(view: Option<&View>, method: &str, params: Value, done: impl FnOnce(Answer) + 'static) {
    let params = match params {
        Value::Object(members) => Value::Object(members.into_iter().filter(|(_, value)| !value.is_null()).collect()),
        Value::Null => Value::Object(Vec::new()),
        other => other,
    };
    let finish = move |result: Result<String, Error>| {
        done(result.and_then(|text| if text.is_empty() || text == "null" { Ok(Value::Object(Vec::new())) } else { Value::parse(&text) }))
    };
    match view {
        Some(view) => call(view, method, &params.to_string(), finish),
        None => call_global(method, &params.to_string(), finish),
    }
}

/// `done` for a command whose answer is read by `read`.
fn then<T>(done: impl FnOnce(Result<T, Error>) + 'static, read: impl FnOnce(Value) -> Result<T, Error> + 'static) -> impl FnOnce(Answer) + 'static {
    move |answer| done(answer.and_then(read))
}

/// `done` for a command that answers nothing.
fn unit(done: impl FnOnce(Result<(), Error>) + 'static) -> impl FnOnce(Answer) + 'static {
    then(done, |_| Ok(()))
}

/// `done` for a command that answers `{"value": ...}`.
fn value(done: impl FnOnce(Result<Value, Error>) + 'static) -> impl FnOnce(Answer) + 'static {
    then(done, |answer| Ok(member(answer, "value")))
}

/// The member `key` of `value`, taken out.
fn member(value: Value, key: &str) -> Value {
    match value {
        Value::Object(members) => members.into_iter().find(|(name, _)| name == key).map(|(_, value)| value).unwrap_or_default(),
        _ => Value::Null,
    }
}

fn string_value(done: impl FnOnce(Result<Option<String>, Error>) + 'static) -> impl FnOnce(Answer) + 'static {
    then(done, |answer| Ok(answer.text("value")))
}

fn text_value(done: impl FnOnce(Result<String, Error>) + 'static) -> impl FnOnce(Answer) + 'static {
    then(done, |answer| Ok(answer.text("value").unwrap_or_default()))
}

fn bool_value(done: impl FnOnce(Result<bool, Error>) + 'static) -> impl FnOnce(Answer) + 'static {
    then(done, |answer| Ok(answer["value"].as_bool().unwrap_or(false)))
}

/// `{"name": ..., "value": ...}` of each header.
fn header_list(headers: &[(&str, &str)]) -> Value {
    Value::Array(headers.iter().map(|(name, value)| Value::object([("name", (*name).into()), ("value", (*value).into())])).collect())
}

/// The headers of a request or a response, their names in lower case.
fn headers_of(value: &Value) -> Vec<(String, String)> {
    value["headers"]
        .as_array()
        .unwrap_or_default()
        .iter()
        .map(|header| (header.text("name").unwrap_or_default().to_lowercase(), header.text("value").unwrap_or_default()))
        .collect()
}

fn milliseconds(timeout: Option<f64>) -> Value {
    timeout.into()
}

/// Whether `expression` reads as a function rather than an expression:
/// `function`, `(a, b) =>`, `a =>`, each maybe `async` (as Python's
/// binding tells them).
fn looks_like_function(expression: &str) -> bool {
    let text = expression.trim();
    let text = match text.strip_prefix("async") {
        Some(rest) if rest.starts_with(char::is_whitespace) => rest.trim_start(),
        _ => text,
    };
    let word = |c: char| c.is_alphanumeric() || c == '_';
    if let Some(rest) = text.strip_prefix("function") {
        if !rest.starts_with(word) {
            return true;
        }
    }
    let arrow = |rest: &str| rest.trim_start().starts_with("=>");
    // \(?[\w$\s,]*\)?\s*=>
    let rest = text.strip_prefix('(').unwrap_or(text);
    let rest = rest.trim_start_matches(|c: char| word(c) || c == '$' || c.is_whitespace() || c == ',');
    if arrow(rest.strip_prefix(')').unwrap_or(rest)) {
        return true;
    }
    // \([^)]*\)\s*=>
    text.strip_prefix('(').and_then(|rest| rest.find(')').map(|at| &rest[at + 1..])).is_some_and(arrow)
}

impl Error {
    /// Whether this is a command whose timeout passed (Playwright's
    /// TimeoutError, `zebkit.automation.TimeoutError` in Python).
    pub fn is_timeout(&self) -> bool {
        self.message().starts_with("Timeout ")
    }
}

// ---- Base64, for the protocol's binaries ---------------------------------

const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

fn base64_encode(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let n = (u32::from(chunk[0]) << 16) | (u32::from(*chunk.get(1).unwrap_or(&0)) << 8) | u32::from(*chunk.get(2).unwrap_or(&0));
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(ALPHABET[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

fn base64_decode(text: &str) -> Result<Vec<u8>, Error> {
    let mut out = Vec::with_capacity(text.len() / 4 * 3);
    let (mut bits, mut count) = (0u32, 0);
    for byte in text.bytes() {
        let digit = match byte {
            b'A'..=b'Z' => byte - b'A',
            b'a'..=b'z' => byte - b'a' + 26,
            b'0'..=b'9' => byte - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            b'=' | b'\n' | b'\r' | b' ' => continue,
            _ => return Err(Error::new("invalid base64")),
        };
        bits = (bits << 6) | u32::from(digit);
        count += 6;
        if count >= 8 {
            count -= 8;
            out.push((bits >> count) as u8);
        }
    }
    Ok(out)
}

/// The base64 member `key` of an answer, decoded.
fn binary(answer: &Value, key: &str) -> Result<Vec<u8>, Error> {
    base64_decode(answer[key].as_str().ok_or_else(|| Error::new(format!("no {key} in the answer")))?)
}

fn write_file(path: Option<&str>, data: Vec<u8>) -> Result<Vec<u8>, Error> {
    if let Some(path) = path {
        std::fs::write(path, &data).map_err(|error| Error::new(format!("{path}: {error}")))?;
    }
    Ok(data)
}

/// `path` made absolute, as the engine takes files.
fn absolute(path: &str) -> String {
    std::path::absolute(path).map(|path| path.to_string_lossy().into_owned()).unwrap_or_else(|_| path.to_string())
}

// ---- Pixels -----------------------------------------------------------------

/// A screenshot's pixels, unencoded (zebkit's own, beyond Playwright's
/// API): `height` rows of `stride` bytes, 4 bytes a pixel, R, G, B, A, not
/// premultiplied, the pixels of the PNG `Page.screenshot` gives. The
/// engine's, for as long as the function they are given to runs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Pixels<'a> {
    pub width: u32,
    pub height: u32,
    pub stride: usize,
    pub data: &'a [u8],
}

/// `Page.screenshot` (or `ElementHandle.screenshot` with its "handle") on
/// `view`'s page with `params` (the command's but type, quality and path),
/// answered with the pixels instead of an encoded picture.
pub fn screenshot_pixels<F: FnOnce(Result<Pixels<'_>, Error>) + 'static>(view: &View, method: &str, params: &str, done: F) {
    let Some(raw) = view.raw() else {
        done(Err(Error::new("the view is closed")));
        return;
    };
    let data = Box::into_raw(Box::new(done));
    unsafe {
        sys::zebkit_automation_screenshot_pixels(raw, c_string(method).as_ptr(), c_string(params).as_ptr(), Some(on_pixels::<F>), data.cast())
    }
}

unsafe extern "C" fn on_pixels<F: FnOnce(Result<Pixels<'_>, Error>)>(
    _view: *mut sys::zebkit_view,
    pixels: *const u8,
    width: c_int,
    height: c_int,
    stride: c_int,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    let result = if let Some(message) = crate::string_from(error) {
        Err(Error::new(message))
    } else if pixels.is_null() || width <= 0 || height <= 0 || stride < width * 4 {
        Err(Error::new("no pixels"))
    } else {
        let size = stride as usize * height as usize;
        Ok(Pixels {
            width: width as u32,
            height: height as u32,
            stride: stride as usize,
            data: std::slice::from_raw_parts(pixels, size),
        })
    };
    crate::guard(|| done(result));
}

// ---- Events -------------------------------------------------------------------

/// Calls `function(event, json)` for the page's events, as Playwright's
/// protocol names them ("Page.request", "BrowserContext.dialog", ...).
/// One function a view, which replaces a [`Page`]'s own; it is let go after
/// the view's last events, once the view has closed (or, when the engine
/// stops first, once it has stopped).
pub fn on_event<F: FnMut(&str, &str) + 'static>(view: &View, function: F) {
    let Some(raw) = view.raw() else { return };
    view.inner.automation_event.set(Some(Box::new(function)));
    let data = Rc::as_ptr(&view.inner).cast_mut().cast::<c_void>();
    unsafe { sys::zebkit_automation_on_event(raw, Some(on_event_raw), data) }
}

/// Stops [`on_event`]'s function.
pub fn clear_event_handler(view: &View) {
    if let Some(raw) = view.raw() {
        unsafe { sys::zebkit_automation_on_event(raw, None, std::ptr::null_mut()) }
    }
    view.inner.automation_event.set(None);
}

/// The id of `view`'s page, as `BrowserContext.pages` and the events name it.
pub fn page_id(view: &View) -> Option<String> {
    let raw = view.raw()?;
    unsafe { take_string(sys::zebkit_automation_page_id(raw)) }
}

unsafe extern "C" fn on_event_raw(_view: *mut sys::zebkit_view, event: *const c_char, json: *const c_char, data: *mut c_void) {
    // The view's state stays until a task after it closed (view.rs).
    let inner = crate::view::hold(data);
    let event = crate::string_from(event).unwrap_or_default();
    let json = crate::string_from(json).unwrap_or_else(|| "{}".to_string());
    inner.automation_event.call(|function| function(&event, &json));
}

/// Serves Playwright's own protocol on 127.0.0.1:`port` (0 picks one), what
/// `chromium.connect(url)` of each Playwright client speaks; `done` gets
/// the url (`ws://127.0.0.1:<port>/<random token>`), the credential to connect
/// with: only that path connects, and no request with an `Origin` header.
pub fn listen<F: FnOnce(Result<String, Error>) + 'static>(port: u16, done: F) {
    let data = Box::into_raw(Box::new(done));
    unsafe { sys::zebkit_automation_listen(c_int::from(port), Some(on_listen::<F>), data.cast()) }
}

unsafe extern "C" fn on_listen<F: FnOnce(Result<String, Error>)>(url: *const c_char, error: *const c_char, data: *mut c_void) {
    let done = Box::from_raw(data.cast::<F>());
    let result = match crate::string_from(url) {
        Some(url) => Ok(url),
        None => Err(Error::new(crate::string_from(error).unwrap_or_default())),
    };
    crate::guard(|| done(result));
}

/// `text` as a JSON string.
pub fn json_string(text: &str) -> String {
    json::quote(text)
}

// ---- Selectors, as Playwright's locatorUtils.ts makes them ---------------

/// Text to match: a string (as a substring, ignoring case, unless exact) or
/// a regular expression's source and flags (JavaScript's, `"i"`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Text {
    Plain(String),
    Regex { source: String, flags: String },
}

impl Text {
    pub fn regex(source: &str, flags: &str) -> Text {
        Text::Regex { source: source.to_string(), flags: flags.to_string() }
    }

    fn selector(&self, exact: bool) -> String {
        match self {
            Text::Plain(text) => format!("{}{}", json::quote(text), if exact { "s" } else { "i" }),
            Text::Regex { source, flags } => format!("/{}/{}", source.replace('/', "\\/"), if flags.contains('i') { "i" } else { "" }),
        }
    }

    fn attribute(&self, exact: bool) -> String {
        match self {
            Text::Plain(value) => format!("\"{}\"{}", value.replace('\\', "\\\\").replace('"', "\\\""), if exact { "s" } else { "i" }),
            Text::Regex { .. } => self.selector(exact),
        }
    }

    /// As `Frame.expect`'s expectedText has it.
    fn expected(&self, substring: Option<bool>, normalize: Option<bool>, ignore_case: Option<bool>) -> Value {
        let mut entry = match self {
            Text::Plain(text) => Value::object([("string", text.as_str().into())]),
            Text::Regex { source, flags } => Value::object([
                ("regexSource", source.as_str().into()),
                ("regexFlags", (if flags.contains('i') { "i" } else { "" }).into()),
            ]),
        };
        for (key, flag) in [("matchSubstring", substring), ("normalizeWhiteSpace", normalize), ("ignoreCase", ignore_case)] {
            if let Some(flag) = flag {
                entry.set(key, flag.into());
            }
        }
        entry
    }
}

impl From<&str> for Text {
    fn from(text: &str) -> Text {
        Text::Plain(text.to_string())
    }
}

impl From<String> for Text {
    fn from(text: String) -> Text {
        Text::Plain(text)
    }
}

impl From<&String> for Text {
    fn from(text: &String) -> Text {
        Text::Plain(text.clone())
    }
}

/// `get_by_role`'s options, as Playwright's.
#[derive(Clone, Debug, Default)]
pub struct RoleOptions {
    pub name: Option<Text>,
    pub exact: bool,
    pub checked: Option<bool>,
    pub disabled: Option<bool>,
    pub selected: Option<bool>,
    pub expanded: Option<bool>,
    pub include_hidden: Option<bool>,
    pub level: Option<u32>,
    pub pressed: Option<bool>,
}

impl RoleOptions {
    /// Only the accessible name.
    pub fn name(name: impl Into<Text>) -> RoleOptions {
        RoleOptions { name: Some(name.into()), ..Default::default() }
    }

    fn selector(&self, role: &str) -> String {
        let mut out = format!("internal:role={role}");
        let flags = [
            ("checked", self.checked),
            ("disabled", self.disabled),
            ("selected", self.selected),
            ("expanded", self.expanded),
            ("include-hidden", self.include_hidden),
        ];
        for (key, flag) in flags {
            if let Some(flag) = flag {
                out.push_str(&format!("[{key}={flag}]"));
            }
        }
        if let Some(level) = self.level {
            out.push_str(&format!("[level={level}]"));
        }
        if let Some(name) = &self.name {
            out.push_str(&format!("[name={}]", name.attribute(self.exact)));
        }
        if let Some(pressed) = self.pressed {
            out.push_str(&format!("[pressed={pressed}]"));
        }
        out
    }
}

/// `Locator::filter`'s options.
#[derive(Clone, Default)]
pub struct Filter {
    pub has_text: Option<Text>,
    pub has_not_text: Option<Text>,
    pub has: Option<Locator>,
    pub has_not: Option<Locator>,
    pub visible: Option<bool>,
}

// ---- Pages and frames -------------------------------------------------------

/// What a page's objects share: its view and the state of its listeners.
#[derive(Clone)]
struct Core {
    view: View,
    state: Rc<State>,
}

type Listener = Rc<RefCell<Box<dyn FnMut(&PageEvent)>>>;
type ProtocolListener = Rc<RefCell<Box<dyn FnMut(&str, &Value)>>>;
type RouteHandler = Rc<RefCell<Box<dyn FnMut(Route, Request)>>>;
type BindingFunction = Rc<RefCell<Box<dyn FnMut(BindingCall)>>>;

struct RouteEntry {
    pattern: Value,
    matches: Rc<dyn Fn(&str) -> bool>,
    handler: RouteHandler,
}

#[derive(Default)]
struct State {
    next_listener: Cell<u64>,
    listeners: RefCell<Vec<(u64, Option<EventKind>, Listener)>>,
    protocol_listeners: RefCell<Vec<(u64, ProtocolListener)>>,
    routes: RefCell<Vec<RouteEntry>>,
    bindings: RefCell<HashMap<String, BindingFunction>>,
    workers: RefCell<Vec<Worker>>,
    /// The last requests, by id: what a request's end names.
    requests: RefCell<VecDeque<Request>>,
    /// The `enabled: false` subscriptions sent and not answered yet, for
    /// dialogs and file choosers: one held before it is answered has no
    /// listener left to answer it.
    unsubscribing: [Cell<u32>; 2],
}

impl State {
    fn listening(&self, kind: EventKind) -> bool {
        self.listeners.borrow().iter().any(|(_, listening, _)| *listening == Some(kind))
    }

    fn clear(&self) {
        let listeners = std::mem::take(&mut *self.listeners.borrow_mut());
        let protocol = std::mem::take(&mut *self.protocol_listeners.borrow_mut());
        let routes = std::mem::take(&mut *self.routes.borrow_mut());
        let bindings = std::mem::take(&mut *self.bindings.borrow_mut());
        let workers = std::mem::take(&mut *self.workers.borrow_mut());
        let requests = std::mem::take(&mut *self.requests.borrow_mut());
        drop((listeners, protocol, routes, bindings, workers, requests));
    }
}

/// Lets go what a page's listeners hold once the view's last events have
/// come: they may hold the page.
struct Releases(Rc<State>);

impl Drop for Releases {
    fn drop(&mut self) {
        self.0.clear();
    }
}

impl Core {
    fn run(&self, method: &str, params: Value, done: impl FnOnce(Answer) + 'static) {
        run(Some(&self.view), method, params, done);
    }
}

/// What [`Page::add_init_script`] added, until [`Disposable::dispose`]
/// takes it away (Playwright's Disposable).
#[derive(Clone)]
pub struct Disposable {
    view: View,
    registration: String,
}

impl Disposable {
    /// The protocol's name for it (`Page.removeRegistration`'s `registration`).
    pub fn registration(&self) -> &str {
        &self.registration
    }

    /// Takes it away: documents from now on do not get it.
    pub fn dispose<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        run(Some(&self.view), "Page.removeRegistration", Value::object([("registration", self.registration.as_str().into())]), unit(done));
    }
}

/// A page: a view and its main frame, with Playwright's Page API (and the
/// main frame's, through `Deref`). One `Page` a view: it takes the view's
/// automation events ([`on_event`]).
#[derive(Clone)]
pub struct Page {
    main: Frame,
}

impl std::ops::Deref for Page {
    type Target = Frame;
    fn deref(&self) -> &Frame {
        &self.main
    }
}

/// What [`Page::on`] listens to: Playwright's page events.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[non_exhaustive]
pub enum EventKind {
    Console,
    Dialog,
    FileChooser,
    Download,
    Request,
    Response,
    RequestFinished,
    RequestFailed,
    FrameNavigated,
    FrameAttached,
    FrameDetached,
    LoadState,
    Worker,
    Crash,
    Close,
}

/// An event of the page, as [`Page::on`] gives it.
#[derive(Clone)]
#[non_exhaustive]
pub enum PageEvent {
    Console(ConsoleMessage),
    Dialog(Dialog),
    FileChooser(FileChooser),
    Download(Download),
    Request(Request),
    Response(Response),
    RequestFinished(Request),
    RequestFailed(Request),
    /// The frame's `{"frame", "url", "name", ...}`.
    FrameNavigated(Value),
    FrameAttached(Value),
    FrameDetached(Value),
    LoadState(Value),
    Worker(Worker),
    Crash,
    Close,
}

impl PageEvent {
    pub fn kind(&self) -> EventKind {
        match self {
            PageEvent::Console(_) => EventKind::Console,
            PageEvent::Dialog(_) => EventKind::Dialog,
            PageEvent::FileChooser(_) => EventKind::FileChooser,
            PageEvent::Download(_) => EventKind::Download,
            PageEvent::Request(_) => EventKind::Request,
            PageEvent::Response(_) => EventKind::Response,
            PageEvent::RequestFinished(_) => EventKind::RequestFinished,
            PageEvent::RequestFailed(_) => EventKind::RequestFailed,
            PageEvent::FrameNavigated(_) => EventKind::FrameNavigated,
            PageEvent::FrameAttached(_) => EventKind::FrameAttached,
            PageEvent::FrameDetached(_) => EventKind::FrameDetached,
            PageEvent::LoadState(_) => EventKind::LoadState,
            PageEvent::Worker(_) => EventKind::Worker,
            PageEvent::Crash => EventKind::Crash,
            PageEvent::Close => EventKind::Close,
        }
    }
}

/// What [`Page::on`] returns, for [`Page::remove_listener`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ListenerId(u64);

impl Page {
    /// `view`'s page; its automation events come to this page's listeners.
    pub fn new(view: View) -> Page {
        let state = Rc::new(State::default());
        let core = Core { view: view.clone(), state: state.clone() };
        let page = Page { main: Frame { core: core.clone(), id: None, name: String::new(), url: String::new() } };
        let releases = Releases(state);
        let weak = Rc::downgrade(&core.state);
        let view_for_events = view.clone();
        on_event(&view, move |event, json| {
            let _ = &releases;
            if let Some(state) = weak.upgrade() {
                let core = Core { view: view_for_events.clone(), state };
                dispatch(&core, event, json);
            }
        });
        page
    }

    /// A page of a new view, `width` by `height`.
    pub fn open(width: u32, height: u32) -> Result<Page, Error> {
        View::try_new(None, width, height).map(Page::new)
    }

    fn from_core(core: Core) -> Page {
        Page { main: Frame { core, id: None, name: String::new(), url: String::new() } }
    }

    fn core(&self) -> &Core {
        &self.main.core
    }

    pub fn view(&self) -> &View {
        &self.main.core.view
    }

    pub fn main_frame(&self) -> Frame {
        self.main.clone()
    }

    /// A frame by its id (from [`Page::frames`] or an event).
    pub fn frame(&self, id: &str) -> Frame {
        Frame { core: self.core().clone(), id: Some(id.to_string()), name: String::new(), url: String::new() }
    }

    /// Calls `listener` for each event of `kind`. A dialog or file chooser
    /// listener holds the page's dialogs or choosers for it to answer.
    pub fn on<F: FnMut(&PageEvent) + 'static>(&self, kind: EventKind, listener: F) -> ListenerId {
        let state = &self.core().state;
        let id = state.next_listener.get() + 1;
        state.next_listener.set(id);
        let first = !state.listening(kind);
        state.listeners.borrow_mut().push((id, Some(kind), Rc::new(RefCell::new(Box::new(listener)))));
        if first {
            self.subscribe(kind, true);
        }
        ListenerId(id)
    }

    /// Asks for, or stops, the page's held dialogs or file choosers: the
    /// engine holds them only while a listener of theirs is there.
    fn subscribe(&self, kind: EventKind, enabled: bool) {
        let (event, at) = match kind {
            EventKind::Dialog => ("dialog", 0),
            EventKind::FileChooser => ("fileChooser", 1),
            _ => return,
        };
        let params = Value::object([("event", event.into()), ("enabled", enabled.into())]);
        // At once: commands run in the order they are given.
        if enabled {
            self.core().run("Page.updateSubscription", params, |_| {});
            return;
        }
        let state = &self.core().state;
        state.unsubscribing[at].set(state.unsubscribing[at].get() + 1);
        let weak = Rc::downgrade(state);
        self.core().run("Page.updateSubscription", params, move |_| {
            if let Some(state) = weak.upgrade() {
                state.unsubscribing[at].set(state.unsubscribing[at].get().saturating_sub(1));
            }
        });
    }

    /// Calls `listener(event, params)` for every protocol event of the page
    /// ("Page.request", "BrowserContext.dialog", ...), before [`Page::on`]'s.
    pub fn on_protocol_event<F: FnMut(&str, &Value) + 'static>(&self, listener: F) -> ListenerId {
        let state = &self.core().state;
        let id = state.next_listener.get() + 1;
        state.next_listener.set(id);
        state.protocol_listeners.borrow_mut().push((id, Rc::new(RefCell::new(Box::new(listener)))));
        ListenerId(id)
    }

    /// Removes a listener [`Page::on`] or [`Page::on_protocol_event`] added.
    pub fn remove_listener(&self, id: ListenerId) {
        let state = &self.core().state;
        let removed = {
            let mut listeners = state.listeners.borrow_mut();
            listeners.iter().position(|(entry, ..)| *entry == id.0).map(|at| listeners.remove(at))
        };
        let protocol = {
            let mut listeners = state.protocol_listeners.borrow_mut();
            listeners.iter().position(|(entry, _)| *entry == id.0).map(|at| listeners.remove(at))
        };
        if let Some((_, Some(kind), _)) = &removed {
            if !state.listening(*kind) {
                self.subscribe(*kind, false);
            }
        }
        drop((removed, protocol));
    }

    /// The next event of `kind` for which `predicate` is true. Its listener
    /// goes once the event has come; [`Page::remove_listener`] with the id
    /// it returns stops waiting (`done` is then never called).
    pub fn wait_for_event<P, F>(&self, kind: EventKind, mut predicate: P, done: F) -> ListenerId
    where
        P: FnMut(&PageEvent) -> bool + 'static,
        F: FnOnce(PageEvent) + 'static,
    {
        let done = Cell::new(Some(done));
        let own = Rc::new(Cell::new(None));
        let (page, id_slot) = (self.clone(), own.clone());
        let id = self.on(kind, move |event| {
            if !predicate(event) {
                return;
            }
            if let Some(id) = id_slot.take() {
                page.remove_listener(id);
            }
            if let Some(done) = done.take() {
                done(event.clone());
            }
        });
        own.set(Some(id));
        id
    }

    /// One of the page's commands, as the protocol has them.
    pub fn command<F: FnOnce(Result<Value, Error>) + 'static>(&self, method: &str, params: Value, done: F) {
        self.core().run(method, params, done);
    }

    /// Calls `handler(route, request)` for the requests whose URL matches
    /// `url`, a glob as Playwright's (`"**/data.json"`); the last route
    /// added that matches answers. A request no handler takes continues.
    pub fn route<H: FnMut(Route, Request) + 'static, F: FnOnce(Result<(), Error>) + 'static>(&self, url: &str, handler: H, done: F) {
        let glob = Glob::new(url);
        self.add_route(Value::object([("glob", url.into())]), Rc::new(move |text| glob.matches(text)), handler, done);
    }

    /// [`Page::route`] for a regular expression (JavaScript's `source` and
    /// `flags`) that the engine matches; `matches` tells which of the page's
    /// routes a request is for (a `regex` crate's `is_match`, say).
    pub fn route_regex<M, H, F>(&self, source: &str, flags: &str, matches: M, handler: H, done: F)
    where
        M: Fn(&str) -> bool + 'static,
        H: FnMut(Route, Request) + 'static,
        F: FnOnce(Result<(), Error>) + 'static,
    {
        let pattern = Value::object([("regexSource", source.into()), ("regexFlags", (if flags.contains('i') { "i" } else { "" }).into())]);
        self.add_route(pattern, Rc::new(matches), handler, done);
    }

    fn add_route<H: FnMut(Route, Request) + 'static>(&self, pattern: Value, matches: Rc<dyn Fn(&str) -> bool>, handler: H, done: impl FnOnce(Result<(), Error>) + 'static) {
        self.core().state.routes.borrow_mut().push(RouteEntry { pattern, matches, handler: Rc::new(RefCell::new(Box::new(handler))) });
        self.set_routes(done);
    }

    /// Removes the routes of `url` (a glob or a regular expression's source).
    pub fn unroute<F: FnOnce(Result<(), Error>) + 'static>(&self, url: &str, done: F) {
        let removed = {
            let mut routes = self.core().state.routes.borrow_mut();
            let (gone, kept): (Vec<_>, Vec<_>) = std::mem::take(&mut *routes)
                .into_iter()
                .partition(|route| route.pattern["glob"].as_str() == Some(url) || route.pattern["regexSource"].as_str() == Some(url));
            *routes = kept;
            gone
        };
        drop(removed);
        self.set_routes(done);
    }

    fn set_routes(&self, done: impl FnOnce(Result<(), Error>) + 'static) {
        let patterns = Value::Array(self.core().state.routes.borrow().iter().map(|route| route.pattern.clone()).collect());
        self.core().run("Page.setNetworkInterceptionPatterns", Value::object([("patterns", patterns)]), unit(done));
    }

    /// Makes `window.<name>(...args)` in the page's frames call `function`,
    /// which answers the page's promise.
    pub fn expose_function<G, F>(&self, name: &str, mut function: G, done: F)
    where
        G: FnMut(&[Value]) -> Result<Value, String> + 'static,
        F: FnOnce(Result<(), Error>) + 'static,
    {
        self.expose_binding(
            name,
            move |call: BindingCall| match function(call.args()) {
                Ok(value) => call.resolve(value),
                Err(message) => call.reject(&message),
            },
            done,
        );
    }

    /// [`Page::expose_function`], with the call's frame, answered now or later.
    pub fn expose_binding<G: FnMut(BindingCall) + 'static, F: FnOnce(Result<(), Error>) + 'static>(&self, name: &str, function: G, done: F) {
        self.core().state.bindings.borrow_mut().insert(name.to_string(), Rc::new(RefCell::new(Box::new(function))));
        self.core().run("Page.exposeBinding", Value::object([("name", name.into())]), unit(done));
    }

    /// The page's frames, the main one first.
    pub fn frames<F: FnOnce(Result<Vec<Frame>, Error>) + 'static>(&self, done: F) {
        let core = self.core().clone();
        self.core().run(
            "Page.frames",
            Value::Null,
            then(done, move |answer| {
                Ok(answer["frames"]
                    .as_array()
                    .unwrap_or_default()
                    .iter()
                    .map(|frame| Frame {
                        core: core.clone(),
                        id: frame.get("parentFrame").filter(|parent| !parent.is_null()).and(frame.text("frame")),
                        name: frame.text("name").unwrap_or_default(),
                        url: frame.text("url").unwrap_or_default(),
                    })
                    .collect())
            }),
        );
    }

    /// The frame named `name`, or whose URL has `url_part`.
    pub fn find_frame<F: FnOnce(Result<Frame, Error>) + 'static>(&self, name: Option<&str>, url_part: Option<&str>, done: F) {
        let (name, url_part) = (name.map(str::to_string), url_part.map(str::to_string));
        self.frames(move |frames| {
            done(frames.and_then(|frames| {
                frames
                    .into_iter()
                    .find(|frame| name.as_deref().is_some_and(|name| frame.name == name) || url_part.as_deref().is_some_and(|part| frame.url.contains(part)))
                    .ok_or_else(|| Error::new(format!("no frame {}", name.or(url_part).unwrap_or_default())))
            }))
        });
    }

    fn navigation<F: FnOnce(Result<Option<Response>, Error>) + 'static>(&self, method: &str, options: Value, done: F) {
        let core = self.core().clone();
        self.core().run(method, options, then(done, move |answer| Ok(response_of(&core, member(answer, "response")))));
    }

    /// Reloads the page; `options`: `waitUntil`, `timeout`.
    pub fn reload<F: FnOnce(Result<Option<Response>, Error>) + 'static>(&self, options: Value, done: F) {
        self.navigation("Page.reload", options, done);
    }

    pub fn go_back<F: FnOnce(Result<Option<Response>, Error>) + 'static>(&self, options: Value, done: F) {
        self.navigation("Page.goBack", options, done);
    }

    pub fn go_forward<F: FnOnce(Result<Option<Response>, Error>) + 'static>(&self, options: Value, done: F) {
        self.navigation("Page.goForward", options, done);
    }

    /// A picture of the page, PNG unless `options` say (`type`, `quality`,
    /// `fullPage`, `clip`, `omitBackground`, `animations`, `caret`, `scale`,
    /// `mask`, `maskColor`, `style`, `timeout`); with `path` it is written
    /// there too.
    pub fn screenshot<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, options: Value, done: F) {
        let (path, options) = take_path(options);
        self.core().run("Page.screenshot", options, then(done, move |answer| write_file(path.as_deref(), binary(&answer, "binary")?)));
    }

    /// The screenshot's pixels, unencoded ([`Pixels`], RGBA), with
    /// [`Page::screenshot`]'s options but type, quality and path.
    pub fn screenshot_pixels<F: FnOnce(Result<Pixels<'_>, Error>) + 'static>(&self, options: Value, done: F) {
        screenshot_pixels(self.view(), "Page.screenshot", &object_or_empty(options).to_string(), done);
    }

    /// The page as PDF, with `Page.pdf`'s options; with `path` written there too.
    pub fn pdf<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, options: Value, done: F) {
        let (path, options) = take_path(options);
        self.core().run("Page.pdf", options, then(done, move |answer| write_file(path.as_deref(), binary(&answer, "pdf")?)));
    }

    pub fn set_viewport_size<F: FnOnce(Result<(), Error>) + 'static>(&self, width: u32, height: u32, done: F) {
        let size = Value::object([("width", width.into()), ("height", height.into())]);
        self.core().run("Page.setViewportSize", Value::object([("viewportSize", size)]), unit(done));
    }

    /// `options`: `media`, `colorScheme`, `reducedMotion`, `forcedColors`, `contrast`.
    pub fn emulate_media<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.core().run("Page.emulateMedia", options, unit(done));
    }

    /// `source` in every document of the page from now on, before the page's
    /// own script; `done` gets a [`Disposable`] that takes it away again.
    pub fn add_init_script<F: FnOnce(Result<Disposable, Error>) + 'static>(&self, source: &str, done: F) {
        let view = self.core().view.clone();
        self.core().run("Page.addInitScript", Value::object([("source", source.into())]), then(done, move |answer| {
            match answer["registration"].as_str() {
                Some(registration) => Ok(Disposable { view, registration: registration.to_string() }),
                None => Err(Error::new("the engine gave no registration")),
            }
        }));
    }

    pub fn set_extra_http_headers<F: FnOnce(Result<(), Error>) + 'static>(&self, headers: &[(&str, &str)], done: F) {
        self.core().run("Page.setExtraHTTPHeaders", Value::object([("headers", header_list(headers))]), unit(done));
    }

    pub fn bring_to_front<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.core().run("Page.bringToFront", Value::Null, unit(done));
    }

    pub fn close<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.core().run("Page.close", Value::Null, unit(done));
    }

    /// The page's dedicated workers, as their events told them.
    pub fn workers(&self) -> Vec<Worker> {
        self.core().state.workers.borrow().clone()
    }

    /// Records the page to `path` (a temporary .webm when `None`): WebM,
    /// VP8; answers the path.
    pub fn start_video<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, path: Option<&str>, size: Option<(u32, u32)>, done: F) {
        let size = size.map(|(width, height)| Value::object([("width", width.into()), ("height", height.into())]));
        self.core().run("Page.startVideo", Value::object([("path", path.into()), ("size", size.into())]), then(done, |answer| Ok(answer.text("path"))));
    }

    pub fn stop_video<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, done: F) {
        self.core().run("Page.stopVideo", Value::Null, then(done, |answer| Ok(answer.text("path"))));
    }

    /// `{"latitude", "longitude", "accuracy"}`, or `Value::Null` for none.
    pub fn set_geolocation<F: FnOnce(Result<(), Error>) + 'static>(&self, geolocation: Value, done: F) {
        self.core().run("BrowserContext.setGeolocation", Value::object([("geolocation", geolocation)]), unit(done));
    }

    /// `{"username", "password", "origin"}`, or `Value::Null` for none.
    pub fn set_http_credentials<F: FnOnce(Result<(), Error>) + 'static>(&self, credentials: Value, done: F) {
        self.core().run("BrowserContext.setHTTPCredentials", Value::object([("httpCredentials", credentials)]), unit(done));
    }

    /// The cookies of the page's profile, of `urls` (none: all).
    pub fn cookies<F: FnOnce(Result<Vec<Value>, Error>) + 'static>(&self, urls: &[&str], done: F) {
        self.core().run("BrowserContext.cookies", cookie_urls(urls), then(done, cookie_list));
    }

    /// Playwright's cookies: `[{"name", "value", "url" or "domain" and "path", ...}]`.
    pub fn add_cookies<F: FnOnce(Result<(), Error>) + 'static>(&self, cookies: Value, done: F) {
        self.core().run("BrowserContext.addCookies", Value::object([("cookies", cookies)]), unit(done));
    }

    pub fn clear_cookies<F: FnOnce(Result<(), Error>) + 'static>(&self, name: Option<&str>, domain: Option<&str>, path: Option<&str>, done: F) {
        self.core().run("BrowserContext.clearCookies", Value::object([("name", name.into()), ("domain", domain.into()), ("path", path.into())]), unit(done));
    }

    /// The profile's cookies and storage, as Playwright's storageState.
    pub fn storage_state<F: FnOnce(Result<Value, Error>) + 'static>(&self, done: F) {
        self.core().run("BrowserContext.storageState", Value::Null, done);
    }

    pub fn set_storage_state<F: FnOnce(Result<(), Error>) + 'static>(&self, state: Value, done: F) {
        self.core().run("BrowserContext.setStorageState", Value::object([("storageState", state)]), unit(done));
    }

    pub fn grant_permissions<F: FnOnce(Result<(), Error>) + 'static>(&self, permissions: &[&str], origin: Option<&str>, done: F) {
        let list = Value::Array(permissions.iter().map(|permission| (*permission).into()).collect());
        self.core().run("BrowserContext.grantPermissions", Value::object([("permissions", list), ("origin", origin.into())]), unit(done));
    }

    pub fn clear_permissions<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.core().run("BrowserContext.clearPermissions", Value::Null, unit(done));
    }

    pub fn set_offline<F: FnOnce(Result<(), Error>) + 'static>(&self, offline: bool, done: F) {
        self.core().run("BrowserContext.setOffline", Value::object([("offline", offline.into())]), unit(done));
    }

    pub fn keyboard(&self) -> Keyboard {
        Keyboard { core: self.core().clone() }
    }

    pub fn mouse(&self) -> Mouse {
        Mouse { core: self.core().clone() }
    }

    pub fn touchscreen(&self) -> Touchscreen {
        Touchscreen { core: self.core().clone() }
    }

    pub fn clock(&self) -> Clock {
        Clock { core: self.core().clone() }
    }

    pub fn tracing(&self) -> Tracing {
        Tracing { core: self.core().clone() }
    }

    /// Requests through the engine's network, with the page's cookies.
    pub fn request(&self) -> APIRequestContext {
        APIRequestContext { core: self.core().clone() }
    }
}

fn object_or_empty(options: Value) -> Value {
    if options.is_null() {
        Value::Object(Vec::new())
    } else {
        options
    }
}

/// `options` without its `path`, and the path.
fn take_path(options: Value) -> (Option<String>, Value) {
    match object_or_empty(options) {
        Value::Object(members) => {
            let path = members.iter().find(|(name, _)| name == "path").and_then(|(_, value)| value.as_str().map(str::to_string));
            (path, Value::Object(members.into_iter().filter(|(name, _)| name != "path").collect()))
        }
        other => (None, other),
    }
}

fn cookie_urls(urls: &[&str]) -> Value {
    let urls = (!urls.is_empty()).then(|| Value::Array(urls.iter().map(|url| (*url).into()).collect()));
    Value::object([("urls", urls.into())])
}

fn cookie_list(answer: Value) -> Result<Vec<Value>, Error> {
    Ok(match member(answer, "cookies") {
        Value::Array(cookies) => cookies,
        _ => Vec::new(),
    })
}

fn response_of(core: &Core, value: Value) -> Option<Response> {
    value.as_object().is_some().then(|| Response { core: Some(core.clone()), value: Rc::new(value) })
}

/// The automation event `event` with `json`, to the page's listeners.
fn dispatch(core: &Core, event: &str, json: &str) {
    let value = Value::parse(json).unwrap_or_default();
    let protocol: Vec<ProtocolListener> = core.state.protocol_listeners.borrow().iter().map(|(_, listener)| listener.clone()).collect();
    for listener in protocol {
        if let Ok(mut listener) = listener.try_borrow_mut() {
            listener(event, &value);
        }
    }
    let event = match event {
        "BrowserContext.route" => {
            route_event(core, value);
            return;
        }
        "BrowserContext.bindingCall" => {
            binding_event(core, value);
            return;
        }
        "Worker.close" => {
            let id = value.text("worker");
            let worker = {
                let mut workers = core.state.workers.borrow_mut();
                workers.iter().position(|worker| Some(&worker.id) == id.as_ref()).map(|at| workers.remove(at))
            };
            if let Some(worker) = worker {
                let listeners = std::mem::take(&mut *worker.close_listeners.borrow_mut());
                for mut listener in listeners {
                    listener(&worker);
                }
            }
            return;
        }
        "BrowserContext.console" => PageEvent::Console(ConsoleMessage { value: Rc::new(value) }),
        "BrowserContext.dialog" => PageEvent::Dialog(Dialog { core: core.clone(), value: Rc::new(value) }),
        "Page.fileChooser" => PageEvent::FileChooser(FileChooser { core: core.clone(), value: Rc::new(value) }),
        "Page.download" => PageEvent::Download(Download { core: core.clone(), value: Rc::new(value) }),
        "Page.request" => {
            let request = Request { value: Rc::new(value) };
            let mut requests = core.state.requests.borrow_mut();
            if requests.len() >= 1000 {
                requests.pop_front();
            }
            requests.push_back(request.clone());
            PageEvent::Request(request)
        }
        "Page.response" => PageEvent::Response(Response { core: Some(core.clone()), value: Rc::new(value) }),
        "Page.requestFinished" => PageEvent::RequestFinished(ended_request(core, value, false)),
        "Page.requestFailed" => PageEvent::RequestFailed(ended_request(core, value, true)),
        "Frame.navigated" => PageEvent::FrameNavigated(value),
        "Page.frameAttached" => PageEvent::FrameAttached(value),
        "Page.frameDetached" => PageEvent::FrameDetached(value),
        "Frame.loadstate" => PageEvent::LoadState(value),
        "Page.worker" => {
            let worker = Worker { core: core.clone(), id: value.text("worker").unwrap_or_default(), url: value.text("url").unwrap_or_default(), close_listeners: Rc::default() };
            core.state.workers.borrow_mut().push(worker.clone());
            PageEvent::Worker(worker)
        }
        "Page.crash" => PageEvent::Crash,
        "Page.close" => PageEvent::Close,
        _ => return,
    };
    let kind = event.kind();
    let listeners: Vec<Listener> =
        core.state.listeners.borrow().iter().filter(|(_, listening, _)| *listening == Some(kind)).map(|(_, _, listener)| listener.clone()).collect();
    if listeners.is_empty() {
        // Held for this page while its last listener went (the engine had
        // not heard yet): let the page go on, as Playwright does without a
        // listener. One held for the application or a CDP client is theirs.
        let unsubscribing = |at: usize| core.state.unsubscribing[at].get() > 0;
        match &event {
            PageEvent::Dialog(dialog) if unsubscribing(0) && dialog.value["auto"].as_bool() != Some(true) => {
                if dialog.kind() == "beforeunload" {
                    dialog.accept(None, |_| {});
                } else {
                    dialog.dismiss(|_| {});
                }
            }
            PageEvent::FileChooser(chooser) if unsubscribing(1) => chooser.set_files(&[], |_| {}),
            _ => {}
        }
        return;
    }
    for listener in listeners {
        // A listener that runs again from inside itself is not called.
        if let Ok(mut listener) = listener.try_borrow_mut() {
            listener(&event);
        }
    }
}

/// The request a request's end names, with what its end says.
fn ended_request(core: &Core, value: Value, failed: bool) -> Request {
    let id = value.text("request");
    let started = core.state.requests.borrow().iter().rev().find(|request| id.is_some() && request.value.text("request") == id).cloned();
    let mut whole = started.map(|request| (*request.value).clone()).unwrap_or_default();
    if let Value::Object(members) = value {
        for (name, value) in members {
            whole.set(&name, value);
        }
    }
    if failed {
        let requests = &mut *core.state.requests.borrow_mut();
        requests.retain(|request| request.value.text("request") != id);
    }
    Request { value: Rc::new(whole) }
}

fn route_event(core: &Core, value: Value) {
    let id = value.text("route").unwrap_or_default();
    let request = Request { value: Rc::new(member(value, "request")) };
    let route = Route { core: core.clone(), id, request: request.clone() };
    let handler = core.state.routes.borrow().iter().rev().find(|route| (route.matches)(request.url())).map(|route| route.handler.clone());
    // A request no route takes, or one its handler's own run asks for, goes on.
    let Some(handler) = handler else {
        route.continue_(Value::Null, |_| {});
        return;
    };
    let Ok(mut function) = handler.try_borrow_mut() else {
        route.continue_(Value::Null, |_| {});
        return;
    };
    function(route, request);
}

fn binding_event(core: &Core, value: Value) {
    let name = value.text("name").unwrap_or_default();
    let function = core.state.bindings.borrow().get(&name).cloned();
    let call = BindingCall {
        core: core.clone(),
        id: value.text("binding").unwrap_or_default(),
        frame: value.text("frame"),
        args: match value.get("args") {
            Some(Value::Array(args)) => args.clone(),
            _ => Vec::new(),
        },
        answered: Cell::new(false),
    };
    match function {
        Some(function) => match function.try_borrow_mut() {
            Ok(mut function) => function(call),
            Err(_) => call.reject("the application's function is already running"),
        },
        None => call.reject(&format!("no function {name}")),
    }
}

/// A frame of a page: its id, `None` for the main frame.
#[derive(Clone)]
pub struct Frame {
    core: Core,
    id: Option<String>,
    name: String,
    url: String,
}

impl Frame {
    /// The frame's id; `None` for the main frame.
    pub fn id(&self) -> Option<&str> {
        self.id.as_deref()
    }

    /// Its name and URL as [`Page::frames`] found them; "" for a frame made otherwise.
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn url(&self) -> &str {
        &self.url
    }

    /// The page the frame is of.
    pub fn page(&self) -> Page {
        Page::from_core(self.core.clone())
    }

    /// `params` with the frame.
    fn params(&self, params: Value) -> Value {
        let mut params = object_or_empty(params);
        if let Some(id) = &self.id {
            params.set("frame", id.as_str().into());
        }
        params
    }

    /// One of the frame's commands ("Frame.*"), with the frame added to `params`.
    pub fn command<F: FnOnce(Result<Value, Error>) + 'static>(&self, method: &str, params: Value, done: F) {
        self.core.run(method, self.params(params), done);
    }

    /// Navigates the frame; the main resource's response, if there was one.
    pub fn goto<F: FnOnce(Result<Option<Response>, Error>) + 'static>(&self, url: &str, done: F) {
        self.goto_with(url, Value::Null, done);
    }

    /// [`Frame::goto`] with `options`: `waitUntil`, `timeout`, `referer`.
    pub fn goto_with<F: FnOnce(Result<Option<Response>, Error>) + 'static>(&self, url: &str, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("url", url.into());
        let core = self.core.clone();
        self.command("Frame.goto", params, then(done, move |answer| Ok(response_of(&core, member(answer, "response")))));
    }

    fn evaluation(&self, expression: &str, arg: Value) -> Value {
        Value::object([
            ("expression", expression.into()),
            ("isFunction", looks_like_function(expression).then_some(true).into()),
            ("arg", arg),
        ])
    }

    /// The value of `expression` (or of the function it is, called with
    /// `arg`; a handle's [`JSHandle::as_arg`] stands for it).
    pub fn evaluate<F: FnOnce(Result<Value, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        self.command("Frame.evaluateExpression", self.evaluation(expression, arg), value(done));
    }

    pub fn evaluate_handle<F: FnOnce(Result<Option<JSHandle>, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        let frame = self.clone();
        self.command("Frame.evaluateExpressionHandle", self.evaluation(expression, arg), then(done, move |answer| Ok(handle_of(&frame, &answer["handle"]))));
    }

    pub fn content<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.content", Value::Null, text_value(done));
    }

    /// `options`: `waitUntil`, `timeout`.
    pub fn set_content<F: FnOnce(Result<(), Error>) + 'static>(&self, html: &str, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("html", html.into());
        self.command("Frame.setContent", params, unit(done));
    }

    pub fn title<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.title", Value::Null, text_value(done));
    }

    /// `options`: `state`, `timeout`, `strict`.
    pub fn wait_for_selector<F: FnOnce(Result<Option<ElementHandle>, Error>) + 'static>(&self, selector: &str, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("selector", selector.into());
        let frame = self.clone();
        self.command("Frame.waitForSelector", params, then(done, move |answer| Ok(element_of(&frame, &answer["element"]))));
    }

    /// `options`: `pollingInterval`, `timeout`.
    pub fn wait_for_function<F: FnOnce(Result<Option<JSHandle>, Error>) + 'static>(&self, expression: &str, arg: Value, options: Value, done: F) {
        let mut params = object_or_empty(options);
        if let Value::Object(members) = self.evaluation(expression, arg) {
            for (name, value) in members {
                params.set(&name, value);
            }
        }
        let frame = self.clone();
        self.command("Frame.waitForFunction", params, then(done, move |answer| Ok(handle_of(&frame, &answer["handle"]))));
    }

    /// `state`: `"load"` (`None`), `"domcontentloaded"`, `"networkidle"`.
    pub fn wait_for_load_state<F: FnOnce(Result<(), Error>) + 'static>(&self, state: Option<&str>, timeout: Option<f64>, done: F) {
        self.command("Frame.waitForLoadState", Value::object([("state", state.into()), ("timeout", milliseconds(timeout))]), unit(done));
    }

    pub fn wait_for_timeout<F: FnOnce(Result<(), Error>) + 'static>(&self, timeout: f64, done: F) {
        self.command("Frame.waitForTimeout", Value::object([("waitTimeout", timeout.into())]), unit(done));
    }

    pub fn query_selector<F: FnOnce(Result<Option<ElementHandle>, Error>) + 'static>(&self, selector: &str, done: F) {
        let frame = self.clone();
        self.command("Frame.querySelector", Value::object([("selector", selector.into())]), then(done, move |answer| Ok(element_of(&frame, &answer["element"]))));
    }

    pub fn query_selector_all<F: FnOnce(Result<Vec<ElementHandle>, Error>) + 'static>(&self, selector: &str, done: F) {
        let frame = self.clone();
        self.command(
            "Frame.querySelectorAll",
            Value::object([("selector", selector.into())]),
            then(done, move |answer| Ok(answer["elements"].as_array().unwrap_or_default().iter().filter_map(|element| element_of(&frame, element)).collect())),
        );
    }

    pub fn eval_on_selector<F: FnOnce(Result<Value, Error>) + 'static>(&self, selector: &str, expression: &str, arg: Value, done: F) {
        let mut params = self.evaluation(expression, arg);
        params.set("selector", selector.into());
        params.set("strict", true.into());
        self.command("Frame.evalOnSelector", params, value(done));
    }

    pub fn eval_on_selector_all<F: FnOnce(Result<Value, Error>) + 'static>(&self, selector: &str, expression: &str, arg: Value, done: F) {
        let mut params = self.evaluation(expression, arg);
        params.set("selector", selector.into());
        self.command("Frame.evalOnSelectorAll", params, value(done));
    }

    /// A `<script>` of `url` or with `content`; `kind` is its type ("module").
    pub fn add_script_tag<F: FnOnce(Result<Option<ElementHandle>, Error>) + 'static>(&self, url: Option<&str>, content: Option<&str>, kind: Option<&str>, done: F) {
        let frame = self.clone();
        let params = Value::object([("url", url.into()), ("content", content.into()), ("type", kind.into())]);
        self.command("Frame.addScriptTag", params, then(done, move |answer| Ok(element_of(&frame, &answer["element"]))));
    }

    pub fn add_style_tag<F: FnOnce(Result<Option<ElementHandle>, Error>) + 'static>(&self, url: Option<&str>, content: Option<&str>, done: F) {
        let frame = self.clone();
        self.command("Frame.addStyleTag", Value::object([("url", url.into()), ("content", content.into())]), then(done, move |answer| Ok(element_of(&frame, &answer["element"]))));
    }

    /// Drags `source` (a selector) onto `target`; `options` as Playwright's.
    pub fn drag_and_drop<F: FnOnce(Result<(), Error>) + 'static>(&self, source: &str, target: &str, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("source", source.into());
        params.set("target", target.into());
        params.set("strict", true.into());
        self.command("Frame.dragAndDrop", params, unit(done));
    }

    pub fn locator(&self, selector: &str) -> Locator {
        Locator { frame: self.clone(), selector: selector.to_string(), options: Value::Null }
    }

    /// The frame of the `<iframe>` of `selector`.
    pub fn frame_locator(&self, selector: &str) -> FrameLocator {
        FrameLocator { frame: self.clone(), selector: selector.to_string() }
    }
}

// ---- Locators --------------------------------------------------------------

/// Playwright's Locator: a selector in a frame, resolved at each use.
#[derive(Clone)]
pub struct Locator {
    frame: Frame,
    selector: String,
    /// What its commands are given besides their own params.
    options: Value,
}

impl Locator {
    pub fn selector(&self) -> &str {
        &self.selector
    }

    pub fn frame(&self) -> &Frame {
        &self.frame
    }

    fn with_selector(&self, selector: String) -> Locator {
        Locator { frame: self.frame.clone(), selector, options: self.options.clone() }
    }

    /// The same locator, whose commands are also given `options` (the
    /// protocol's: `timeout`, `force`, `noWaitAfter`, `position`,
    /// `modifiers`, `button`, `clickCount`, `delay`, `trial`).
    pub fn with_options(&self, options: Value) -> Locator {
        Locator { frame: self.frame.clone(), selector: self.selector.clone(), options }
    }

    /// [`Locator::with_options`] with only a timeout, in milliseconds.
    pub fn with_timeout(&self, timeout: f64) -> Locator {
        let mut options = self.options.clone();
        options.set("timeout", timeout.into());
        self.with_options(options)
    }

    pub fn locator(&self, selector: &str) -> Locator {
        self.with_selector(format!("{} >> {selector}", self.selector))
    }

    /// `other` inside this one.
    pub fn locator_of(&self, other: &Locator) -> Locator {
        self.with_selector(format!("{} >> internal:chain={}", self.selector, json::quote(&other.selector)))
    }

    pub fn nth(&self, index: i32) -> Locator {
        self.locator(&format!("nth={index}"))
    }

    pub fn first(&self) -> Locator {
        self.nth(0)
    }

    pub fn last(&self) -> Locator {
        self.nth(-1)
    }

    pub fn filter(&self, filter: &Filter) -> Locator {
        let mut selector = self.selector.clone();
        if let Some(text) = &filter.has_text {
            selector.push_str(&format!(" >> internal:has-text={}", text.selector(false)));
        }
        if let Some(text) = &filter.has_not_text {
            selector.push_str(&format!(" >> internal:has-not-text={}", text.selector(false)));
        }
        if let Some(has) = &filter.has {
            selector.push_str(&format!(" >> internal:has={}", json::quote(&has.selector)));
        }
        if let Some(has_not) = &filter.has_not {
            selector.push_str(&format!(" >> internal:has-not={}", json::quote(&has_not.selector)));
        }
        if let Some(visible) = filter.visible {
            selector.push_str(&format!(" >> visible={visible}"));
        }
        self.with_selector(selector)
    }

    pub fn and(&self, other: &Locator) -> Locator {
        self.with_selector(format!("{} >> internal:and={}", self.selector, json::quote(&other.selector)))
    }

    pub fn or(&self, other: &Locator) -> Locator {
        self.with_selector(format!("{} >> internal:or={}", self.selector, json::quote(&other.selector)))
    }

    /// The frame of this `<iframe>` selector, as Playwright's `contentFrame`.
    pub fn content_frame(&self) -> FrameLocator {
        FrameLocator { frame: self.frame.clone(), selector: self.selector.clone() }
    }

    pub fn frame_locator(&self, selector: &str) -> FrameLocator {
        FrameLocator { frame: self.frame.clone(), selector: format!("{} >> {selector}", self.selector) }
    }

    /// Runs `method` with the selector, strictly, the locator's options and `params`.
    pub fn command<F: FnOnce(Result<Value, Error>) + 'static>(&self, method: &str, params: Value, done: F) {
        self.command_strict(method, params, true, done);
    }

    fn command_strict(&self, method: &str, params: Value, strict: bool, done: impl FnOnce(Answer) + 'static) {
        let mut all = object_or_empty(self.options.clone());
        if let Value::Object(members) = params {
            for (name, value) in members {
                all.set(&name, value);
            }
        }
        all.set("selector", self.selector.as_str().into());
        all.set("strict", strict.into());
        self.frame.command(method, all, done);
    }

    fn act(&self, method: &str, params: Value, done: impl FnOnce(Result<(), Error>) + 'static) {
        self.command(method, params, unit(done));
    }

    pub fn click<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.click", Value::Null, done);
    }

    pub fn dblclick<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.dblclick", Value::Null, done);
    }

    pub fn hover<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.hover", Value::Null, done);
    }

    pub fn tap<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.tap", Value::Null, done);
    }

    pub fn check<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.check", Value::Null, done);
    }

    pub fn uncheck<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.uncheck", Value::Null, done);
    }

    pub fn set_checked<F: FnOnce(Result<(), Error>) + 'static>(&self, checked: bool, done: F) {
        self.act(if checked { "Frame.check" } else { "Frame.uncheck" }, Value::Null, done);
    }

    pub fn fill<F: FnOnce(Result<(), Error>) + 'static>(&self, value: &str, done: F) {
        self.act("Frame.fill", Value::object([("value", value.into())]), done);
    }

    pub fn clear<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.fill("", done);
    }

    pub fn press<F: FnOnce(Result<(), Error>) + 'static>(&self, key: &str, done: F) {
        self.act("Frame.press", Value::object([("key", key.into())]), done);
    }

    pub fn press_sequentially<F: FnOnce(Result<(), Error>) + 'static>(&self, text: &str, done: F) {
        self.act("Frame.type", Value::object([("text", text.into())]), done);
    }

    pub fn focus<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.focus", Value::Null, done);
    }

    pub fn blur<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("Frame.blur", Value::Null, done);
    }

    /// Selects the options; the values selected.
    pub fn select_option<F: FnOnce(Result<Vec<String>, Error>) + 'static>(&self, options: &[SelectOption], done: F) {
        let chosen = Value::Array(
            options
                .iter()
                .map(|option| match option {
                    SelectOption::ValueOrLabel(text) => Value::object([("valueOrLabel", text.as_str().into())]),
                    SelectOption::Value(text) => Value::object([("value", text.as_str().into())]),
                    SelectOption::Label(text) => Value::object([("label", text.as_str().into())]),
                    SelectOption::Index(index) => Value::object([("index", (*index).into())]),
                })
                .collect(),
        );
        self.command("Frame.selectOption", Value::object([("options", chosen)]), then(done, |answer| Ok(strings(&answer["values"]))));
    }

    /// Files by path for an `<input type=file>`.
    pub fn set_input_files<F: FnOnce(Result<(), Error>) + 'static>(&self, paths: &[&str], done: F) {
        let list = Value::Array(paths.iter().map(|path| absolute(path).into()).collect());
        self.act("Frame.setInputFiles", Value::object([("localPaths", list)]), done);
    }

    /// Files of the application's: (name, MIME type, bytes).
    pub fn set_input_file_payloads<F: FnOnce(Result<(), Error>) + 'static>(&self, files: &[(&str, &str, &[u8])], done: F) {
        let payloads = Value::Array(
            files
                .iter()
                .map(|(name, mime, bytes)| Value::object([("name", (*name).into()), ("mimeType", (*mime).into()), ("buffer", base64_encode(bytes).into())]))
                .collect(),
        );
        self.act("Frame.setInputFiles", Value::object([("payloads", payloads)]), done);
    }

    pub fn dispatch_event<F: FnOnce(Result<(), Error>) + 'static>(&self, kind: &str, event_init: Value, done: F) {
        self.act("Frame.dispatchEvent", Value::object([("type", kind.into()), ("eventInit", event_init)]), done);
    }

    pub fn drag_to<F: FnOnce(Result<(), Error>) + 'static>(&self, target: &Locator, done: F) {
        let mut params = object_or_empty(self.options.clone());
        params.set("source", self.selector.as_str().into());
        params.set("target", target.selector.as_str().into());
        params.set("strict", true.into());
        self.frame.command("Frame.dragAndDrop", params, unit(done));
    }

    pub fn text_content<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, done: F) {
        self.command("Frame.textContent", Value::Null, string_value(done));
    }

    pub fn inner_text<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.innerText", Value::Null, text_value(done));
    }

    pub fn inner_html<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.innerHTML", Value::Null, text_value(done));
    }

    pub fn get_attribute<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, name: &str, done: F) {
        self.command("Frame.getAttribute", Value::object([("name", name.into())]), string_value(done));
    }

    pub fn input_value<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.inputValue", Value::Null, text_value(done));
    }

    pub fn is_visible<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isVisible", Value::Null, bool_value(done));
    }

    pub fn is_hidden<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isHidden", Value::Null, bool_value(done));
    }

    pub fn is_enabled<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isEnabled", Value::Null, bool_value(done));
    }

    pub fn is_disabled<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isDisabled", Value::Null, bool_value(done));
    }

    pub fn is_checked<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isChecked", Value::Null, bool_value(done));
    }

    pub fn is_editable<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isEditable", Value::Null, bool_value(done));
    }

    pub fn count<F: FnOnce(Result<usize, Error>) + 'static>(&self, done: F) {
        self.command_strict("Frame.queryCount", Value::Null, false, then(done, |answer| Ok(answer["value"].as_f64().unwrap_or(0.0) as usize)));
    }

    /// A locator for each element it matches now.
    pub fn all<F: FnOnce(Result<Vec<Locator>, Error>) + 'static>(&self, done: F) {
        let locator = self.clone();
        self.count(move |count| done(count.map(|count| (0..count).map(|index| locator.nth(index as i32)).collect())));
    }

    pub fn all_text_contents<F: FnOnce(Result<Vec<String>, Error>) + 'static>(&self, done: F) {
        self.evaluate_all("es => es.map(e => e.textContent || '')", Value::Null, move |result| done(result.map(|value| strings(&value))));
    }

    pub fn all_inner_texts<F: FnOnce(Result<Vec<String>, Error>) + 'static>(&self, done: F) {
        self.evaluate_all("es => es.map(e => e.innerText)", Value::Null, move |result| done(result.map(|value| strings(&value))));
    }

    /// The function `expression` called with the element and `arg`.
    pub fn evaluate<F: FnOnce(Result<Value, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        let expression = expression.to_string();
        self.with_element(move |element, finish| element.evaluate(&expression, arg, finish), done);
    }

    /// The function `expression` called with every element it matches and `arg`.
    pub fn evaluate_all<F: FnOnce(Result<Value, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        let params = Value::object([("selector", self.selector.as_str().into()), ("expression", expression.into()), ("isFunction", true.into()), ("arg", arg)]);
        self.frame.command("Frame.evalOnSelectorAll", params, value(done));
    }

    /// Runs `with` on the element, then lets its handle go.
    fn with_element<T: 'static>(&self, with: impl FnOnce(&ElementHandle, Box<dyn FnOnce(Result<T, Error>)>) + 'static, done: impl FnOnce(Result<T, Error>) + 'static) {
        self.element_handle(move |element| match element {
            Err(error) => done(Err(error)),
            Ok(element) => {
                let disposing = element.clone();
                with(
                    &element,
                    Box::new(move |result| {
                        disposing.dispose(|_| {});
                        done(result);
                    }),
                );
            }
        });
    }

    /// A handle to the element, once it is attached.
    pub fn element_handle<F: FnOnce(Result<ElementHandle, Error>) + 'static>(&self, done: F) {
        let frame = self.frame.clone();
        self.command(
            "Frame.waitForSelector",
            Value::object([("state", "attached".into())]),
            then(done, move |answer| element_of(&frame, &answer["element"]).ok_or_else(|| Error::new("no element"))),
        );
    }

    pub fn element_handles<F: FnOnce(Result<Vec<ElementHandle>, Error>) + 'static>(&self, done: F) {
        self.frame.query_selector_all(&self.selector, done);
    }

    /// `state`: `"attached"`, `"detached"`, `"visible"` (`None`), `"hidden"`.
    pub fn wait_for<F: FnOnce(Result<(), Error>) + 'static>(&self, state: Option<&str>, done: F) {
        self.act("Frame.waitForSelector", Value::object([("state", state.into()), ("omitReturnValue", true.into())]), done);
    }

    pub fn aria_snapshot<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.ariaSnapshot", Value::Null, then(done, |answer| Ok(answer.text("snapshot").unwrap_or_default())));
    }

    pub fn bounding_box<F: FnOnce(Result<Option<BoundingBox>, Error>) + 'static>(&self, done: F) {
        self.with_element(|element, finish| element.bounding_box(finish), done);
    }

    /// A picture of the element, with [`Page::screenshot`]'s options.
    pub fn screenshot<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, options: Value, done: F) {
        self.with_element(move |element, finish| element.screenshot(options, finish), done);
    }

    /// The element's pixels, unencoded, with [`Page::screenshot_pixels`]'s options.
    pub fn screenshot_pixels<F: FnOnce(Result<Pixels<'_>, Error>) + 'static>(&self, options: Value, done: F) {
        self.element_handle(move |element| match element {
            Err(error) => done(Err(error)),
            Ok(element) => {
                let disposing = element.clone();
                element.screenshot_pixels(options, move |pixels| {
                    disposing.dispose(|_| {});
                    done(pixels);
                });
            }
        });
    }

    pub fn scroll_into_view_if_needed<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.with_element(|element, finish| element.scroll_into_view_if_needed(finish), done);
    }
}

fn strings(value: &Value) -> Vec<String> {
    value.as_array().unwrap_or_default().iter().map(|item| item.as_str().unwrap_or_default().to_string()).collect()
}

/// An option of a `<select>` to choose.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SelectOption {
    ValueOrLabel(String),
    Value(String),
    Label(String),
    Index(u32),
}

/// Playwright's FrameLocator: the frame of an `<iframe>` selector.
#[derive(Clone)]
pub struct FrameLocator {
    frame: Frame,
    selector: String,
}

impl FrameLocator {
    pub fn locator(&self, selector: &str) -> Locator {
        self.frame.locator(&format!("{} >> internal:control=enter-frame >> {selector}", self.selector))
    }

    pub fn frame_locator(&self, selector: &str) -> FrameLocator {
        FrameLocator { frame: self.frame.clone(), selector: format!("{} >> internal:control=enter-frame >> {selector}", self.selector) }
    }

    /// The `<iframe>` itself.
    pub fn owner(&self) -> Locator {
        self.frame.locator(&self.selector)
    }
}

/// Playwright's `getBy*` for a type with `locator`.
macro_rules! selectors {
    ($type:ty) => {
        impl $type {
            /// Elements by their ARIA role, and the options' name and states.
            pub fn get_by_role(&self, role: &str, options: RoleOptions) -> Locator {
                self.locator(&options.selector(role))
            }

            /// Elements by their text: a substring ignoring case, or `exact`.
            pub fn get_by_text(&self, text: impl Into<Text>, exact: bool) -> Locator {
                self.locator(&format!("internal:text={}", text.into().selector(exact)))
            }

            /// Inputs by their label's text (or `aria-label`).
            pub fn get_by_label(&self, text: impl Into<Text>, exact: bool) -> Locator {
                self.locator(&format!("internal:label={}", text.into().selector(exact)))
            }

            pub fn get_by_placeholder(&self, text: impl Into<Text>, exact: bool) -> Locator {
                self.locator(&format!("internal:attr=[placeholder={}]", text.into().attribute(exact)))
            }

            pub fn get_by_alt_text(&self, text: impl Into<Text>, exact: bool) -> Locator {
                self.locator(&format!("internal:attr=[alt={}]", text.into().attribute(exact)))
            }

            pub fn get_by_title(&self, text: impl Into<Text>, exact: bool) -> Locator {
                self.locator(&format!("internal:attr=[title={}]", text.into().attribute(exact)))
            }

            /// Elements by their `data-testid`.
            pub fn get_by_test_id(&self, id: &str) -> Locator {
                self.locator(&format!("internal:testid=[data-testid={}]", Text::from(id).attribute(true)))
            }
        }
    };
}

selectors!(Frame);
selectors!(Locator);
selectors!(FrameLocator);

// ---- Handles ---------------------------------------------------------------

/// A box in CSS pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct BoundingBox {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

fn handle_of(frame: &Frame, value: &Value) -> Option<JSHandle> {
    let id = value.text("handle")?;
    let element = !matches!(value["element"], Value::Null | Value::Bool(false));
    Some(JSHandle { frame: frame.clone(), id, preview: value.text("preview").unwrap_or_default(), element })
}

fn element_of(frame: &Frame, value: &Value) -> Option<ElementHandle> {
    handle_of(frame, value).map(|handle| ElementHandle { handle })
}

/// A JavaScript object of the page, kept until [`JSHandle::dispose`].
#[derive(Clone)]
pub struct JSHandle {
    frame: Frame,
    id: String,
    preview: String,
    element: bool,
}

impl std::fmt::Debug for JSHandle {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "JSHandle({} {})", self.id, self.preview)
    }
}

impl JSHandle {
    pub fn id(&self) -> &str {
        &self.id
    }

    /// How the engine shows it ("JSHandle@object", "div#out").
    pub fn preview(&self) -> &str {
        &self.preview
    }

    /// The handle as an argument of an evaluation.
    pub fn as_arg(&self) -> Value {
        Value::object([("handle", self.id.as_str().into())])
    }

    pub fn as_element(&self) -> Option<ElementHandle> {
        self.element.then(|| ElementHandle { handle: self.clone() })
    }

    fn prefix(&self) -> &'static str {
        if self.element {
            "ElementHandle."
        } else {
            "JSHandle."
        }
    }

    fn run(&self, name: &str, params: Value, done: impl FnOnce(Answer) + 'static) {
        let mut params = object_or_empty(params);
        params.set("handle", self.id.as_str().into());
        self.frame.core.run(&format!("{}{name}", self.prefix()), params, done);
    }

    pub fn evaluate<F: FnOnce(Result<Value, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        self.run("evaluateExpression", self.frame.evaluation(expression, arg), value(done));
    }

    pub fn evaluate_handle<F: FnOnce(Result<Option<JSHandle>, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        let frame = self.frame.clone();
        self.run("evaluateExpressionHandle", self.frame.evaluation(expression, arg), then(done, move |answer| Ok(handle_of(&frame, &answer["handle"]))));
    }

    pub fn get_property<F: FnOnce(Result<Option<JSHandle>, Error>) + 'static>(&self, name: &str, done: F) {
        let frame = self.frame.clone();
        self.run("getProperty", Value::object([("name", name.into())]), then(done, move |answer| Ok(handle_of(&frame, &answer["handle"]))));
    }

    pub fn get_properties<F: FnOnce(Result<Vec<(String, Option<JSHandle>)>, Error>) + 'static>(&self, done: F) {
        let frame = self.frame.clone();
        self.run(
            "getPropertyList",
            Value::Null,
            then(done, move |answer| {
                Ok(answer["properties"].as_array().unwrap_or_default().iter().map(|property| (property.text("name").unwrap_or_default(), handle_of(&frame, &property["value"]))).collect())
            }),
        );
    }

    pub fn json_value<F: FnOnce(Result<Value, Error>) + 'static>(&self, done: F) {
        self.run("jsonValue", Value::Null, value(done));
    }

    pub fn dispose<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.run("dispose", Value::Null, unit(done));
    }
}

/// An element of the page: a [`JSHandle`] (through `Deref`) with Playwright's
/// ElementHandle API.
#[derive(Clone, Debug)]
pub struct ElementHandle {
    handle: JSHandle,
}

impl std::ops::Deref for ElementHandle {
    type Target = JSHandle;
    fn deref(&self) -> &JSHandle {
        &self.handle
    }
}

impl ElementHandle {
    fn act(&self, name: &str, params: Value, done: impl FnOnce(Result<(), Error>) + 'static) {
        self.handle.run(name, params, unit(done));
    }

    /// Runs an ElementHandle command (`"click"`, `"fill"`, ...) with `params`.
    pub fn command<F: FnOnce(Result<Value, Error>) + 'static>(&self, name: &str, params: Value, done: F) {
        self.handle.run(name, params, done);
    }

    pub fn click<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.act("click", options, done);
    }

    pub fn dblclick<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.act("dblclick", options, done);
    }

    pub fn hover<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.act("hover", options, done);
    }

    pub fn tap<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.act("tap", options, done);
    }

    pub fn check<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.act("check", options, done);
    }

    pub fn uncheck<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.act("uncheck", options, done);
    }

    pub fn fill<F: FnOnce(Result<(), Error>) + 'static>(&self, value: &str, done: F) {
        self.act("fill", Value::object([("value", value.into())]), done);
    }

    pub fn press<F: FnOnce(Result<(), Error>) + 'static>(&self, key: &str, done: F) {
        self.act("press", Value::object([("key", key.into())]), done);
    }

    pub fn type_text<F: FnOnce(Result<(), Error>) + 'static>(&self, text: &str, done: F) {
        self.act("type", Value::object([("text", text.into())]), done);
    }

    pub fn focus<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("focus", Value::Null, done);
    }

    pub fn select_text<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("selectText", Value::Null, done);
    }

    pub fn scroll_into_view_if_needed<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.act("scrollIntoViewIfNeeded", Value::Null, done);
    }

    pub fn text_content<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, done: F) {
        self.handle.run("textContent", Value::Null, string_value(done));
    }

    pub fn inner_text<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.handle.run("innerText", Value::Null, text_value(done));
    }

    pub fn inner_html<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.handle.run("innerHTML", Value::Null, text_value(done));
    }

    pub fn get_attribute<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, name: &str, done: F) {
        self.handle.run("getAttribute", Value::object([("name", name.into())]), string_value(done));
    }

    pub fn input_value<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.handle.run("inputValue", Value::Null, text_value(done));
    }

    pub fn is_visible<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.handle.run("isVisible", Value::Null, bool_value(done));
    }

    pub fn is_hidden<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.handle.run("isHidden", Value::Null, bool_value(done));
    }

    pub fn is_enabled<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.handle.run("isEnabled", Value::Null, bool_value(done));
    }

    pub fn is_checked<F: FnOnce(Result<bool, Error>) + 'static>(&self, done: F) {
        self.handle.run("isChecked", Value::Null, bool_value(done));
    }

    pub fn bounding_box<F: FnOnce(Result<Option<BoundingBox>, Error>) + 'static>(&self, done: F) {
        self.handle.run(
            "boundingBox",
            Value::Null,
            then(done, |answer| {
                let value = &answer["value"];
                Ok(value["width"].as_f64().map(|width| BoundingBox {
                    x: value["x"].as_f64().unwrap_or(0.0),
                    y: value["y"].as_f64().unwrap_or(0.0),
                    width,
                    height: value["height"].as_f64().unwrap_or(0.0),
                }))
            }),
        );
    }

    /// A picture of the element, with [`Page::screenshot`]'s options.
    pub fn screenshot<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, options: Value, done: F) {
        let (path, options) = take_path(options);
        self.handle.run("screenshot", options, then(done, move |answer| write_file(path.as_deref(), binary(&answer, "binary")?)));
    }

    /// The element's pixels, unencoded.
    pub fn screenshot_pixels<F: FnOnce(Result<Pixels<'_>, Error>) + 'static>(&self, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("handle", self.id.as_str().into());
        screenshot_pixels(&self.frame.core.view, "ElementHandle.screenshot", &params.to_string(), done);
    }

    pub fn query_selector<F: FnOnce(Result<Option<ElementHandle>, Error>) + 'static>(&self, selector: &str, done: F) {
        let frame = self.frame.clone();
        self.handle.run("querySelector", Value::object([("selector", selector.into())]), then(done, move |answer| Ok(element_of(&frame, &answer["element"]))));
    }

    pub fn query_selector_all<F: FnOnce(Result<Vec<ElementHandle>, Error>) + 'static>(&self, selector: &str, done: F) {
        let frame = self.frame.clone();
        self.handle.run(
            "querySelectorAll",
            Value::object([("selector", selector.into())]),
            then(done, move |answer| Ok(answer["elements"].as_array().unwrap_or_default().iter().filter_map(|element| element_of(&frame, element)).collect())),
        );
    }

    /// `state`: `"visible"`, `"hidden"`, `"stable"`, `"enabled"`, `"disabled"`, `"editable"`.
    pub fn wait_for_element_state<F: FnOnce(Result<(), Error>) + 'static>(&self, state: &str, timeout: Option<f64>, done: F) {
        self.act("waitForElementState", Value::object([("state", state.into()), ("timeout", milliseconds(timeout))]), done);
    }

    /// The frame of an `<iframe>` element.
    pub fn content_frame<F: FnOnce(Result<Option<Frame>, Error>) + 'static>(&self, done: F) {
        let core = self.frame.core.clone();
        self.handle.run(
            "contentFrame",
            Value::Null,
            then(done, move |answer| Ok(answer.text("frame").map(|id| Frame { core, id: Some(id), name: String::new(), url: String::new() }))),
        );
    }

    pub fn set_input_files<F: FnOnce(Result<(), Error>) + 'static>(&self, paths: &[&str], done: F) {
        let list = Value::Array(paths.iter().map(|path| absolute(path).into()).collect());
        self.act("setInputFiles", Value::object([("localPaths", list)]), done);
    }
}

// ---- Input ------------------------------------------------------------------

/// The page's keyboard.
#[derive(Clone)]
pub struct Keyboard {
    core: Core,
}

impl Keyboard {
    pub fn down<F: FnOnce(Result<(), Error>) + 'static>(&self, key: &str, done: F) {
        self.core.run("Page.keyboardDown", Value::object([("key", key.into())]), unit(done));
    }

    pub fn up<F: FnOnce(Result<(), Error>) + 'static>(&self, key: &str, done: F) {
        self.core.run("Page.keyboardUp", Value::object([("key", key.into())]), unit(done));
    }

    /// `delay`: milliseconds between down and up.
    pub fn press<F: FnOnce(Result<(), Error>) + 'static>(&self, key: &str, delay: Option<f64>, done: F) {
        self.core.run("Page.keyboardPress", Value::object([("key", key.into()), ("delay", delay.into())]), unit(done));
    }

    pub fn type_text<F: FnOnce(Result<(), Error>) + 'static>(&self, text: &str, delay: Option<f64>, done: F) {
        self.core.run("Page.keyboardType", Value::object([("text", text.into()), ("delay", delay.into())]), unit(done));
    }

    pub fn insert_text<F: FnOnce(Result<(), Error>) + 'static>(&self, text: &str, done: F) {
        self.core.run("Page.keyboardInsertText", Value::object([("text", text.into())]), unit(done));
    }
}

/// The page's mouse; `options` are `button`, `clickCount`, `delay`.
#[derive(Clone)]
pub struct Mouse {
    core: Core,
}

impl Mouse {
    pub fn move_to<F: FnOnce(Result<(), Error>) + 'static>(&self, x: f64, y: f64, steps: Option<u32>, done: F) {
        self.core.run("Page.mouseMove", Value::object([("x", x.into()), ("y", y.into()), ("steps", steps.into())]), unit(done));
    }

    pub fn down<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.core.run("Page.mouseDown", options, unit(done));
    }

    pub fn up<F: FnOnce(Result<(), Error>) + 'static>(&self, options: Value, done: F) {
        self.core.run("Page.mouseUp", options, unit(done));
    }

    pub fn click<F: FnOnce(Result<(), Error>) + 'static>(&self, x: f64, y: f64, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("x", x.into());
        params.set("y", y.into());
        self.core.run("Page.mouseClick", params, unit(done));
    }

    pub fn dblclick<F: FnOnce(Result<(), Error>) + 'static>(&self, x: f64, y: f64, options: Value, done: F) {
        let mut options = object_or_empty(options);
        options.set("clickCount", 2.into());
        self.click(x, y, options, done);
    }

    pub fn wheel<F: FnOnce(Result<(), Error>) + 'static>(&self, delta_x: f64, delta_y: f64, done: F) {
        self.core.run("Page.mouseWheel", Value::object([("deltaX", delta_x.into()), ("deltaY", delta_y.into())]), unit(done));
    }
}

#[derive(Clone)]
pub struct Touchscreen {
    core: Core,
}

impl Touchscreen {
    pub fn tap<F: FnOnce(Result<(), Error>) + 'static>(&self, x: f64, y: f64, done: F) {
        self.core.run("Page.touchscreenTap", Value::object([("x", x.into()), ("y", y.into())]), unit(done));
    }
}

// ---- Network, dialogs, files ----------------------------------------------

/// Playwright's URL globs: `*` (no `/`), `**`, `**/`, `{a,b}`, `\` to escape.
struct Glob {
    tokens: Vec<Token>,
}

enum Token {
    Char(char),
    /// `*`: anything but `/`.
    Star,
    /// `**`: anything.
    Any,
    /// `**/`: nothing, or anything ending in `/`; `before_slash` when a `/`
    /// came before, which then needs one character at least.
    Dirs { after_slash: bool },
    Group(Vec<Vec<Token>>),
}

impl Glob {
    fn new(glob: &str) -> Glob {
        let chars: Vec<char> = glob.chars().collect();
        let mut at = 0;
        let tokens = Glob::parse(&chars, &mut at, false).pop().unwrap_or_default();
        Glob { tokens }
    }

    /// The alternatives up to the end, or to `}` inside a group.
    fn parse(chars: &[char], at: &mut usize, in_group: bool) -> Vec<Vec<Token>> {
        let mut alternatives = vec![Vec::new()];
        while *at < chars.len() {
            let c = chars[*at];
            *at += 1;
            let current = alternatives.last_mut().expect("one alternative");
            match c {
                '\\' if *at < chars.len() => {
                    current.push(Token::Char(chars[*at]));
                    *at += 1;
                }
                '*' => {
                    let before = if *at >= 2 { Some(chars[*at - 2]) } else { None };
                    let mut stars = 1;
                    while chars.get(*at) == Some(&'*') {
                        stars += 1;
                        *at += 1;
                    }
                    if stars > 1 {
                        if chars.get(*at) == Some(&'/') {
                            *at += 1;
                            current.push(Token::Dirs { after_slash: before == Some('/') });
                        } else {
                            current.push(Token::Any);
                        }
                    } else {
                        current.push(Token::Star);
                    }
                }
                '{' if !in_group => {
                    let group = Glob::parse(chars, at, true);
                    alternatives.last_mut().expect("one alternative").push(Token::Group(group));
                }
                '}' if in_group => return alternatives,
                ',' if in_group => alternatives.push(Vec::new()),
                c => current.push(Token::Char(c)),
            }
        }
        alternatives
    }

    fn matches(&self, text: &str) -> bool {
        let chars: Vec<char> = text.chars().collect();
        Glob::match_at(&self.tokens, &chars, &mut Vec::new())
    }

    /// Whether `tokens` and then `rest` (what follows a group) match `text` whole.
    fn match_at<'a>(tokens: &'a [Token], text: &[char], rest: &mut Vec<&'a [Token]>) -> bool {
        let Some((token, tail)) = tokens.split_first() else {
            return match rest.pop() {
                Some(next) => {
                    let matched = Glob::match_at(next, text, rest);
                    rest.push(next);
                    matched
                }
                None => text.is_empty(),
            };
        };
        match token {
            Token::Char(c) => text.first() == Some(c) && Glob::match_at(tail, &text[1..], rest),
            Token::Star => (0..=text.len()).take_while(|&n| n == 0 || text[n - 1] != '/').any(|n| Glob::match_at(tail, &text[n..], rest)),
            Token::Any => (0..=text.len()).any(|n| Glob::match_at(tail, &text[n..], rest)),
            Token::Dirs { after_slash } => {
                // `(.*/)` after anything, `((.+/)|)` after a `/`.
                (*after_slash && Glob::match_at(tail, text, rest))
                    || (1..=text.len())
                        .filter(|&n| text[n - 1] == '/' && (!*after_slash || n >= 2))
                        .any(|n| Glob::match_at(tail, &text[n..], rest))
            }
            Token::Group(alternatives) => alternatives.iter().any(|alternative| {
                rest.push(tail);
                let matched = Glob::match_at(alternative, text, rest);
                rest.pop();
                matched
            }),
        }
    }
}

/// A request of the page, as Playwright's Request.
#[derive(Clone, Debug)]
pub struct Request {
    value: Rc<Value>,
}

impl Request {
    pub fn url(&self) -> &str {
        self.value["url"].as_str().unwrap_or_default()
    }

    pub fn method(&self) -> &str {
        self.value["method"].as_str().unwrap_or_default()
    }

    pub fn resource_type(&self) -> &str {
        self.value["resourceType"].as_str().unwrap_or_default()
    }

    /// Its headers, their names in lower case.
    pub fn headers(&self) -> Vec<(String, String)> {
        headers_of(&self.value)
    }

    /// Why it failed, for a failed one.
    pub fn failure(&self) -> Option<&str> {
        self.value["failureText"].as_str()
    }

    pub fn is_navigation_request(&self) -> bool {
        self.value["isNavigationRequest"].as_bool().unwrap_or(false)
    }

    /// All the protocol says of it.
    pub fn json(&self) -> &Value {
        &self.value
    }
}

/// A response, as Playwright's Response.
#[derive(Clone, Debug)]
pub struct Response {
    core: Option<Core>,
    value: Rc<Value>,
}

impl std::fmt::Debug for Core {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("Page")
    }
}

impl Response {
    pub fn url(&self) -> &str {
        self.value["url"].as_str().unwrap_or_default()
    }

    /// The HTTP status, 0 when there was none.
    pub fn status(&self) -> u16 {
        self.value["status"].as_f64().unwrap_or(0.0) as u16
    }

    pub fn status_text(&self) -> &str {
        self.value["statusText"].as_str().unwrap_or_default()
    }

    pub fn headers(&self) -> Vec<(String, String)> {
        headers_of(&self.value)
    }

    /// 200-299, or 0 (a response with no HTTP, such as a file's).
    pub fn ok(&self) -> bool {
        self.value["status"].as_f64().is_some_and(|status| status == 0.0 || (200.0..=299.0).contains(&status))
    }

    pub fn json(&self) -> &Value {
        &self.value
    }

    /// Its body.
    pub fn body<F: FnOnce(Result<Vec<u8>, Error>) + 'static>(&self, done: F) {
        let Some(core) = &self.core else {
            done(Err(Error::new("the response is of no page")));
            return;
        };
        core.run("Response.body", Value::object([("request", self.value.get("request").cloned().unwrap_or_default())]), then(done, |answer| binary(&answer, "binary")));
    }

    /// Its body as text (UTF-8, lossily).
    pub fn text<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.body(move |body| done(body.map(|body| String::from_utf8_lossy(&body).into_owned())));
    }
}

/// A request a route holds: continue, fulfill or abort it.
#[derive(Clone)]
pub struct Route {
    core: Core,
    id: String,
    request: Request,
}

impl Route {
    pub fn request(&self) -> &Request {
        &self.request
    }

    /// Continues it, with `overrides`: `url`, `method`, `headers` (`[{"name",
    /// "value"}]`), `postData` (bytes in base64; [`Route::continue_with_body`]).
    pub fn continue_<F: FnOnce(Result<(), Error>) + 'static>(&self, overrides: Value, done: F) {
        let mut params = object_or_empty(overrides);
        params.set("route", self.id.as_str().into());
        self.core.run("Route.continue", params, unit(done));
    }

    /// Continues it with `body` as what it sends.
    pub fn continue_with_body<F: FnOnce(Result<(), Error>) + 'static>(&self, body: &[u8], done: F) {
        self.continue_(Value::object([("postData", base64_encode(body).into())]), done);
    }

    /// Answers it: `status`, `headers`, `body`, and a `content_type` header when given.
    pub fn fulfill<F: FnOnce(Result<(), Error>) + 'static>(&self, status: u16, headers: &[(&str, &str)], body: &[u8], content_type: Option<&str>, done: F) {
        let mut listed: Vec<(&str, &str)> = headers.to_vec();
        if let Some(content_type) = content_type {
            listed.push(("content-type", content_type));
        }
        let params = Value::object([
            ("route", self.id.as_str().into()),
            ("status", status.into()),
            ("headers", header_list(&listed)),
            ("body", base64_encode(body).into()),
            ("isBase64", true.into()),
        ]);
        self.core.run("Route.fulfill", params, unit(done));
    }

    /// Answers it with `value` as JSON.
    pub fn fulfill_json<F: FnOnce(Result<(), Error>) + 'static>(&self, value: &Value, done: F) {
        self.fulfill(200, &[], value.to_string().as_bytes(), Some("application/json"), done);
    }

    /// Fails it: `error_code` as Playwright's ("failed", "aborted", "timedout", ...).
    pub fn abort<F: FnOnce(Result<(), Error>) + 'static>(&self, error_code: &str, done: F) {
        self.core.run("Route.abort", Value::object([("route", self.id.as_str().into()), ("errorCode", error_code.into())]), unit(done));
    }
}

/// A call of a function [`Page::expose_binding`] gave the page; one dropped
/// unanswered rejects the page's promise.
pub struct BindingCall {
    core: Core,
    id: String,
    frame: Option<String>,
    args: Vec<Value>,
    answered: Cell<bool>,
}

impl BindingCall {
    pub fn args(&self) -> &[Value] {
        &self.args
    }

    /// The id of the frame that called.
    pub fn frame(&self) -> Option<&str> {
        self.frame.as_deref()
    }

    pub fn resolve(self, value: Value) {
        self.answered.set(true);
        self.core.run("BindingCall.resolve", Value::object([("binding", self.id.as_str().into()), ("result", value)]), |_| {});
    }

    pub fn reject(self, message: &str) {
        self.answer_error(message);
    }

    fn answer_error(&self, message: &str) {
        self.answered.set(true);
        self.core.run("BindingCall.reject", Value::object([("binding", self.id.as_str().into()), ("error", message.into())]), |_| {});
    }
}

impl Drop for BindingCall {
    fn drop(&mut self) {
        if !self.answered.get() {
            self.answer_error("the application did not answer");
        }
    }
}

/// A JavaScript dialog of the page, held for a [`EventKind::Dialog`] listener.
#[derive(Clone, Debug)]
pub struct Dialog {
    core: Core,
    value: Rc<Value>,
}

impl Dialog {
    /// `"alert"`, `"confirm"`, `"prompt"` or `"beforeunload"`.
    pub fn kind(&self) -> &str {
        self.value["type"].as_str().unwrap_or_default()
    }

    pub fn message(&self) -> &str {
        self.value["message"].as_str().unwrap_or_default()
    }

    pub fn default_value(&self) -> &str {
        self.value["defaultValue"].as_str().unwrap_or_default()
    }

    fn id(&self) -> Value {
        self.value.get("dialog").cloned().unwrap_or_default()
    }

    /// OK, a prompt with `prompt_text` (`None`: its default).
    pub fn accept<F: FnOnce(Result<(), Error>) + 'static>(&self, prompt_text: Option<&str>, done: F) {
        self.core.run("Dialog.accept", Value::object([("dialog", self.id()), ("promptText", prompt_text.into())]), unit(done));
    }

    pub fn dismiss<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.core.run("Dialog.dismiss", Value::object([("dialog", self.id())]), unit(done));
    }
}

/// A file chooser the page opened, held for a [`EventKind::FileChooser`] listener.
#[derive(Clone, Debug)]
pub struct FileChooser {
    core: Core,
    value: Rc<Value>,
}

impl FileChooser {
    pub fn is_multiple(&self) -> bool {
        self.value["isMultiple"].as_bool().unwrap_or(false)
    }

    pub fn page(&self) -> Page {
        Page::from_core(self.core.clone())
    }

    pub fn set_files<F: FnOnce(Result<(), Error>) + 'static>(&self, paths: &[&str], done: F) {
        let list = Value::Array(paths.iter().map(|path| absolute(path).into()).collect());
        self.core.run("FileChooser.setFiles", Value::object([("fileChooser", self.value.get("fileChooser").cloned().unwrap_or_default()), ("localPaths", list)]), unit(done));
    }
}

/// A download of the page.
#[derive(Clone, Debug)]
pub struct Download {
    core: Core,
    value: Rc<Value>,
}

impl Download {
    pub fn url(&self) -> &str {
        self.value["url"].as_str().unwrap_or_default()
    }

    pub fn suggested_filename(&self) -> &str {
        self.value["suggestedFilename"].as_str().unwrap_or_default()
    }

    pub fn page(&self) -> Page {
        Page::from_core(self.core.clone())
    }

    fn run(&self, method: &str, mut params: Value, done: impl FnOnce(Answer) + 'static) {
        params.set("download", self.value.get("download").cloned().unwrap_or_default());
        self.core.run(method, params, done);
    }

    /// Where it was saved, once it has finished; `None` when it failed.
    pub fn path<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, done: F) {
        self.run("Artifact.pathAfterFinished", Value::Null, string_value(done));
    }

    pub fn save_as<F: FnOnce(Result<(), Error>) + 'static>(&self, path: &str, done: F) {
        self.run("Artifact.saveAs", Value::object([("path", path.into())]), unit(done));
    }

    /// Why it failed; `None` when it did not.
    pub fn failure<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, done: F) {
        self.run("Artifact.failure", Value::Null, then(done, |answer| Ok(answer.text("error"))));
    }

    pub fn cancel<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.run("Artifact.cancel", Value::Null, unit(done));
    }

    pub fn delete<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.run("Artifact.delete", Value::Null, unit(done));
    }
}

/// A message of the page's console.
#[derive(Clone, Debug)]
pub struct ConsoleMessage {
    value: Rc<Value>,
}

impl ConsoleMessage {
    /// `"log"`, `"warning"`, `"error"`, ...
    pub fn kind(&self) -> &str {
        self.value["type"].as_str().unwrap_or_default()
    }

    pub fn text(&self) -> &str {
        self.value["text"].as_str().unwrap_or_default()
    }

    /// `{"url", "lineNumber", "columnNumber"}`.
    pub fn location(&self) -> &Value {
        &self.value["location"]
    }
}

/// A dedicated worker of the page ([`Page::workers`], [`EventKind::Worker`]).
#[derive(Clone)]
pub struct Worker {
    core: Core,
    id: String,
    url: String,
    #[allow(clippy::type_complexity)]
    close_listeners: Rc<RefCell<Vec<Box<dyn FnMut(&Worker)>>>>,
}

impl std::fmt::Debug for Worker {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "Worker({})", self.url)
    }
}

impl Worker {
    pub fn url(&self) -> &str {
        &self.url
    }

    pub fn evaluate<F: FnOnce(Result<Value, Error>) + 'static>(&self, expression: &str, arg: Value, done: F) {
        let params = Value::object([
            ("worker", self.id.as_str().into()),
            ("expression", expression.into()),
            ("arg", arg),
            ("isFunction", looks_like_function(expression).then_some(true).into()),
        ]);
        self.core.run("Worker.evaluateExpression", params, value(done));
    }

    /// Calls `listener` once the worker ends.
    pub fn on_close<F: FnMut(&Worker) + 'static>(&self, listener: F) {
        self.close_listeners.borrow_mut().push(Box::new(listener));
    }
}

/// Playwright's traces (the zip its trace viewer opens) and HAR files of the page.
#[derive(Clone)]
pub struct Tracing {
    core: Core,
}

impl Tracing {
    pub fn start<F: FnOnce(Result<(), Error>) + 'static>(&self, name: Option<&str>, title: Option<&str>, screenshots: bool, snapshots: bool, done: F) {
        let core = self.core.clone();
        let title: Value = title.into();
        self.core.run(
            "Tracing.tracingStart",
            Value::object([("name", name.into()), ("screenshots", screenshots.into()), ("snapshots", snapshots.into())]),
            move |answer| match answer {
                Err(error) => done(Err(error)),
                Ok(_) => core.run("Tracing.tracingStartChunk", Value::object([("title", title)]), unit(done)),
            },
        );
    }

    pub fn start_chunk<F: FnOnce(Result<(), Error>) + 'static>(&self, title: Option<&str>, done: F) {
        self.core.run("Tracing.tracingStartChunk", Value::object([("title", title.into())]), unit(done));
    }

    /// Ends the chunk, written to `path` as a zip (`None`: discarded); the path written.
    pub fn stop_chunk<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, path: Option<&str>, done: F) {
        let params = match path {
            Some(path) => Value::object([("mode", "archive".into()), ("path", path.into())]),
            None => Value::object([("mode", "discard".into())]),
        };
        let written = path.is_some();
        self.core.run("Tracing.tracingStopChunk", params, then(done, move |answer| Ok(if written { answer.text("path") } else { None })));
    }

    pub fn stop<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, path: Option<&str>, done: F) {
        let core = self.core.clone();
        self.stop_chunk(path, move |written| match written {
            Err(error) => done(Err(error)),
            Ok(written) => core.run("Tracing.tracingStop", Value::Null, then(done, move |_| Ok(written))),
        });
    }

    /// Starts recording a HAR (`content`: `"omit"`, `"embed"`, `"attach"`;
    /// `mode`: `"full"`, `"minimal"`; `url_filter`: a glob); its id.
    pub fn start_har<F: FnOnce(Result<String, Error>) + 'static>(&self, path: Option<&str>, content: Option<&str>, mode: Option<&str>, url_filter: Option<&str>, done: F) {
        let options = Value::Object(
            [("content", content), ("mode", mode), ("urlGlob", url_filter)]
                .into_iter()
                .filter_map(|(name, value)| value.map(|value| (name.to_string(), value.into())))
                .collect(),
        );
        self.core.run("Tracing.harStart", Value::object([("options", options), ("path", path.into())]), then(done, |answer| Ok(answer.text("harId").unwrap_or_default())));
    }

    /// Writes HAR `har_id` to `path`; the path written.
    pub fn export_har<F: FnOnce(Result<Option<String>, Error>) + 'static>(&self, har_id: &str, path: Option<&str>, done: F) {
        self.core.run("Tracing.harExport", Value::object([("harId", har_id.into()), ("path", path.into())]), then(done, |answer| Ok(answer.text("path"))));
    }
}

/// A time for the [`Clock`]: milliseconds since the epoch, or a date as
/// JavaScript reads one.
#[derive(Clone, Debug, PartialEq)]
pub enum ClockTime {
    Milliseconds(f64),
    Text(String),
}

impl From<f64> for ClockTime {
    fn from(value: f64) -> ClockTime {
        ClockTime::Milliseconds(value)
    }
}

impl From<i64> for ClockTime {
    fn from(value: i64) -> ClockTime {
        ClockTime::Milliseconds(value as f64)
    }
}

impl From<&str> for ClockTime {
    fn from(value: &str) -> ClockTime {
        ClockTime::Text(value.to_string())
    }
}

impl ClockTime {
    fn params(&self, ticks: bool) -> Value {
        let head = if ticks { "ticks" } else { "time" };
        match self {
            ClockTime::Milliseconds(value) => Value::object([(format!("{head}Number"), (*value).into())]),
            ClockTime::Text(value) => Value::object([(format!("{head}String"), value.as_str().into())]),
        }
    }
}

/// The page's clock, as Playwright's.
#[derive(Clone)]
pub struct Clock {
    core: Core,
}

impl Clock {
    pub fn install<F: FnOnce(Result<(), Error>) + 'static>(&self, time: Option<ClockTime>, done: F) {
        self.core.run("BrowserContext.clockInstall", time.map(|time| time.params(false)).unwrap_or_default(), unit(done));
    }

    pub fn fast_forward<F: FnOnce(Result<(), Error>) + 'static>(&self, ticks: impl Into<ClockTime>, done: F) {
        self.core.run("BrowserContext.clockFastForward", ticks.into().params(true), unit(done));
    }

    pub fn run_for<F: FnOnce(Result<(), Error>) + 'static>(&self, ticks: impl Into<ClockTime>, done: F) {
        self.core.run("BrowserContext.clockRunFor", ticks.into().params(true), unit(done));
    }

    pub fn pause_at<F: FnOnce(Result<(), Error>) + 'static>(&self, time: impl Into<ClockTime>, done: F) {
        self.core.run("BrowserContext.clockPauseAt", time.into().params(false), unit(done));
    }

    pub fn resume<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.core.run("BrowserContext.clockResume", Value::Null, unit(done));
    }

    pub fn set_fixed_time<F: FnOnce(Result<(), Error>) + 'static>(&self, time: impl Into<ClockTime>, done: F) {
        self.core.run("BrowserContext.clockSetFixedTime", time.into().params(false), unit(done));
    }

    pub fn set_system_time<F: FnOnce(Result<(), Error>) + 'static>(&self, time: impl Into<ClockTime>, done: F) {
        self.core.run("BrowserContext.clockSetSystemTime", time.into().params(false), unit(done));
    }
}

/// Requests through the engine's network, with the page's cookies
/// ([`Page::request`]).
#[derive(Clone)]
pub struct APIRequestContext {
    core: Core,
}

/// [`APIRequestContext::fetch`]'s request.
#[derive(Clone, Debug, Default)]
pub struct FetchOptions<'a> {
    /// `None`: GET.
    pub method: Option<&'a str>,
    pub headers: &'a [(&'a str, &'a str)],
    pub data: Option<&'a [u8]>,
    pub timeout: Option<f64>,
    pub max_redirects: Option<u32>,
    pub max_body_size: Option<u64>,
}

impl APIRequestContext {
    pub fn fetch<F: FnOnce(Result<APIResponse, Error>) + 'static>(&self, url: &str, options: &FetchOptions<'_>, done: F) {
        let params = Value::object([
            ("url", url.into()),
            ("method", options.method.unwrap_or("GET").into()),
            ("headers", header_list(options.headers)),
            ("postData", options.data.map(base64_encode).into()),
            ("timeout", milliseconds(options.timeout)),
            ("maxRedirects", options.max_redirects.into()),
            ("maxBodySize", options.max_body_size.into()),
        ]);
        self.core.run(
            "APIRequestContext.fetch",
            params,
            then(done, |answer| {
                let value = member(answer, "response");
                let body = base64_decode(value["body"].as_str().unwrap_or_default())?;
                Ok(APIResponse { value, body })
            }),
        );
    }

    pub fn get<F: FnOnce(Result<APIResponse, Error>) + 'static>(&self, url: &str, done: F) {
        self.fetch(url, &FetchOptions::default(), done);
    }

    pub fn post<F: FnOnce(Result<APIResponse, Error>) + 'static>(&self, url: &str, data: &[u8], done: F) {
        self.fetch(url, &FetchOptions { method: Some("POST"), data: Some(data), ..Default::default() }, done);
    }
}

/// [`APIRequestContext::fetch`]'s answer, with its body.
#[derive(Clone, Debug)]
pub struct APIResponse {
    value: Value,
    body: Vec<u8>,
}

impl APIResponse {
    pub fn url(&self) -> &str {
        self.value["url"].as_str().unwrap_or_default()
    }

    pub fn status(&self) -> u16 {
        self.value["status"].as_f64().unwrap_or(0.0) as u16
    }

    pub fn status_text(&self) -> &str {
        self.value["statusText"].as_str().unwrap_or_default()
    }

    pub fn headers(&self) -> Vec<(String, String)> {
        headers_of(&self.value)
    }

    pub fn ok(&self) -> bool {
        let status = self.status();
        status == 0 || (200..=299).contains(&status)
    }

    pub fn body(&self) -> &[u8] {
        &self.body
    }

    pub fn text(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }

    pub fn json(&self) -> Result<Value, Error> {
        Value::parse(&self.text())
    }
}

// ---- Contexts ---------------------------------------------------------------

/// A context of its own ([`new_context`]): its own profile, cookies,
/// storage, permissions and cache, and its pages.
#[derive(Clone)]
pub struct BrowserContext {
    id: String,
    pages: Rc<RefCell<Vec<Page>>>,
}

/// `browser.newContext()`: Playwright's options as the protocol names them
/// (`{"viewport": {...}, "userAgent": ..., "geolocation": ...}`).
pub fn new_context<F: FnOnce(Result<BrowserContext, Error>) + 'static>(options: Value, done: F) {
    run(
        None,
        "Browser.newContext",
        options,
        then(done, |answer| {
            answer.text("context").map(|id| BrowserContext { id, pages: Rc::default() }).ok_or_else(|| Error::new("no context"))
        }),
    );
}

impl BrowserContext {
    /// Its id ("x1").
    pub fn id(&self) -> &str {
        &self.id
    }

    /// The pages [`BrowserContext::new_page`] made, open or not.
    pub fn pages(&self) -> Vec<Page> {
        self.pages.borrow().clone()
    }

    /// A new page of the context: an off-screen view under --headless.
    pub fn new_page<F: FnOnce(Result<Page, Error>) + 'static>(&self, done: F) {
        let pages = self.pages.clone();
        self.command(
            "BrowserContext.newPage",
            Value::Null,
            then(done, move |answer| {
                let id = answer.text("page").ok_or_else(|| Error::new("no page"))?;
                let raw = unsafe { sys::zebkit_automation_page(c_string(&id).as_ptr()) };
                if raw.is_null() {
                    return Err(Error::new("the page is closed"));
                }
                let page = Page::new(View::wrap(raw));
                pages.borrow_mut().push(page.clone());
                Ok(page)
            }),
        );
    }

    /// One of the context's commands ("BrowserContext.cookies", ...), with
    /// the context added to `params`.
    pub fn command<F: FnOnce(Result<Value, Error>) + 'static>(&self, method: &str, params: Value, done: F) {
        let mut params = object_or_empty(params);
        params.set("context", self.id.as_str().into());
        run(None, method, params, done);
    }

    pub fn cookies<F: FnOnce(Result<Vec<Value>, Error>) + 'static>(&self, urls: &[&str], done: F) {
        self.command("BrowserContext.cookies", cookie_urls(urls), then(done, cookie_list));
    }

    pub fn add_cookies<F: FnOnce(Result<(), Error>) + 'static>(&self, cookies: Value, done: F) {
        self.command("BrowserContext.addCookies", Value::object([("cookies", cookies)]), unit(done));
    }

    pub fn clear_cookies<F: FnOnce(Result<(), Error>) + 'static>(&self, name: Option<&str>, domain: Option<&str>, path: Option<&str>, done: F) {
        self.command("BrowserContext.clearCookies", Value::object([("name", name.into()), ("domain", domain.into()), ("path", path.into())]), unit(done));
    }

    pub fn grant_permissions<F: FnOnce(Result<(), Error>) + 'static>(&self, permissions: &[&str], origin: Option<&str>, done: F) {
        let list = Value::Array(permissions.iter().map(|permission| (*permission).into()).collect());
        self.command("BrowserContext.grantPermissions", Value::object([("permissions", list), ("origin", origin.into())]), unit(done));
    }

    pub fn clear_permissions<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.command("BrowserContext.clearPermissions", Value::Null, unit(done));
    }

    pub fn set_geolocation<F: FnOnce(Result<(), Error>) + 'static>(&self, geolocation: Value, done: F) {
        self.command("BrowserContext.setGeolocation", Value::object([("geolocation", geolocation)]), unit(done));
    }

    pub fn set_http_credentials<F: FnOnce(Result<(), Error>) + 'static>(&self, credentials: Value, done: F) {
        self.command("BrowserContext.setHTTPCredentials", Value::object([("httpCredentials", credentials)]), unit(done));
    }

    pub fn set_offline<F: FnOnce(Result<(), Error>) + 'static>(&self, offline: bool, done: F) {
        self.command("BrowserContext.setOffline", Value::object([("offline", offline.into())]), unit(done));
    }

    pub fn set_extra_http_headers<F: FnOnce(Result<(), Error>) + 'static>(&self, headers: &[(&str, &str)], done: F) {
        self.command("BrowserContext.setExtraHTTPHeaders", Value::object([("headers", header_list(headers))]), unit(done));
    }

    pub fn storage_state<F: FnOnce(Result<Value, Error>) + 'static>(&self, done: F) {
        self.command("BrowserContext.storageState", Value::Null, done);
    }

    /// Closes the context and its pages.
    pub fn close<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        let pages = self.pages.clone();
        self.command(
            "BrowserContext.close",
            Value::Null,
            then(done, move |_| {
                pages.borrow_mut().clear();
                Ok(())
            }),
        );
    }
}

// ---- expect ----------------------------------------------------------------

/// What [`expect`] checks: a locator's elements, or a page.
#[derive(Clone)]
enum Target {
    Locator(Locator),
    Frame(Frame),
}

/// Playwright's assertions, retried in the page until they hold or their
/// timeout (5 s unless [`Expect::timeout`]) passes; a failed one is an
/// [`Error`] saying what was received.
#[derive(Clone)]
pub struct Expect {
    target: Target,
    not: bool,
    timeout: f64,
    message: Option<String>,
}

/// `expect(locator)`.
pub fn expect(locator: &Locator) -> Expect {
    Expect { target: Target::Locator(locator.clone()), not: false, timeout: 5000.0, message: None }
}

/// `expect(page)`: its title and URL.
pub fn expect_page(page: &Page) -> Expect {
    Expect { target: Target::Frame(page.main_frame()), not: false, timeout: 5000.0, message: None }
}

impl Expect {
    /// The opposite assertion.
    pub fn not(&self) -> Expect {
        Expect { not: !self.not, ..self.clone() }
    }

    pub fn timeout(&self, milliseconds: f64) -> Expect {
        Expect { timeout: milliseconds, ..self.clone() }
    }

    /// What a failure says instead.
    pub fn message(&self, message: &str) -> Expect {
        Expect { message: Some(message.to_string()), ..self.clone() }
    }

    /// `Frame.expect` with `expression` ("to.have.text", "to.be.visible",
    /// ...) and its `options` (the protocol's: expectedText, expectedNumber, ...).
    pub fn check<F: FnOnce(Result<(), Error>) + 'static>(&self, expression: &str, options: Value, done: F) {
        let mut params = object_or_empty(options);
        params.set("expression", expression.into());
        params.set("isNot", self.not.into());
        params.set("timeout", self.timeout.into());
        let (not, message, expression) = (self.not, self.message.clone(), expression.to_string());
        let shown = match &self.target {
            Target::Locator(locator) => locator.selector.clone(),
            Target::Frame(_) => "page".to_string(),
        };
        let finish = then(done, move |answer| {
            if answer["matches"].as_bool() != Some(not) {
                return Ok(());
            }
            Err(Error::new(message.unwrap_or_else(|| {
                format!(
                    "expect({shown}).{}{expression} failed{}: received {} {}",
                    if not { "not." } else { "" },
                    if answer["timedOut"].as_bool() == Some(true) { " (timed out)" } else { "" },
                    answer["received"],
                    answer["errorMessage"].as_str().unwrap_or_default()
                )
            })))
        });
        match &self.target {
            Target::Locator(locator) => {
                params.set("selector", locator.selector.as_str().into());
                locator.frame.command("Frame.expect", params, finish);
            }
            Target::Frame(frame) => frame.command("Frame.expect", params, finish),
        }
    }

    fn texts(&self, expression: &str, expected: &[Text], substring: Option<bool>, normalize: Option<bool>, done: impl FnOnce(Result<(), Error>) + 'static, more: Value) {
        let mut options = object_or_empty(more);
        options.set("expectedText", Value::Array(expected.iter().map(|text| text.expected(substring, normalize, None)).collect()));
        self.check(expression, options, done);
    }

    pub fn to_have_text<F: FnOnce(Result<(), Error>) + 'static>(&self, expected: impl Into<Text>, done: F) {
        self.texts("to.have.text", &[expected.into()], Some(false), Some(true), done, Value::Null);
    }

    /// The texts of each element it matches, in order.
    pub fn to_have_texts<F: FnOnce(Result<(), Error>) + 'static>(&self, expected: &[Text], done: F) {
        self.texts("to.have.text.array", expected, Some(false), Some(true), done, Value::Null);
    }

    pub fn to_contain_text<F: FnOnce(Result<(), Error>) + 'static>(&self, expected: impl Into<Text>, done: F) {
        self.texts("to.have.text", &[expected.into()], Some(true), Some(true), done, Value::Null);
    }

    pub fn to_have_value<F: FnOnce(Result<(), Error>) + 'static>(&self, value: impl Into<Text>, done: F) {
        self.texts("to.have.value", &[value.into()], None, None, done, Value::Null);
    }

    pub fn to_have_attribute<F: FnOnce(Result<(), Error>) + 'static>(&self, name: &str, value: impl Into<Text>, done: F) {
        self.texts("to.have.attribute.value", &[value.into()], None, None, done, Value::object([("expressionArg", name.into())]));
    }

    pub fn to_have_class<F: FnOnce(Result<(), Error>) + 'static>(&self, expected: impl Into<Text>, done: F) {
        self.texts("to.have.class", &[expected.into()], None, None, done, Value::Null);
    }

    pub fn to_have_count<F: FnOnce(Result<(), Error>) + 'static>(&self, count: usize, done: F) {
        self.check("to.have.count", Value::object([("expectedNumber", count.into())]), done);
    }

    pub fn to_have_title<F: FnOnce(Result<(), Error>) + 'static>(&self, title: impl Into<Text>, done: F) {
        self.texts("to.have.title", &[title.into()], None, Some(true), done, Value::Null);
    }

    pub fn to_have_url<F: FnOnce(Result<(), Error>) + 'static>(&self, url: impl Into<Text>, done: F) {
        self.texts("to.have.url", &[url.into()], None, None, done, Value::Null);
    }

    pub fn to_match_aria_snapshot<F: FnOnce(Result<(), Error>) + 'static>(&self, expected: &str, done: F) {
        self.check("to.match.aria", Value::object([("expectedValue", expected.into())]), done);
    }

    pub fn to_be_visible<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.visible", Value::Null, done);
    }

    pub fn to_be_hidden<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.hidden", Value::Null, done);
    }

    pub fn to_be_enabled<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.enabled", Value::Null, done);
    }

    pub fn to_be_disabled<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.disabled", Value::Null, done);
    }

    pub fn to_be_checked<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.checked", Value::Null, done);
    }

    pub fn to_be_editable<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.editable", Value::Null, done);
    }

    pub fn to_be_focused<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.focused", Value::Null, done);
    }

    pub fn to_be_empty<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.empty", Value::Null, done);
    }

    pub fn to_be_attached<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.attached", Value::Null, done);
    }

    pub fn to_be_in_viewport<F: FnOnce(Result<(), Error>) + 'static>(&self, done: F) {
        self.check("to.be.in.viewport", Value::Null, done);
    }
}

#[cfg(test)]
mod tests {
    use super::{base64_decode, base64_encode, looks_like_function, Glob};

    #[test]
    fn globs_match_as_playwright_s() {
        let cases = [
            ("**/data.json", "http://127.0.0.1:8000/data.json", true),
            ("**/data.json", "http://127.0.0.1:8000/a/b/data.json", true),
            ("**/data.json", "http://127.0.0.1:8000/data.jsonp", false),
            ("http://a.test/*.js", "http://a.test/x.js", true),
            ("http://a.test/*.js", "http://a.test/b/x.js", false),
            ("**/*.{png,jpg}", "https://x.test/i/a.jpg", true),
            ("**/*.{png,jpg}", "https://x.test/i/a.gif", false),
            ("**", "anything at all", true),
            ("https://x.test/a?b", "https://x.test/a?b", true),
            ("https://x.test/a?b", "https://x.test/aXb", false),
            ("https://x.test/\\*", "https://x.test/*", true),
            ("https://x.test/**/z", "https://x.test/z", true),
            ("https://x.test/**/z", "https://x.test/yz", false),
            ("https://x.test/**/z", "https://x.test/y/z", true),
        ];
        for (glob, url, expected) in cases {
            assert_eq!(Glob::new(glob).matches(url), expected, "{glob} {url}");
        }
    }

    #[test]
    fn base64_both_ways() {
        for text in [&b""[..], b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar", &[0, 255, 128]] {
            assert_eq!(base64_decode(&base64_encode(text)).unwrap(), text);
        }
        assert_eq!(base64_encode(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64_encode(b"fo"), "Zm8=");
    }

    #[test]
    fn functions_are_told_from_expressions() {
        assert!(looks_like_function("(b) => b.tagName"));
        assert!(looks_like_function("es => es.map(e => e)"));
        assert!(looks_like_function("async () => 1"));
        assert!(looks_like_function("function () { return 1 }"));
        assert!(!looks_like_function("document.title"));
        assert!(!looks_like_function("1 + 2"));
        assert!(!looks_like_function("fetch('a').then(r => r.json())"));
    }
}
