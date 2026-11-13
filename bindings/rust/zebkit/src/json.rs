//! JSON values, for what the engine gives and takes as JSON: automation
//! results and events, a page's call arguments, open dialogs, find results.
//! A small reader and writer of the crate's own, so that it depends on
//! nothing; an application with serde_json can as well read the `*_json`
//! accessors' text.
//!
//! ```
//! use zebkit::json::Value;
//!
//! let value = Value::parse(r#"{"status": 200, "headers": [{"name": "a", "value": "b"}]}"#).unwrap();
//! assert_eq!(value["status"].as_i64(), Some(200));
//! assert_eq!(value["headers"][0]["name"].as_str(), Some("a"));
//! assert!(value["missing"].is_null());
//! assert_eq!(Value::object([("ok", true.into())]).to_string(), r#"{"ok":true}"#);
//! ```

use crate::Error;
use std::fmt;

/// A JSON value. Numbers are `f64`, as in JavaScript; an object keeps its
/// members in their order.
#[derive(Debug, Clone, PartialEq, Default)]
pub enum Value {
    #[default]
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Value>),
    Object(Vec<(String, Value)>),
}

static NULL: Value = Value::Null;

impl Value {
    /// Reads `text`, which must be one JSON value (whitespace around it
    /// allowed), strictly as RFC 8259 has it; a later duplicate member wins,
    /// as in JavaScript. Nested deeper than 128 is an error.
    pub fn parse(text: &str) -> Result<Value, Error> {
        let mut reader = Reader { bytes: text.as_bytes(), at: 0, depth: 0 };
        let value = reader.value()?;
        reader.space();
        if reader.at != reader.bytes.len() {
            return Err(reader.error("text after the value"));
        }
        Ok(value)
    }

    /// An object of `members`.
    pub fn object<K: Into<String>>(members: impl IntoIterator<Item = (K, Value)>) -> Value {
        Value::Object(members.into_iter().map(|(key, value)| (key.into(), value)).collect())
    }

    /// The member `key` of an object; `None` for another value or no such member.
    pub fn get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Object(members) => members.iter().find(|(name, _)| name == key).map(|(_, value)| value),
            _ => None,
        }
    }

    /// Sets the member `key` of an object (a `Null` becomes one); does
    /// nothing to another value.
    pub fn set(&mut self, key: &str, value: Value) {
        if self.is_null() {
            *self = Value::Object(Vec::new());
        }
        if let Value::Object(members) = self {
            match members.iter_mut().find(|(name, _)| name == key) {
                Some(member) => member.1 = value,
                None => members.push((key.to_string(), value)),
            }
        }
    }

    pub fn is_null(&self) -> bool {
        matches!(self, Value::Null)
    }

    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(value) => Some(*value),
            _ => None,
        }
    }

    pub fn as_f64(&self) -> Option<f64> {
        match self {
            Value::Number(value) => Some(*value),
            _ => None,
        }
    }

    /// A number that is a whole one.
    pub fn as_i64(&self) -> Option<i64> {
        self.as_f64().filter(|value| value.fract() == 0.0 && value.abs() < 9.007_199_254_740_992e15).map(|value| value as i64)
    }

    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::String(value) => Some(value),
            _ => None,
        }
    }

    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(values) => Some(values),
            _ => None,
        }
    }

    pub fn as_object(&self) -> Option<&[(String, Value)]> {
        match self {
            Value::Object(members) => Some(members),
            _ => None,
        }
    }

    /// The text of a string member, `None` when it is missing or not a string.
    pub(crate) fn text(&self, key: &str) -> Option<String> {
        self.get(key).and_then(Value::as_str).map(str::to_string)
    }

    /// The value as JSON text, compact.
    pub fn to_json(&self) -> String {
        self.to_string()
    }
}

