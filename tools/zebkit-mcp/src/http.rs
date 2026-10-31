//! MCP's Streamable HTTP, on loopback by default. A local server that drives a
//! browser is what any web page would like to reach, so each request has to pass,
//! in this order, before anything is parsed as MCP: its Host (against DNS
//! rebinding), its Origin (against pages), the method and path, the token.
//! There is no CORS: a browser's preflight is refused, and nothing is served to
//! a page. The answers are single JSON objects: no streams, no resumption.

use crate::rpc::{self, Message};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{Read, Write};
use std::net::{IpAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const MAX_HEADERS: usize = 16 * 1024;
const MAX_BODY: usize = 128 * 1024 * 1024;
const MAX_CONNECTIONS: usize = 64;
const MAX_SESSIONS: usize = 256;
/// To send a whole request, however slowly it comes: a read's own timeout would
/// let one byte every 29 seconds keep a connection, and there are 64 of those.
const REQUEST_TIME: Duration = Duration::from_secs(30);
const WRITE_TIMEOUT: Duration = Duration::from_secs(60);

pub struct Config {
    pub bind: IpAddr,
    pub port: u16,
    pub token: String,
    /// Origins of web clients the operator allows, as sent: `https://app.example`.
    pub allow_origins: Vec<String>,
    /// Host names besides this machine's loopback names.
    pub allow_hosts: Vec<String>,
}

struct Server {
    config: Config,
    port: u16,
    /// Sessions of the handshake revisions: an id, its version, when last used.
    /// Most clients just leave, without a DELETE: the oldest makes way for a new one.
    sessions: Mutex<HashMap<String, (String, Instant)>>,
    connections: AtomicUsize,
}

/// Binds now, so that a port in use fails the start; serves on a thread.
pub fn listen(config: Config) -> Result<u16, String> {
    let listener = TcpListener::bind((config.bind, config.port)).map_err(|e| format!("{}:{}: {e}", config.bind, config.port))?;
    let port = listener.local_addr().map_err(|e| e.to_string())?.port();
    let server = Arc::new(Server { config, port, sessions: Mutex::new(HashMap::new()), connections: AtomicUsize::new(0) });
    std::thread::spawn(move || {
        for stream in listener.incoming().flatten() {
            if server.connections.fetch_add(1, Ordering::SeqCst) >= MAX_CONNECTIONS {
                server.connections.fetch_sub(1, Ordering::SeqCst);
                let mut stream = stream;
                let _ = reply(&mut stream, 503, "application/json", &[], br#"{"error":"too many connections"}"#);
                continue;
            }
            let server = server.clone();
            std::thread::spawn(move || {
                handle(&server, stream);
                server.connections.fetch_sub(1, Ordering::SeqCst);
            });
        }
    });
    Ok(port)
}

struct Request {
    method: String,
    path: String,
    headers: Vec<(String, String)>,
}

impl Request {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(key, _)| key.eq_ignore_ascii_case(name)).map(|(_, value)| value.as_str())
    }
    fn count(&self, name: &str) -> usize {
        self.headers.iter().filter(|(key, _)| key.eq_ignore_ascii_case(name)).count()
    }
}

fn reason(status: u16) -> &'static str {
    match status {
        200 => "OK",
        202 => "Accepted",
        204 => "No Content",
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        406 => "Not Acceptable",
        411 => "Length Required",
        413 => "Content Too Large",
        415 => "Unsupported Media Type",
        431 => "Request Header Fields Too Large",
        503 => "Service Unavailable",
        _ => "Error",
    }
}

fn reply(stream: &mut TcpStream, status: u16, kind: &str, extra: &[(&str, String)], body: &[u8]) -> std::io::Result<()> {
    let mut head = format!("HTTP/1.1 {status} {}\r\nContent-Length: {}\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n", reason(status), body.len());
    if !body.is_empty() {
        head.push_str(&format!("Content-Type: {kind}\r\n"));
    }
    for (name, value) in extra {
        head.push_str(&format!("{name}: {value}\r\n"));
    }
    head.push_str("\r\n");
    stream.write_all(head.as_bytes())?;
    stream.write_all(body)?;
    stream.flush()
}

fn refuse(stream: &mut TcpStream, status: u16, id: Option<&Value>, code: i64, message: &str, extra: &[(&str, String)]) {
    let mut body = json!({"jsonrpc": "2.0", "error": {"code": code, "message": message}});
    if let Some(id) = id {
        body["id"] = id.clone();
    }
    let _ = reply(stream, status, "application/json", extra, body.to_string().as_bytes());
}

