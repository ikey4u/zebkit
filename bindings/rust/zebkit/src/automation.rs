//! Playwright's API for a view's page, natively (`<zebkit/automation.h>`,
//! docs/AUTOMATION.md): each command is one of Playwright's protocol, run
//! inside the engine without CDP or a driver process.
//!
//! ```no_run
//! use zebkit::automation::Page;
//! use zebkit::View;
//!
//! std::process::exit(zebkit::run(|| {
//!     let page = Page::new(View::new(None, 1280, 720));
//!     let next = page.clone();
//!     page.goto("https://example.com/", move |_| {
//!         next.get_by_role("link", Some("More information")).click(|result| {
//!             println!("clicked: {result:?}");
//!             zebkit::quit();
//!         });
//!     });
//! }));
//! ```
//!
//! Results are JSON (as the protocol has them, with zebkit's differences:
//! plain JSON values, handles as `{"handle": "h3", ...}`); the commands
//! the engine has are [`commands`].

use crate::{c_string, script_result, sys, take_string, Error, View};
use std::cell::RefCell;
use std::collections::HashMap;
use std::os::raw::{c_char, c_void};
use std::rc::Rc;

/// The commands the engine has, as a JSON array of their names.
pub fn commands() -> String {
    unsafe { take_string(sys::zebkit_automation_commands()) }.unwrap_or_else(|| "[]".to_string())
}

/// Runs Playwright's command `method` on `view`'s page with `params` (a
/// JSON object) and calls `done` with its result as JSON or the error.
pub fn call<F: FnOnce(Result<String, Error>) + 'static>(view: &View, method: &str, params: &str, done: F) {
    let Some(raw) = view.raw() else {
        done(Err(Error::new("the view is closed")));
        return;
    };
    let data = Box::into_raw(Box::new(done));
    unsafe {
        sys::zebkit_automation_call(
            raw,
            c_string(method).as_ptr(),
            c_string(params).as_ptr(),
            Some(on_done::<F>),
            data.cast(),
        )
    }
}

unsafe extern "C" fn on_done<F: FnOnce(Result<String, Error>)>(
    _view: *mut sys::zebkit_view,
    json: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    done(script_result(json, error));
}

type EventFunction = Box<dyn FnMut(&str, &str)>;

thread_local! {
    static EVENTS: RefCell<HashMap<usize, Rc<RefCell<EventFunction>>>> = RefCell::new(HashMap::new());
}

/// Calls `function(event, json)` for the page's events, as Playwright's
/// protocol names them ("Page.request", "BrowserContext.dialog", ...).
pub fn on_event<F: FnMut(&str, &str) + 'static>(view: &View, function: F) {
    let Some(raw) = view.raw() else { return };
    EVENTS.with(|events| {
        events.borrow_mut().insert(raw as usize, Rc::new(RefCell::new(Box::new(function))));
    });
    unsafe { sys::zebkit_automation_on_event(raw, Some(on_event_raw), std::ptr::null_mut()) }
}

unsafe extern "C" fn on_event_raw(
    view: *mut sys::zebkit_view,
    event: *const c_char,
    json: *const c_char,
    _data: *mut c_void,
) {
    let Some(function) = EVENTS.with(|events| events.borrow().get(&(view as usize)).cloned()) else { return };
    let event = crate::string_from(event).unwrap_or_default();
    let json = crate::string_from(json).unwrap_or_else(|| "{}".to_string());
    if let Ok(mut function) = function.try_borrow_mut() {
        function(&event, &json);
    };
}

/// Runs a command that names no view: "Browser.newContext" and a
/// context's ("BrowserContext.*" with its "context", "APIRequestContext.fetch").
pub fn call_global<F: FnOnce(Result<String, Error>) + 'static>(method: &str, params: &str, done: F) {
    let data = Box::into_raw(Box::new(done));
    unsafe {
        sys::zebkit_automation_call(
            std::ptr::null_mut(),
            c_string(method).as_ptr(),
            c_string(params).as_ptr(),
            Some(on_done::<F>),
            data.cast(),
        )
    }
}

/// A context of its own (`Browser.newContext`): its own profile, cookies,
/// storage, permissions and cache.
#[derive(Clone)]
pub struct BrowserContext {
    id: String,
}