impl fmt::Display for Value {
    fn fmt(&self, out: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Null => out.write_str("null"),
            Value::Bool(value) => write!(out, "{value}"),
            Value::Number(value) if !value.is_finite() => out.write_str("null"),
            Value::Number(value) if value.fract() == 0.0 && value.abs() < 1e17 => write!(out, "{}", *value as i64),
            Value::Number(value) => write!(out, "{value}"),
            Value::String(value) => out.write_str(&quote(value)),
            Value::Array(values) => {
                out.write_str("[")?;
                for (index, value) in values.iter().enumerate() {
                    if index > 0 {
                        out.write_str(",")?;
                    }
                    write!(out, "{value}")?;
                }
                out.write_str("]")
            }
            Value::Object(members) => {
                out.write_str("{")?;
                for (index, (name, value)) in members.iter().enumerate() {
                    if index > 0 {
                        out.write_str(",")?;
                    }
                    write!(out, "{}:{value}", quote(name))?;
                }
                out.write_str("}")
            }
        }
    }
}

impl std::ops::Index<&str> for Value {
    type Output = Value;
    /// The member, or `Null` when there is none.
    fn index(&self, key: &str) -> &Value {
        self.get(key).unwrap_or(&NULL)
    }
}

impl std::ops::Index<usize> for Value {
    type Output = Value;
    /// The element, or `Null` when there is none.
    fn index(&self, index: usize) -> &Value {
        self.as_array().and_then(|values| values.get(index)).unwrap_or(&NULL)
    }
}

impl From<bool> for Value {
    fn from(value: bool) -> Value {
        Value::Bool(value)
    }
}

macro_rules! numbers {
    ($($type:ty),*) => {$(
        impl From<$type> for Value {
            fn from(value: $type) -> Value {
                Value::Number(value as f64)
            }
        }
    )*};
}
numbers!(f64, f32, i64, i32, u64, u32, u16, usize);

impl From<&str> for Value {
    fn from(value: &str) -> Value {
        Value::String(value.to_string())
    }
}

impl From<String> for Value {
    fn from(value: String) -> Value {
        Value::String(value)
    }
}

impl From<Vec<Value>> for Value {
    fn from(values: Vec<Value>) -> Value {
        Value::Array(values)
    }
}

impl<T: Into<Value>> From<Option<T>> for Value {
    fn from(value: Option<T>) -> Value {
        value.map_or(Value::Null, Into::into)
    }
}

/// `text` as a JSON string.
pub fn quote(text: &str) -> String {
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

/// Nested deeper than this is refused rather than overflowing the stack
/// (serde_json's limit): a page's value may be read on a small-stack thread.
const DEPTH: usize = 128;

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
    depth: usize,
}