fn handle(server: &Server, mut stream: TcpStream) {
    let _ = stream.set_write_timeout(Some(WRITE_TIMEOUT));
    let deadline = Instant::now() + REQUEST_TIME;
    let (request, mut rest) = match read_head(&mut stream, deadline) {
        Ok(found) => found,
        Err(status) => {
            refuse(&mut stream, status, None, rpc::INVALID_REQUEST, "bad request", &[]);
            return;
        }
    };

    // Before anything else: who is asking, by what they cannot forge in a browser.
    if request.count("host") != 1 || !host_allowed(&server.config, server.port, request.header("host").unwrap_or("")) {
        refuse(&mut stream, 403, None, rpc::INVALID_REQUEST, "this Host is not served", &[]);
        return;
    }
    if request.count("origin") > 1 || request.header("origin").is_some_and(|origin| !origin_allowed(&server.config, server.port, origin)) {
        refuse(&mut stream, 403, None, rpc::INVALID_REQUEST, "this Origin is not allowed", &[]);
        return;
    }
    if request.path != "/mcp" {
        refuse(&mut stream, 404, None, rpc::INVALID_REQUEST, "the MCP endpoint is /mcp", &[]);
        return;
    }
    if !token_matches(&server.config.token, request.header("authorization")) {
        refuse(&mut stream, 401, None, rpc::INVALID_REQUEST, "a bearer token is required", &[("WWW-Authenticate", "Bearer".to_string())]);
        return;
    }
    match request.method.as_str() {
        "POST" => {}
        "DELETE" => {
            let removed = request.header("mcp-session-id").is_some_and(|id| {
                server.sessions.lock().unwrap_or_else(|e| e.into_inner()).remove(id).is_some()
            });
            let _ = reply(&mut stream, if removed { 204 } else { 404 }, "", &[], b"");
            return;
        }
        _ => {
            refuse(&mut stream, 405, None, rpc::INVALID_REQUEST, "only POST: this server sends no streams", &[("Allow", "POST, DELETE".to_string())]);
            return;
        }
    }
    let kind = request.header("content-type").unwrap_or("").split(';').next().unwrap_or("").trim().to_ascii_lowercase();
    if kind != "application/json" {
        refuse(&mut stream, 415, None, rpc::INVALID_REQUEST, "the body must be application/json", &[]);
        return;
    }
    let accept = request.header("accept").unwrap_or("").to_ascii_lowercase();
    if !(accept.contains("application/json") && accept.contains("text/event-stream")) && !accept.contains("*/*") {
        refuse(&mut stream, 406, None, rpc::INVALID_REQUEST, "Accept must list application/json and text/event-stream", &[]);
        return;
    }
    if request.header("transfer-encoding").is_some() || request.count("content-length") != 1 {
        refuse(&mut stream, 411, None, rpc::INVALID_REQUEST, "one Content-Length is required", &[]);
        return;
    }
    let Some(length) = request.header("content-length").and_then(|text| text.trim().parse::<usize>().ok()) else {
        refuse(&mut stream, 400, None, rpc::INVALID_REQUEST, "a bad Content-Length", &[]);
        return;
    };
    if length > MAX_BODY {
        refuse(&mut stream, 413, None, rpc::INVALID_REQUEST, "the body is too large", &[]);
        return;
    }
    if rest.len() < length {
        let have = rest.len();
        rest.resize(length, 0);
        if read_until(&mut stream, &mut rest[have..], deadline).is_err() {
            return;
        }
    }
    rest.truncate(length);
    let Ok(message) = serde_json::from_slice::<Value>(&rest) else {
        refuse(&mut stream, 400, None, -32700, "Parse error", &[]);
        return;
    };
    answer(server, &mut stream, &request, &message);
}