impl BrowserContext {
    /// A new context with Playwright's options as a JSON object
    /// (`{"viewport": {...}, "userAgent": ..., "geolocation": ...}`).
    pub fn new<F: FnOnce(Result<BrowserContext, Error>) + 'static>(options: &str, done: F) {
        call_global("Browser.newContext", options, move |result| {
            done(result.and_then(|json| {
                string_member(&json, "context")
                    .map(|id| BrowserContext { id })
                    .ok_or_else(|| Error::new("no context"))
            }))
        });
    }

    /// Its id ("x1").
    pub fn id(&self) -> &str {
        &self.id
    }

    /// A new page of the context: an off-screen view under --headless.
    pub fn new_page<F: FnOnce(Result<Page, Error>) + 'static>(&self, done: F) {
        self.command("BrowserContext.newPage", "", move |result| {
            done(result.and_then(|json| {
                let id = string_member(&json, "page").ok_or_else(|| Error::new("no page"))?;
                let raw = unsafe { sys::zebkit_automation_page(c_string(&id).as_ptr()) };
                if raw.is_null() {
                    return Err(Error::new("the page is closed"));
                }
                Ok(Page::new(View::wrap(raw)))
            }))
        });
    }

    /// One of the context's commands ("BrowserContext.cookies", ...);
    /// `members` are more of its params, as JSON object members.
    pub fn command<F: FnOnce(Result<String, Error>) + 'static>(&self, method: &str, members: &str, done: F) {
        let params = if members.is_empty() {
            format!("{{\"context\": {}}}", json_string(&self.id))
        } else {
            format!("{{\"context\": {}, {members}}}", json_string(&self.id))
        };
        call_global(method, &params, done);
    }

    pub fn close<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("BrowserContext.close", "", done);
    }
}

fn string_member(json: &str, key: &str) -> Option<String> {
    let pattern = format!("\"{key}\":\"");
    let start = json.find(&pattern)? + pattern.len();
    let end = json[start..].find('"')? + start;
    Some(json[start..end].to_string())
}

/// Serves Playwright's own protocol on 127.0.0.1:`port` (0 picks one), what
/// `chromium.connect(url)` of each Playwright client speaks; `done` gets
/// the url (`ws://127.0.0.1:<port>/<random token>`), the credential to connect
/// with: only that path connects, and no request with an `Origin` header.
pub fn listen<F: FnOnce(Result<String, Error>) + 'static>(port: u16, done: F) {
    let data = Box::into_raw(Box::new(done));
    unsafe { sys::zebkit_automation_listen(port as i32, Some(on_listen::<F>), data.cast()) }
}

unsafe extern "C" fn on_listen<F: FnOnce(Result<String, Error>)>(
    url: *const c_char,
    error: *const c_char,
    data: *mut c_void,
) {
    let done = Box::from_raw(data.cast::<F>());
    match crate::string_from(url) {
        Some(url) => done(Ok(url)),
        None => done(Err(Error::new(crate::string_from(error).unwrap_or_default()))),
    }
}

