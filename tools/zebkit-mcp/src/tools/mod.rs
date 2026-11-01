//! The table of tools. A group is a module with a `tools()` function, with `--tools`
//! and the checks of arguments working for it as they do for the others.

use crate::bounds::Bounds;
use serde_json::{json, Value};
use std::sync::OnceLock;

pub mod auth;
pub mod files;
pub mod media;
pub mod pages;
pub mod pdf;
pub mod settings;
pub mod ui;

pub struct Output {
    pub content: Vec<Value>,
    pub structured: Option<Value>,
}
/// What a tool gives, or why not: an error the agent reads.
pub type Outcome = Result<Output, String>;
pub type Done = Box<dyn FnOnce(Outcome) + Send>;

pub enum Run {
    /// Blocking, on the request's thread.
    Worker(fn(&'static Bounds, &Value) -> Outcome),
    /// On the engine's thread: must not block, and calls `Done` once.
    Engine(fn(&'static Bounds, Value, Done)),
}

pub struct Tool {
    pub name: &'static str,
    pub group: &'static str,
    pub description: &'static str,
    pub schema: Value,
    pub read_only: bool,
    pub destructive: bool,
    pub run: Run,
}

/// Every tool of this build.
pub fn all() -> &'static [Tool] {
    static TOOLS: OnceLock<Vec<Tool>> = OnceLock::new();
    TOOLS.get_or_init(|| {
        let mut tools = pages::tools();
        tools.extend(pdf::tools());
        tools.extend(files::tools());
        tools.extend(media::tools());
        tools.extend(ui::tools());
        tools.extend(auth::tools());
        tools.extend(settings::tools());
        for tool in &tools {
            if let Err(why) = crate::schema::check_schema(&tool.schema) {
                panic!("{}: {why}", tool.name);
            }
        }
        tools
    })
}

pub fn find(name: &str) -> Option<&'static Tool> {
    all().iter().find(|tool| tool.name == name)
}

pub fn describe(tool: &Tool) -> Value {
    json!({
        "name": tool.name,
        "description": tool.description,
        "inputSchema": tool.schema,
        "annotations": {
            "readOnlyHint": tool.read_only,
            "destructiveHint": tool.destructive,
            "openWorldHint": matches!(tool.group, "pages" | "auth") || tool.name == "net_fetch",
        },
    })
}

/// What this server offers: what its bounds allow.
pub fn listed() -> Vec<Value> {
    all().iter().filter(|tool| crate::bounds().tools.contains(tool.name)).map(describe).collect()
}

/// A result as JSON: readable as text, and structured.
pub fn json_output(value: Value) -> Output {
    let text = serde_json::to_string(&value).unwrap_or_default();
    let structured = value.is_object().then_some(value);
    Output { content: vec![json!({"type": "text", "text": text})], structured }
}

pub fn ok(value: Value) -> Outcome {
    Ok(json_output(value))
}

pub fn image_output(base64: String, mime: &str, details: Value) -> Output {
    Output {
        content: vec![json!({"type": "image", "data": base64, "mimeType": mime}), json!({"type": "text", "text": details.to_string()})],
        structured: details.is_object().then_some(details),
    }
}

/// A required string the schema has checked.
pub fn text<'a>(args: &'a Value, name: &str) -> &'a str {
    args[name].as_str().unwrap_or("")
}

const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn base64_encode(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let number = (chunk[0] as u32) << 16 | (*chunk.get(1).unwrap_or(&0) as u32) << 8 | *chunk.get(2).unwrap_or(&0) as u32;
        out.push(ALPHABET[(number >> 18) as usize & 63] as char);
        out.push(ALPHABET[(number >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 { ALPHABET[(number >> 6) as usize & 63] as char } else { '=' });
        out.push(if chunk.len() > 2 { ALPHABET[number as usize & 63] as char } else { '=' });
    }
    out
}

pub fn base64_decode(text: &str) -> Result<Vec<u8>, String> {
    let clean = text.trim_end_matches('=');
    let mut out = Vec::with_capacity(clean.len() * 3 / 4);
    let (mut buffer, mut bits) = (0u32, 0);
    for byte in clean.bytes() {
        let value = match byte {
            b'A'..=b'Z' => byte - b'A',
            b'a'..=b'z' => byte - b'a' + 26,
            b'0'..=b'9' => byte - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            _ => return Err("not base64".to_string()),
        };
        buffer = buffer << 6 | value as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((buffer >> bits) as u8);
            buffer &= (1 << bits) - 1;
        }
    }
    Ok(out)
}