fn answer(server: &Server, stream: &mut TcpStream, request: &Request, message: &Value) {
    let (id, method) = match rpc::classify(message) {
        Err(why) => {
            refuse(stream, 400, None, why.code, &why.message, &[]);
            return;
        }
        Ok(Message::Response) => {
            refuse(stream, 400, None, rpc::INVALID_REQUEST, "this server sends no requests to answer", &[]);
            return;
        }
        Ok(Message::Notification { .. }) => {
            let _ = reply(stream, 202, "", &[], b"");
            return;
        }
        Ok(Message::Request { id, method }) => (id, method),
    };

    let modern = rpc::declared_version(message);
    if let Some(version) = modern {
        if let Err(why) = rpc::check_version(version) {
            let _ = send(stream, 400, id, Err(why));
            return;
        }
        if let Err(why) = check_mirrors(request, message, version, method) {
            let _ = send(stream, 400, id, Err(rpc::error(rpc::HEADER_MISMATCH, why)));
            return;
        }
    } else if method == "initialize" {
        let session = crate::bounds::hex(&zebkit::services::random_bytes(24));
        let result = rpc::initialize(message);
        {
            let mut sessions = server.sessions.lock().unwrap_or_else(|e| e.into_inner());
            if sessions.len() >= MAX_SESSIONS {
                let oldest = sessions.iter().min_by_key(|(_, (_, used))| *used).map(|(id, _)| id.clone());
                if let Some(oldest) = oldest {
                    sessions.remove(&oldest);
                }
            }
            sessions.insert(session.clone(), (result["protocolVersion"].as_str().unwrap_or("").to_string(), Instant::now()));
        }
        let body = rpc::response(id, Ok(result)).to_string();
        let _ = reply(stream, 200, "application/json", &[("Mcp-Session-Id", session)], body.as_bytes());
        return;
    } else {
        // The handshake revisions: a session this server made.
        let version = request.header("mcp-session-id").and_then(|session| {
            server.sessions.lock().unwrap_or_else(|e| e.into_inner()).get_mut(session).map(|(version, used)| {
                *used = Instant::now();
                version.clone()
            })
        });
        let Some(version) = version else {
            let status = if request.header("mcp-session-id").is_some() { 404 } else { 400 };
            refuse(stream, status, Some(id), rpc::INVALID_REQUEST, "no such session; initialize, or send the protocol version in _meta", &[]);
            return;
        };
        if request.header("mcp-protocol-version").is_some_and(|header| header != version) {
            refuse(stream, 400, Some(id), rpc::INVALID_REQUEST, "MCP-Protocol-Version is not the session's", &[]);
            return;
        }
    }
    let result = rpc::dispatch(message, method, modern.is_some());
    let status = match &result {
        Err(why) if why.code == rpc::METHOD_NOT_FOUND && modern.is_some() => 404,
        _ => 200,
    };
    let _ = send(stream, status, id, result);
}

fn send(stream: &mut TcpStream, status: u16, id: &Value, result: Result<Value, rpc::RpcError>) -> std::io::Result<()> {
    reply(stream, status, "application/json", &[], rpc::response(id, result).to_string().as_bytes())
}

/// The current revision's headers mirror the body, and must agree with it.
fn check_mirrors(request: &Request, message: &Value, version: &str, method: &str) -> Result<(), String> {
    match request.header("mcp-protocol-version") {
        Some(header) if header == version => {}
        Some(header) => return Err(format!("MCP-Protocol-Version {} does not match the body", clip(header))),
        None => return Err("MCP-Protocol-Version is required".to_string()),
    }
    match request.header("mcp-method") {
        Some(header) if header == method => {}
        Some(header) => return Err(format!("Mcp-Method {} does not match the body", clip(header))),
        None => return Err("Mcp-Method is required".to_string()),
    }
    if method == "tools/call" {
        let Some(name) = message["params"]["name"].as_str() else { return Ok(()) };
        let Some(header) = request.header("mcp-name") else { return Err("Mcp-Name is required".to_string()) };
        let decoded = match header.strip_prefix("=?base64?").and_then(|rest| rest.strip_suffix("?=")) {
            Some(encoded) => {
                let bytes = crate::tools::base64_decode(encoded).map_err(|_| "Mcp-Name holds bad base64".to_string())?;
                String::from_utf8(bytes).map_err(|_| "Mcp-Name holds bad UTF-8".to_string())?
            }
            None => header.to_string(),
        };
        if decoded != name {
            return Err(format!("Mcp-Name {} does not match the body", clip(&decoded)));
        }
    }
    Ok(())
}

fn clip(text: &str) -> String {
    text.chars().filter(|c| !c.is_control()).take(100).collect()
}