/// `text` as a JSON string.
pub fn json_string(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

// ---- Selectors, as Playwright's locatorUtils.ts makes them ---------------

fn text_selector(text: &str, exact: bool) -> String {
    format!("{}{}", json_string(text), if exact { "s" } else { "i" })
}

fn attribute_selector(value: &str, exact: bool) -> String {
    format!("\"{}\"{}", value.replace('\\', "\\\\").replace('"', "\\\""), if exact { "s" } else { "i" })
}

/// A page: a view and its main frame.
#[derive(Clone)]
pub struct Page {
    view: View,
}

impl Page {
    pub fn new(view: View) -> Page {
        Page { view }
    }

    pub fn view(&self) -> &View {
        &self.view
    }

    pub fn main_frame(&self) -> Frame {
        Frame { page: self.clone(), id: None }
    }

    /// A frame by its id (from `Page.frames` or an event).
    pub fn frame(&self, id: &str) -> Frame {
        Frame { page: self.clone(), id: Some(id.to_string()) }
    }

    pub fn command<F: FnOnce(Result<String, Error>) + 'static>(&self, method: &str, params: &str, done: F) {
        call(&self.view, method, params, done);
    }

    pub fn on_event<F: FnMut(&str, &str) + 'static>(&self, function: F) {
        on_event(&self.view, function);
    }

    pub fn goto<F: FnOnce(Result<String, Error>) + 'static>(&self, url: &str, done: F) {
        self.main_frame().goto(url, done);
    }

    pub fn locator(&self, selector: &str) -> Locator {
        self.main_frame().locator(selector)
    }

    pub fn get_by_role(&self, role: &str, name: Option<&str>) -> Locator {
        self.main_frame().get_by_role(role, name)
    }

    pub fn get_by_text(&self, text: &str, exact: bool) -> Locator {
        self.main_frame().get_by_text(text, exact)
    }

    pub fn get_by_test_id(&self, id: &str) -> Locator {
        self.main_frame().get_by_test_id(id)
    }

    pub fn evaluate<F: FnOnce(Result<String, Error>) + 'static>(&self, function: &str, arg: Option<&str>, done: F) {
        self.main_frame().evaluate(function, arg, done);
    }

    pub fn screenshot<F: FnOnce(Result<String, Error>) + 'static>(&self, path: &str, done: F) {
        self.command("Page.screenshot", &format!("{{\"path\":{}}}", json_string(path)), done);
    }

    pub fn press<F: FnOnce(Result<String, Error>) + 'static>(&self, key: &str, done: F) {
        self.command("Page.keyboardPress", &format!("{{\"key\":{}}}", json_string(key)), done);
    }

    pub fn type_text<F: FnOnce(Result<String, Error>) + 'static>(&self, text: &str, done: F) {
        self.command("Page.keyboardType", &format!("{{\"text\":{}}}", json_string(text)), done);
    }

    pub fn mouse_click<F: FnOnce(Result<String, Error>) + 'static>(&self, x: f64, y: f64, done: F) {
        self.command("Page.mouseClick", &format!("{{\"x\":{x},\"y\":{y}}}"), done);
    }
}

/// A frame of a page: its id, `None` for the main frame.
#[derive(Clone)]
pub struct Frame {
    page: Page,
    id: Option<String>,
}

impl Frame {
    /// `params` (a JSON object's members, without braces) with the frame.
    fn params(&self, members: &str) -> String {
        let frame = self.id.as_ref().map(|id| format!("\"frame\":{}", json_string(id)));
        match (frame, members.is_empty()) {
            (Some(frame), true) => format!("{{{frame}}}"),
            (Some(frame), false) => format!("{{{frame},{members}}}"),
            (None, _) => format!("{{{members}}}"),
        }
    }

    pub fn command<F: FnOnce(Result<String, Error>) + 'static>(&self, method: &str, members: &str, done: F) {
        self.page.command(method, &self.params(members), done);
    }

    pub fn goto<F: FnOnce(Result<String, Error>) + 'static>(&self, url: &str, done: F) {
        self.command("Frame.goto", &format!("\"url\":{}", json_string(url)), done);
    }

    pub fn evaluate<F: FnOnce(Result<String, Error>) + 'static>(&self, function: &str, arg: Option<&str>, done: F) {
        let arg = arg.map(|arg| format!(",\"arg\":{arg}")).unwrap_or_default();
        self.command(
            "Frame.evaluateExpression",
            &format!("\"expression\":{},\"isFunction\":true{arg}", json_string(function)),
            done,
        );
    }

    pub fn locator(&self, selector: &str) -> Locator {
        Locator { frame: self.clone(), selector: selector.to_string() }
    }

    pub fn get_by_role(&self, role: &str, name: Option<&str>) -> Locator {
        let name = name.map(|name| format!("[name={}]", attribute_selector(name, false))).unwrap_or_default();
        self.locator(&format!("internal:role={role}{name}"))
    }

    pub fn get_by_text(&self, text: &str, exact: bool) -> Locator {
        self.locator(&format!("internal:text={}", text_selector(text, exact)))
    }

    pub fn get_by_label(&self, text: &str, exact: bool) -> Locator {
        self.locator(&format!("internal:label={}", text_selector(text, exact)))
    }

    pub fn get_by_placeholder(&self, text: &str, exact: bool) -> Locator {
        self.locator(&format!("internal:attr=[placeholder={}]", attribute_selector(text, exact)))
    }

    pub fn get_by_test_id(&self, id: &str) -> Locator {
        self.locator(&format!("internal:testid=[data-testid={}]", attribute_selector(id, true)))
    }
}

