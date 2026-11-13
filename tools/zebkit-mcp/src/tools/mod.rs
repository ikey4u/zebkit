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

/// How deep a value the server passes either way may be nested: serde_json's own
/// limit, and the binding's reader's. What is deeper is an error, never `null`.
const DEPTH: usize = 128;

/// The largest integer a JSON number (an `f64`, as in JavaScript) holds exactly.
const EXACT: u64 = 1 << 53;

/// A value of the server's JSON as the binding's (automation commands take it),
/// converted member by member. Numbers become `f64`, as the engine reads them:
/// an integer beyond 2^53, which would arrive as another, is an error.
pub fn to_engine(value: &Value) -> Result<zebkit::json::Value, String> {
    fn convert(value: &Value, depth: usize) -> Result<zebkit::json::Value, String> {
        use zebkit::json::Value as Engine;
        if depth > DEPTH {
            return Err(format!("a value nested deeper than {DEPTH} levels"));
        }
        Ok(match value {
            Value::Null => Engine::Null,
            Value::Bool(value) => Engine::Bool(*value),
            Value::Number(number) => {
                let exact = number.as_u64().map(|n| n <= EXACT).or_else(|| number.as_i64().map(|n| n.unsigned_abs() <= EXACT)).unwrap_or(true);
                if !exact {
                    return Err(format!("{number}: an integer beyond 2^53, which JavaScript cannot hold exactly"));
                }
                Engine::Number(number.as_f64().ok_or_else(|| format!("{number}: not a number the engine can read"))?)
            }
            Value::String(text) => Engine::String(text.clone()),
            Value::Array(items) => Engine::Array(items.iter().map(|item| convert(item, depth + 1)).collect::<Result<_, _>>()?),
            Value::Object(members) => Engine::Object(
                members.iter().map(|(key, item)| Ok((key.clone(), convert(item, depth + 1)?))).collect::<Result<_, String>>()?,
            ),
        })
    }
    convert(value, 1)
}

/// A value of the binding's JSON as the server's, converted member by member.
/// An integral number is an integer, exactly the `f64` the engine gave; one
/// not finite is `null`, as JSON.stringify writes it. A member named twice
/// keeps its last value, as in JavaScript. Deeper than 128 levels is an error.
pub fn from_engine(value: &zebkit::json::Value) -> Result<Value, String> {
    fn convert(value: &zebkit::json::Value, depth: usize) -> Result<Value, String> {
        use zebkit::json::Value as Engine;
        if depth > DEPTH {
            return Err(format!("the engine's result is nested deeper than {DEPTH} levels"));
        }
        Ok(match value {
            Engine::Null => Value::Null,
            Engine::Bool(value) => Value::Bool(*value),
            // Every integral f64 of this range is an i64 exactly.
            Engine::Number(number) if number.fract() == 0.0 && number.abs() < 9.0e18 => Value::from(*number as i64),
            Engine::Number(number) => serde_json::Number::from_f64(*number).map_or(Value::Null, Value::Number),
            Engine::String(text) => Value::String(text.clone()),
            Engine::Array(items) => Value::Array(items.iter().map(|item| convert(item, depth + 1)).collect::<Result<_, _>>()?),
            Engine::Object(members) => {
                let mut object = serde_json::Map::new();
                for (key, item) in members {
                    object.insert(key.clone(), convert(item, depth + 1)?);
                }
                Value::Object(object)
            }
        })
    }
    convert(value, 1)
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

#[cfg(test)]
mod tests {
    use super::*;

    fn nested(depth: usize) -> String {
        format!("{}{}", "[".repeat(depth), "]".repeat(depth))
    }

    #[test]
    fn conversions() {
        let value = json!({"a": [1, -2, 2.5, true, null, "x\u{1f600}"], "b": {"c": 9007199254740992u64}});
        assert_eq!(from_engine(&to_engine(&value).unwrap()).unwrap(), value);
        // Integers: exact to 2^53 either way; beyond, refused on the way in, and
        // on the way out exactly the f64 the engine holds, not a float's text.
        assert!(to_engine(&json!(9007199254740993u64)).is_err());
        assert!(to_engine(&json!(-9007199254740993i64)).is_err());
        assert!(to_engine(&json!(u64::MAX)).is_err());
        let big = zebkit::json::Value::Number(2f64.powi(60));
        assert_eq!(from_engine(&big).unwrap(), json!(1152921504606846976i64));
        assert_eq!(from_engine(&zebkit::json::Value::Number(3.0)).unwrap().to_string(), "3");
        assert_eq!(from_engine(&zebkit::json::Value::Number(f64::INFINITY)).unwrap(), Value::Null);
        assert_eq!(from_engine(&zebkit::json::Value::Number(1e300)).unwrap(), json!(1e300));
        // A member named twice: the last, as in JavaScript.
        let twice = zebkit::json::Value::Object(vec![("k".into(), 1.into()), ("k".into(), 2.into())]);
        assert_eq!(from_engine(&twice).unwrap(), json!({"k": 2}));
        // Nesting: 128 levels pass, deeper is an error, never null.
        let deep: Value = serde_json::from_str(&nested(127)).unwrap();
        assert!(to_engine(&deep).is_ok());
        let mut engine = zebkit::json::Value::Array(vec![]);
        for _ in 0..127 {
            engine = zebkit::json::Value::Array(vec![engine]);
        }
        assert!(from_engine(&engine).is_ok());
        let engine = zebkit::json::Value::Array(vec![zebkit::json::Value::Array(vec![engine])]);
        assert!(from_engine(&engine).unwrap_err().contains("deeper than 128"));
    }
}