/// A strict RFC 8259 reader: what JavaScript's `JSON.parse` takes, and a
/// later duplicate member replaces the earlier one's value, as there.
impl Reader<'_> {
    fn error(&self, what: &str) -> Error {
        Error::new(format!("invalid JSON at byte {}: {what}", self.at))
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.at).copied()
    }

    fn space(&mut self) {
        while self.peek().is_some_and(|byte| matches!(byte, b' ' | b'\t' | b'\n' | b'\r')) {
            self.at += 1;
        }
    }

    fn literal(&mut self, word: &str, value: Value) -> Result<Value, Error> {
        if self.bytes[self.at..].starts_with(word.as_bytes()) {
            self.at += word.len();
            Ok(value)
        } else {
            Err(self.error("unknown word"))
        }
    }

    fn value(&mut self) -> Result<Value, Error> {
        self.space();
        match self.peek() {
            None => Err(self.error("no value")),
            Some(b'n') => self.literal("null", Value::Null),
            Some(b't') => self.literal("true", Value::Bool(true)),
            Some(b'f') => self.literal("false", Value::Bool(false)),
            Some(b'"') => self.string().map(Value::String),
            Some(b'[') => self.array(),
            Some(b'{') => self.object(),
            Some(b'-' | b'0'..=b'9') => self.number(),
            Some(_) => Err(self.error("not a value")),
        }
    }

    // Arrays and objects are functions of their own, kept out of value():
    // its frame is what each level of nesting costs on the stack.
    #[inline(never)]
    fn array(&mut self) -> Result<Value, Error> {
        self.nest()?;
        let mut values = Vec::new();
        self.space();
        if self.peek() == Some(b']') {
            self.at += 1;
        } else {
            loop {
                values.push(self.value()?);
                self.space();
                match self.peek() {
                    Some(b',') => self.at += 1,
                    Some(b']') => {
                        self.at += 1;
                        break;
                    }
                    _ => return Err(self.error("expected , or ]")),
                }
            }
        }
        self.depth -= 1;
        Ok(Value::Array(values))
    }

    #[inline(never)]
    fn object(&mut self) -> Result<Value, Error> {
        self.nest()?;
        let mut members: Vec<(String, Value)> = Vec::new();
        // Where each name is, once there are enough members to look up.
        let mut index: Option<std::collections::HashMap<String, usize>> = None;
        self.space();
        if self.peek() == Some(b'}') {
            self.at += 1;
        } else {
            loop {
                self.space();
                if self.peek() != Some(b'"') {
                    return Err(self.error("expected a member's name"));
                }
                let name = self.string()?;
                self.space();
                if self.peek() != Some(b':') {
                    return Err(self.error("expected :"));
                }
                self.at += 1;
                let value = self.value()?;
                let earlier = match &index {
                    Some(index) => index.get(&name).copied(),
                    None => members.iter().position(|(earlier, _)| *earlier == name),
                };
                match earlier {
                    // JavaScript's: the last value, in the first one's place.
                    Some(at) => members[at].1 = value,
                    None => {
                        if let Some(index) = &mut index {
                            index.insert(name.clone(), members.len());
                        }
                        members.push((name, value));
                        if index.is_none() && members.len() > 16 {
                            index = Some(members.iter().enumerate().map(|(at, (name, _))| (name.clone(), at)).collect());
                        }
                    }
                }
                self.space();
                match self.peek() {
                    Some(b',') => self.at += 1,
                    Some(b'}') => {
                        self.at += 1;
                        break;
                    }
                    _ => return Err(self.error("expected , or }")),
                }
            }
        }
        self.depth -= 1;
        Ok(Value::Object(members))
    }

    fn nest(&mut self) -> Result<(), Error> {
        self.depth += 1;
        if self.depth > DEPTH {
            return Err(self.error("nested too deep"));
        }
        self.at += 1;
        Ok(())
    }

    fn digits(&mut self) -> usize {
        let start = self.at;
        while self.peek().is_some_and(|byte| byte.is_ascii_digit()) {
            self.at += 1;
        }
        self.at - start
    }

    // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
    fn number(&mut self) -> Result<Value, Error> {
        let start = self.at;
        if self.peek() == Some(b'-') {
            self.at += 1;
        }
        match self.peek() {
            Some(b'0') => {
                self.at += 1;
                if self.peek().is_some_and(|byte| byte.is_ascii_digit()) {
                    return Err(self.error("a number with a leading zero"));
                }
            }
            Some(b'1'..=b'9') => {
                self.digits();
            }
            _ => return Err(self.error("not a number")),
        }
        if self.peek() == Some(b'.') {
            self.at += 1;
            if self.digits() == 0 {
                return Err(self.error("no digits after the decimal point"));
            }
        }
        if matches!(self.peek(), Some(b'e' | b'E')) {
            self.at += 1;
            if matches!(self.peek(), Some(b'+' | b'-')) {
                self.at += 1;
            }
            if self.digits() == 0 {
                return Err(self.error("no digits in the exponent"));
            }
        }
        // ASCII only, by the grammar above.
        let text = std::str::from_utf8(&self.bytes[start..self.at]).map_err(|_| self.error("not a number"))?;
        match text.parse::<f64>() {
            Ok(number) if number.is_finite() => Ok(Value::Number(number)),
            _ => Err(self.error("a number out of range")),
        }
    }

    fn hex4(&mut self) -> Result<u32, Error> {
        let digits = self.bytes.get(self.at..self.at + 4).ok_or_else(|| self.error("short \\u escape"))?;
        if !digits.iter().all(u8::is_ascii_hexdigit) {
            return Err(self.error("bad \\u escape"));
        }
        let unit = digits.iter().fold(0, |unit, digit| (unit << 4) | (*digit as char).to_digit(16).unwrap_or(0));
        self.at += 4;
        Ok(unit)
    }

    /// The low surrogate of a `\u` escape that comes next, read; `None`
    /// (and nothing read) for anything else.
    fn low_surrogate(&mut self) -> Option<u32> {
        let rest = &self.bytes[self.at..];
        if !rest.starts_with(b"\\u") {
            return None;
        }
        let digits = rest.get(2..6).filter(|digits| digits.iter().all(u8::is_ascii_hexdigit))?;
        let unit = digits.iter().fold(0, |unit, digit| (unit << 4) | (*digit as char).to_digit(16).unwrap_or(0));
        if !(0xdc00..0xe000).contains(&unit) {
            return None;
        }
        self.at += 6;
        Some(unit)
    }

    fn string(&mut self) -> Result<String, Error> {
        self.at += 1;
        let mut out = String::new();
        loop {
            let start = self.at;
            while self.peek().is_some_and(|byte| byte != b'"' && byte != b'\\' && byte >= 0x20) {
                self.at += 1;
            }
            // The input is a &str, and the stops are ASCII: whole characters.
            out.push_str(std::str::from_utf8(&self.bytes[start..self.at]).map_err(|_| self.error("not UTF-8"))?);
            match self.peek() {
                None => return Err(self.error("unterminated string")),
                Some(b'"') => {
                    self.at += 1;
                    return Ok(out);
                }
                Some(b'\\') => {}
                Some(_) => return Err(self.error("a control character in a string")),
            }
            self.at += 1;
            let escape = self.peek().ok_or_else(|| self.error("unterminated escape"))?;
            self.at += 1;
            match escape {
                b'"' => out.push('"'),
                b'\\' => out.push('\\'),
                b'/' => out.push('/'),
                b'b' => out.push('\u{8}'),
                b'f' => out.push('\u{c}'),
                b'n' => out.push('\n'),
                b'r' => out.push('\r'),
                b't' => out.push('\t'),
                b'u' => {
                    let unit = self.hex4()?;
                    // A high surrogate pairs only with a low one; a lone
                    // half is U+FFFD, and what follows it is read on its own.
                    let c = match unit {
                        0xd800..=0xdbff => self.low_surrogate().and_then(|low| char::from_u32(0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00))),
                        _ => char::from_u32(unit),
                    };
                    out.push(c.unwrap_or('\u{fffd}'));
                }
                _ => return Err(self.error("unknown escape")),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::Value;

    #[test]
    fn reads_and_writes() {
        let text = r#" {"a": [1, 2.5, -3e2, true, false, null], "b": {"c": "x\"\\\né😀"}, "": {}} "#;
        let value = Value::parse(text).unwrap();
        assert_eq!(value["a"][1].as_f64(), Some(2.5));
        assert_eq!(value["a"][2].as_i64(), Some(-300));
        assert_eq!(value["b"]["c"].as_str(), Some("x\"\\\né😀"));
        assert_eq!(Value::parse(&value.to_string()).unwrap(), value);
        assert_eq!(value.to_string(), r#"{"a":[1,2.5,-300,true,false,null],"b":{"c":"x\"\\\né😀"},"":{}}"#);
        for bad in ["", "{", "[1,]", "{\"a\" 1}", "\"open", "nul", "1 2", "{\"a\":1,}"] {
            assert!(Value::parse(bad).is_err(), "{bad}");
        }
        assert!(Value::parse(&"[".repeat(600)).is_err());
        let mut object = Value::Null;
        object.set("k", 1.into());
        object.set("k", "v".into());
        assert_eq!(object.to_string(), r#"{"k":"v"}"#);
    }

    #[test]
    fn strict_numbers_and_escapes() {
        for bad in ["01", "-01", "+1", ".5", "1.", "1.e2", "1e", "1e+", "-", "--1", "0x10", "1e999", "-1e999", "Infinity", "NaN", r#""\u+123""#, r#""\u12""#, r#""\u 123""#, r#""\x41""#, "\"a\u{1}b\"", "\"a\nb\"", "[1 2]", "nullx"] {
            assert!(Value::parse(bad).is_err(), "accepted {bad:?}");
        }
        for (good, number) in [("0", 0.0), ("-0", -0.0), ("1", 1.0), ("-1.5", -1.5), ("1e2", 100.0), ("1E+2", 100.0), ("2.5e-1", 0.25), ("1e308", 1e308), ("10", 10.0)] {
            assert_eq!(Value::parse(good).unwrap().as_f64(), Some(number), "{good}");
        }
        // "1e-999" is 0, as in JavaScript; "-0" writes as "0", as JSON.stringify does.
        assert_eq!(Value::parse("1e-999").unwrap().as_f64(), Some(0.0));
        assert_eq!(Value::parse("-0").unwrap().to_string(), "0");
    }

    #[test]
    fn surrogates() {
        let read = |text: &str| Value::parse(text).unwrap().as_str().unwrap().to_string();
        assert_eq!(read(r#""😀""#), "😀");
        assert_eq!(read(r#""😀""#), "😀");
        // A high surrogate before a unit that is not a low one: U+FFFD, then that unit.
        assert_eq!(read(r#""\ud83dA""#), "\u{fffd}A");
        assert_eq!(read(r#""\ud83d😀""#), "\u{fffd}😀");
        assert_eq!(read(r#""\ud83dA""#), "\u{fffd}A");
        assert_eq!(read(r#""\ud83d""#), "\u{fffd}");
        assert_eq!(read(r#""\ude00\ud83d""#), "\u{fffd}\u{fffd}");
        assert_eq!(read(r#""\ud83d\n""#), "\u{fffd}\n");
        assert!(Value::parse(r#""\ud83d\u00""#).is_err());
        assert_eq!(read(r#""é\u0000""#), "é\0");
    }

    #[test]
    fn duplicate_members() {
        // JavaScript's JSON.parse: the last value, in the first one's place.
        let value = Value::parse(r#"{"a":1,"b":2,"a":3}"#).unwrap();
        assert_eq!(value.to_string(), r#"{"a":3,"b":2}"#);
        let many: Vec<String> = (0..40).map(|n| format!("\"k{n}\":{n}")).chain(["\"k3\":\"last\"".into(), "\"k30\":true".into()]).collect();
        let value = Value::parse(&format!("{{{}}}", many.join(","))).unwrap();
        assert_eq!(value["k3"].as_str(), Some("last"));
        assert_eq!(value["k30"].as_bool(), Some(true));
        assert!(matches!(&value, Value::Object(members) if members.len() == 40 && members[3].0 == "k3"));
    }

    #[test]
    fn nesting_on_a_small_stack() {
        // Call::args() and events may be read on a worker of 256 KiB.
        let on_small_stack = |text: String| std::thread::Builder::new().stack_size(256 * 1024).spawn(move || Value::parse(&text).map(|_| ())).unwrap().join().unwrap();
        let nested = |open: &str, close: &str, levels: usize| format!("{}1{}", open.repeat(levels), close.repeat(levels));
        assert!(on_small_stack(nested("[", "]", super::DEPTH)).is_ok());
        assert!(on_small_stack(nested("{\"a\":", "}", super::DEPTH)).is_ok());
        assert!(on_small_stack(nested("[{\"a\":", "}]", super::DEPTH / 2)).is_ok());
        assert!(on_small_stack(nested("[", "]", super::DEPTH + 1)).is_err());
        assert!(on_small_stack(nested("{\"a\":", "}", super::DEPTH + 1)).is_err());
        assert!(on_small_stack(nested("[", "]", 10_000)).is_err());
        assert!(on_small_stack(nested("[{\"a\":", "}]", 5_000)).is_err());
        assert!(on_small_stack("[".repeat(10_000)).is_err());
    }
}
