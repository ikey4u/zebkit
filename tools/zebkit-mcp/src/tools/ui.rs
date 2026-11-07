//! Native windows: `zebkit.ui`'s tree read, clicked, typed into and captured, as
//! pages are, in a script of the engine's. The agent's data reaches the script
//! only as a JSON literal, to functions of ui_bridge.js; it runs no code there.

use super::{image_output, ok, text, Done, Run, Tool};
use crate::bounds::Bounds;
use crate::schema::{described, integer, long_string, object, object_free, string};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::rc::Rc;
use zebkit::Script;

thread_local! {
    static SCRIPT: RefCell<Option<Rc<Script>>> = const { RefCell::new(None) };
    static NEXT_CAPTURE: std::cell::Cell<u64> = const { std::cell::Cell::new(0) };
}

const BRIDGE: &str = include_str!("ui_bridge.js");
/// Evaluate gives what a script's last statement is, not what a promise comes to:
/// a capture is kept on the script's global, and read back when it has settled.
const CAPTURE_POLL_MS: u64 = 20;

fn with_script(function: impl FnOnce(&Script)) {
    // Not borrowed while it runs: the script may call back at once, and ask again.
    let script = SCRIPT.with(|slot| {
        slot.borrow_mut()
            .get_or_insert_with(|| {
                let script = Script::new(&[]);
                // Standard output is the protocol's: the script's console writes to
                // standard error, on Windows too, where the engine's own descriptor
                // for standard output is still the protocol's.
                let console = "globalThis.console = Object.freeze({...console, log: console.error, info: console.error, debug: console.error})";
                script.evaluate(&format!("{console}; globalThis.__bridge = {BRIDGE}; null"), |_| {});
                Rc::new(script)
            })
            .clone()
    });
    function(&script);
}

/// `__bridge.<call>(args...)`, with its value as JSON.
fn bridge(call: &str, args: &[Value], done: impl FnOnce(Result<Value, String>) + 'static) {
    // Through JSON.parse: as a literal, a "__proto__" key would mean something.
    let data = Value::String(Value::Array(args.to_vec()).to_string());
    let source = format!("__bridge.{call}(...JSON.parse({data}))");
    with_script(move |script| {
        script.evaluate(&source, move |result| match result {
            Ok(json) => done(Ok(serde_json::from_str(&json).unwrap_or(Value::Null))),
            Err(error) => done(Err(first_line(error.message()))),
        });
    });
}

/// What was thrown, without the stack.
fn first_line(message: &str) -> String {
    message.lines().next().unwrap_or("").trim_start_matches("Error: ").chars().take(1000).collect()
}

fn answer(done: Done) -> impl FnOnce(Result<Value, String>) + 'static {
    move |result| done(result.and_then(ok))
}

fn ui_open(_: &'static Bounds, args: Value, done: Done) {
    bridge("open", &[args], answer(done));
}

fn ui_tree(_: &'static Bounds, args: Value, done: Done) {
    match args["window"].as_str() {
        Some(window) => bridge("tree", &[json!(window)], answer(done)),
        None => bridge("list", &[], answer(done)),
    }
}

fn ui_click(_: &'static Bounds, args: Value, done: Done) {
    bridge("click", &[args["window"].clone(), args["node"].clone()], answer(done));
}

fn ui_type(_: &'static Bounds, args: Value, done: Done) {
    bridge("type", &[args["window"].clone(), args["node"].clone(), args["text"].clone()], answer(done));
}

fn ui_submit(_: &'static Bounds, args: Value, done: Done) {
    bridge("submit", &[args["window"].clone(), args["node"].clone()], answer(done));
}

fn ui_events(_: &'static Bounds, args: Value, done: Done) {
    bridge("events", &[json!(text(&args, "window"))], answer(done));
}

fn ui_close(_: &'static Bounds, args: Value, done: Done) {
    bridge("close", &[json!(text(&args, "window"))], answer(done));
}