/// `Host` names this server: its loopback names, or those the operator lists. A
/// page that got here through DNS rebinding sends its own name.
fn host_allowed(config: &Config, port: u16, host: &str) -> bool {
    let host = host.trim().to_ascii_lowercase();
    // `name` or `name:port`, `[address]` or `[address]:port`, and nothing else.
    let (name, given) = if host.starts_with('[') {
        let Some(end) = host.find(']') else { return false };
        let (name, rest) = host.split_at(end + 1);
        match rest.strip_prefix(':') {
            Some(given) => (name.to_string(), Some(given.to_string())),
            None if rest.is_empty() => (name.to_string(), None),
            None => return false,
        }
    } else {
        match host.split_once(':') {
            Some((name, given)) => (name.to_string(), Some(given.to_string())),
            None => (host.clone(), None),
        }
    };
    if given.is_some_and(|given| given != port.to_string()) {
        return false;
    }
    let bound = if config.bind.is_ipv6() { format!("[{}]", config.bind) } else { config.bind.to_string() };
    matches!(name.as_str(), "127.0.0.1" | "localhost" | "[::1]")
        || bound == name
        || config.allow_hosts.iter().any(|allowed| allowed.eq_ignore_ascii_case(&name))
}

/// `Origin`, which browsers send and pages cannot change: only what the operator
/// named, and this server's own loopback address (it serves no page there).
/// "null", from sandboxed frames and files, is never allowed.
fn origin_allowed(config: &Config, port: u16, origin: &str) -> bool {
    config.allow_origins.iter().any(|allowed| allowed == origin)
        || [format!("http://127.0.0.1:{port}"), format!("http://localhost:{port}"), format!("http://[::1]:{port}")]
            .iter()
            .any(|own| own == origin)
}

fn token_matches(token: &str, header: Option<&str>) -> bool {
    let Some(header) = header else { return false };
    let Some((scheme, given)) = header.split_once(' ') else { return false };
    if !scheme.eq_ignore_ascii_case("bearer") {
        return false;
    }
    let (given, token) = (given.trim().as_bytes(), token.as_bytes());
    // The whole of it, whatever matches: no time to measure.
    let mut difference = given.len() ^ token.len();
    for (index, byte) in token.iter().enumerate() {
        difference |= (*byte ^ given.get(index).copied().unwrap_or(0)) as usize;
    }
    difference == 0
}

/// One read that ends by `deadline`.
fn read_before(stream: &mut TcpStream, buffer: &mut [u8], deadline: Instant) -> std::io::Result<usize> {
    let left = deadline.saturating_duration_since(Instant::now());
    if left.is_zero() {
        return Err(std::io::ErrorKind::TimedOut.into());
    }
    stream.set_read_timeout(Some(left))?;
    stream.read(buffer)
}

fn read_until(stream: &mut TcpStream, mut buffer: &mut [u8], deadline: Instant) -> std::io::Result<()> {
    while !buffer.is_empty() {
        let read = read_before(stream, buffer, deadline)?;
        if read == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        buffer = &mut buffer[read..];
    }
    Ok(())
}

/// The head of a request, and what came after it.
fn read_head(stream: &mut TcpStream, deadline: Instant) -> Result<(Request, Vec<u8>), u16> {
    let mut buffer = Vec::with_capacity(4096);
    let mut chunk = [0; 4096];
    let end = loop {
        if let Some(end) = buffer.windows(4).position(|window| window == b"\r\n\r\n") {
            break end;
        }
        if buffer.len() > MAX_HEADERS {
            return Err(431);
        }
        let read = read_before(stream, &mut chunk, deadline).map_err(|_| 400u16)?;
        if read == 0 {
            return Err(400);
        }
        buffer.extend_from_slice(&chunk[..read]);
    };
    if end > MAX_HEADERS {
        return Err(431);
    }
    let head = std::str::from_utf8(&buffer[..end]).map_err(|_| 400u16)?;
    let mut lines = head.split("\r\n");
    let mut first = lines.next().unwrap_or("").split(' ');
    let (method, path, version) = (first.next().unwrap_or(""), first.next().unwrap_or(""), first.next().unwrap_or(""));
    if method.is_empty() || path.is_empty() || !version.starts_with("HTTP/1.") || first.next().is_some() {
        return Err(400);
    }
    let mut headers = Vec::new();
    for line in lines {
        let Some((name, value)) = line.split_once(':') else { return Err(400) };
        if name.is_empty() || name.contains(|c: char| c.is_whitespace() || c.is_control()) || value.contains(|c: char| c.is_control() && c != '\t') {
            return Err(400);
        }
        headers.push((name.to_string(), value.trim().to_string()));
    }
    let rest = buffer[end + 4..].to_vec();
    Ok((Request { method: method.to_string(), path: path.to_string(), headers }, rest))
}