/// Playwright's Locator: a selector in a frame, resolved at each use.
#[derive(Clone)]
pub struct Locator {
    frame: Frame,
    selector: String,
}

impl Locator {
    pub fn selector(&self) -> &str {
        &self.selector
    }

    pub fn locator(&self, selector: &str) -> Locator {
        Locator { frame: self.frame.clone(), selector: format!("{} >> {selector}", self.selector) }
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

    pub fn filter_has_text(&self, text: &str) -> Locator {
        self.locator(&format!("internal:has-text={}", text_selector(text, false)))
    }

    /// The `<iframe>` of this selector, as Playwright's FrameLocator.
    pub fn content_frame(&self) -> Locator {
        self.locator("internal:control=enter-frame")
    }

    /// Runs `method` with the selector, strictly, and `members` (a JSON
    /// object's members, without braces).
    pub fn command<F: FnOnce(Result<String, Error>) + 'static>(&self, method: &str, members: &str, done: F) {
        let selector = format!("\"selector\":{},\"strict\":true", json_string(&self.selector));
        let members = if members.is_empty() { selector } else { format!("{selector},{members}") };
        self.frame.command(method, &members, done);
    }

    pub fn click<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.click", "", done);
    }

    pub fn dblclick<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.dblclick", "", done);
    }

    pub fn hover<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.hover", "", done);
    }

    pub fn check<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.check", "", done);
    }

    pub fn uncheck<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.uncheck", "", done);
    }

    pub fn fill<F: FnOnce(Result<String, Error>) + 'static>(&self, value: &str, done: F) {
        self.command("Frame.fill", &format!("\"value\":{}", json_string(value)), done);
    }

    pub fn press<F: FnOnce(Result<String, Error>) + 'static>(&self, key: &str, done: F) {
        self.command("Frame.press", &format!("\"key\":{}", json_string(key)), done);
    }

    pub fn press_sequentially<F: FnOnce(Result<String, Error>) + 'static>(&self, text: &str, done: F) {
        self.command("Frame.type", &format!("\"text\":{}", json_string(text)), done);
    }

    pub fn select_option<F: FnOnce(Result<String, Error>) + 'static>(&self, value_or_label: &str, done: F) {
        self.command(
            "Frame.selectOption",
            &format!("\"options\":[{{\"valueOrLabel\":{}}}]", json_string(value_or_label)),
            done,
        );
    }

    pub fn set_input_files<F: FnOnce(Result<String, Error>) + 'static>(&self, paths: &[&str], done: F) {
        let list: Vec<String> = paths.iter().map(|path| json_string(path)).collect();
        self.command("Frame.setInputFiles", &format!("\"localPaths\":[{}]", list.join(",")), done);
    }

    pub fn text_content<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.textContent", "", done);
    }

    pub fn inner_text<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.innerText", "", done);
    }

    pub fn input_value<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.inputValue", "", done);
    }

    pub fn get_attribute<F: FnOnce(Result<String, Error>) + 'static>(&self, name: &str, done: F) {
        self.command("Frame.getAttribute", &format!("\"name\":{}", json_string(name)), done);
    }

    pub fn is_visible<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.isVisible", "", done);
    }

    pub fn count<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        let selector = format!("\"selector\":{}", json_string(&self.selector));
        self.frame.command("Frame.queryCount", &selector, done);
    }

    pub fn wait_for<F: FnOnce(Result<String, Error>) + 'static>(&self, state: &str, done: F) {
        self.command(
            "Frame.waitForSelector",
            &format!("\"state\":{},\"omitReturnValue\":true", json_string(state)),
            done,
        );
    }

    pub fn aria_snapshot<F: FnOnce(Result<String, Error>) + 'static>(&self, done: F) {
        self.command("Frame.ariaSnapshot", "", done);
    }

    /// Playwright's `expect(locator)`: `expression` is the protocol's
    /// ("to.have.text", "to.be.visible", ...), `members` its options.
    pub fn expect<F: FnOnce(Result<String, Error>) + 'static>(&self, expression: &str, members: &str, done: F) {
        let options = format!(
            "\"expression\":{},\"isNot\":false{}",
            json_string(expression),
            if members.is_empty() { String::new() } else { format!(",{members}") }
        );
        self.command("Frame.expect", &options, done);
    }
}