/// A capture is a promise, and a script's evaluation gives a value at once: it is
/// started, kept on the script's global, and asked after.
fn ui_capture(_: &'static Bounds, args: Value, done: Done) {
    // A number or null, and a string as JSON: nothing of it can be code.
    let node = args["node"].as_u64().map_or("null".to_string(), |node| node.to_string());
    let window = Value::String(text(&args, "window").to_string());
    // Each its own slot: requests come at once, for different windows.
    let slot = NEXT_CAPTURE.with(|next| {
        next.set(next.get() + 1);
        next.get()
    });
    let source = format!(
        "(globalThis.__captures ??= {{}}); __bridge.capture({window},{node}).then((value) => {{ __captures[{slot}] = value; }}, (error) => {{ __captures[{slot}] = {{error: String(error.message)}}; }}); true"
    );
    with_script(move |script| {
        script.evaluate(&source, move |result| match result {
            Err(error) => done(Err(first_line(error.message()))),
            Ok(_) => poll_capture(done, slot, 500),
        });
    });
}

fn poll_capture(done: Done, slot: u64, tries: u32) {
    if tries == 0 {
        with_script(move |script| script.evaluate(&format!("delete __captures[{slot}]; null"), |_| {}));
        return done(Err("the capture did not finish".to_string()));
    }
    with_script(move |script| {
        script.evaluate(&format!("(() => {{ const value = __captures[{slot}] ?? null; delete __captures[{slot}]; return value; }})()"), move |result| {
            let value: Value = result.ok().and_then(|json| serde_json::from_str(&json).ok()).unwrap_or(Value::Null);
            if let Some(why) = value["error"].as_str() {
                return done(Err(why.to_string()));
            }
            match value["base64"].as_str() {
                Some(data) => done(Ok(image_output(data.to_string(), "image/png", json!({"mimeType": "image/png"})))),
                None => zebkit::after(std::time::Duration::from_millis(CAPTURE_POLL_MS), move || poll_capture(done, slot, tries - 1)),
            }
        });
    });
}

fn tool(name: &'static str, description: &'static str, read_only: bool, destructive: bool, schema: Value, run: fn(&'static Bounds, Value, Done)) -> Tool {
    Tool { name, group: "ui", description, schema, read_only, destructive, run: Run::Engine(run) }
}

pub fn tools() -> Vec<Tool> {
    let window = || ("window", described(string(), "A window, from ui_open"));
    let node = || ("node", described(integer(0, 1 << 40), "A node's id, from the tree"));
    vec![
        tool("ui_open", "Open a native window of the engine's toolkit, to show something to a person or take an answer. content is a tree of {type: view, text, button, input, image or scroll, name, class (a subset of Tailwind), children: [...]} with text as plain strings; no web elements. Without --windows it has no window on screen, and can still be driven and captured.", false, false,
            object(&[("title", string()), ("width", integer(100, 8192)), ("height", integer(100, 8192)), ("content", object_free())], &["content"]), ui_open),
        tool("ui_tree", "A window's tree as it is laid out: ids, types, text, values, bounds. Without window, the windows open.", true, false,
            object(&[window()], &[]), ui_tree),
        tool("ui_click", "Click a button in a window.", false, false, object(&[window(), node()], &["window", "node"]), ui_click),
        tool("ui_type", "Type into an input of a window, after what it holds.", false, false,
            object(&[window(), node(), ("text", long_string(1_000_000))], &["window", "node", "text"]), ui_type),
        tool("ui_submit", "Submit an input, as Enter does.", false, false, object(&[window(), node()], &["window", "node"]), ui_submit),
        tool("ui_events", "What happened in a window since last asked, a person's or an agent's: clicks, input and submits, by the name given to the element.", true, false,
            object(&[window()], &["window"]), ui_events),
        tool("ui_capture", "A window or a node as PNG, as the compositor shows it.", true, false, object(&[window(), node()], &["window"]), ui_capture),
        tool("ui_close", "Close a window.", false, true, object(&[window()], &["window"]), ui_close),
    ]
}
