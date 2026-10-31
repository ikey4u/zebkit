//! Every tool's arguments are checked against the schema it publishes, before
//! the tool sees them: types, ranges, sizes and no property it does not name.
//! The JSON Schema subset here is what the tools use; a schema keyword that is
//! not enforced fails the server's start (`check_schema`).

use serde_json::{json, Value};

/// How long a string may be unless a schema says otherwise.
const DEFAULT_MAX_LENGTH: u64 = 1024 * 1024;
const MAX_DEPTH: usize = 16;

pub fn string() -> Value {
    json!({"type": "string", "maxLength": 65536})
}
pub fn long_string(max: u64) -> Value {
    json!({"type": "string", "maxLength": max})
}
pub fn described(mut schema: Value, text: &str) -> Value {
    schema["description"] = json!(text);
    schema
}
pub fn integer(minimum: i64, maximum: i64) -> Value {
    json!({"type": "integer", "minimum": minimum, "maximum": maximum})
}
pub fn number(minimum: f64, maximum: f64) -> Value {
    json!({"type": "number", "minimum": minimum, "maximum": maximum})
}
pub fn boolean() -> Value {
    json!({"type": "boolean"})
}
pub fn choice(values: &[&str]) -> Value {
    json!({"type": "string", "enum": values})
}
pub fn array(items: Value, max: u64) -> Value {
    json!({"type": "array", "items": items, "maxItems": max})
}
/// An object the engine checks itself, all or nothing (annotation fields).
pub fn object_free() -> Value {
    json!({"type": "object"})
}
/// An object with the properties given, `required` of them there, and no others.
pub fn object(properties: &[(&str, Value)], required: &[&str]) -> Value {
    let mut map = serde_json::Map::new();
    for (name, schema) in properties {
        map.insert((*name).to_string(), schema.clone());
    }
    json!({"type": "object", "properties": map, "required": required, "additionalProperties": false})
}

/// The first thing wrong with `value`.
pub fn validate(value: &Value, schema: &Value, path: &str, depth: usize) -> Result<(), String> {
    let at = |path: &str| if path.is_empty() { "arguments".to_string() } else { path.to_string() };
    if depth > MAX_DEPTH {
        return Err(format!("{}: nested too deeply", at(path)));
    }
    let kind = schema["type"].as_str().unwrap_or("");
    let matches = match kind {
        "string" => value.is_string(),
        "boolean" => value.is_boolean(),
        "integer" => value.as_i64().is_some() || value.as_u64().is_some() || value.as_f64().is_some_and(|n| n.fract() == 0.0),
        "number" => value.is_number(),
        "array" => value.is_array(),
        "object" => value.is_object(),
        _ => true,
    };
    if !matches {
        return Err(format!("{}: expected {kind}", at(path)));
    }
    if let Some(text) = value.as_str() {
        let max = schema["maxLength"].as_u64().unwrap_or(DEFAULT_MAX_LENGTH);
        if text.len() as u64 > max {
            return Err(format!("{}: longer than {max} bytes", at(path)));
        }
        if let Some(allowed) = schema["enum"].as_array() {
            if !allowed.iter().any(|option| option == value) {
                return Err(format!("{}: must be one of {}", at(path), schema["enum"]));
            }
        }
        if text.contains('\0') {
            return Err(format!("{}: holds a zero byte", at(path)));
        }
    }
    if let Some(number) = value.as_f64() {
        if !number.is_finite() {
            return Err(format!("{}: not a finite number", at(path)));
        }
        if schema["minimum"].as_f64().is_some_and(|minimum| number < minimum) {
            return Err(format!("{}: under the minimum {}", at(path), schema["minimum"]));
        }
        if schema["maximum"].as_f64().is_some_and(|maximum| number > maximum) {
            return Err(format!("{}: over the maximum {}", at(path), schema["maximum"]));
        }
    }
    if let Some(items) = value.as_array() {
        if schema["maxItems"].as_u64().is_some_and(|max| items.len() as u64 > max) {
            return Err(format!("{}: more than {} items", at(path), schema["maxItems"]));
        }
        if let Some(item_schema) = schema.get("items") {
            for (index, item) in items.iter().enumerate() {
                validate(item, item_schema, &format!("{path}[{index}]"), depth + 1)?;
            }
        }
    }
    if let Some(members) = value.as_object() {
        if let Some(required) = schema["required"].as_array() {
            for name in required.iter().filter_map(Value::as_str) {
                if !members.contains_key(name) {
                    return Err(format!("{}: {name} is required", at(path)));
                }
            }
        }
        if let Some(properties) = schema["properties"].as_object() {
            for (name, member) in members {
                let inner = if path.is_empty() { name.clone() } else { format!("{path}.{name}") };
                match properties.get(name) {
                    Some(member_schema) => validate(member, member_schema, &inner, depth + 1)?,
                    None if schema["additionalProperties"] == json!(false) => {
                        return Err(format!("{inner}: not an argument of this tool"));
                    }
                    None => {}
                }
            }
        }
    }
    Ok(())
}

/// A tool's own schema uses only what `validate` enforces.
pub fn check_schema(schema: &Value) -> Result<(), String> {
    const KNOWN: &[&str] = &[
        "type", "properties", "required", "additionalProperties", "items", "maxItems", "maxLength", "minimum", "maximum",
        "enum", "description",
    ];
    if let Some(members) = schema.as_object() {
        for (key, value) in members {
            if !KNOWN.contains(&key.as_str()) {
                return Err(format!("unknown schema keyword {key}"));
            }
            if key == "items" {
                check_schema(value)?;
            }
            if key == "properties" {
                for member in value.as_object().into_iter().flat_map(|m| m.values()) {
                    check_schema(member)?;
                }
            }
        }
    }
    Ok(())
}
