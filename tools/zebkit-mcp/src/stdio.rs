//! MCP over standard input and output: one JSON-RPC message per line. Standard
//! output is the protocol's alone, so the descriptor is taken away from
//! everything else in the process and its children, which see standard error
//! there instead.

use crate::rpc::{self, Message};
use serde_json::{json, Value};
use std::collections::HashSet;
use std::io::{BufRead, Read, Write};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

const MAX_LINE: u64 = 128 * 1024 * 1024;
const MAX_IN_FLIGHT: usize = 64;

/// The protocol's two descriptors, and what everyone else gets at 0 and 1.
#[cfg(unix)]
pub struct Streams {
    pub input: std::fs::File,
    pub output: std::fs::File,
}

#[cfg(unix)]
pub fn take_streams() -> Streams {
    use std::os::fd::FromRawFd;
    extern "C" {
        fn dup(fd: i32) -> i32;
        fn dup2(old: i32, new: i32) -> i32;
        fn fcntl(fd: i32, command: i32, argument: i32) -> i32;
    }
    const F_SETFD: i32 = 2;
    const FD_CLOEXEC: i32 = 1;
    unsafe {
        let input = dup(0);
        let output = dup(1);
        assert!(input >= 0 && output >= 0, "zebkit-mcp: standard input and output are needed");
        fcntl(input, F_SETFD, FD_CLOEXEC);
        fcntl(output, F_SETFD, FD_CLOEXEC);
        if let Ok(null) = std::fs::File::open("/dev/null") {
            use std::os::fd::AsRawFd;
            dup2(null.as_raw_fd(), 0);
        }
        dup2(2, 1);
        Streams { input: std::fs::File::from_raw_fd(input), output: std::fs::File::from_raw_fd(output) }
    }
}

/// Reads messages until standard input ends. Each request runs on a thread of
/// its own; answers come back in the order they finish, by id.
#[cfg(unix)]
pub fn serve(streams: Streams) {
    let output = Arc::new(Mutex::new(streams.output));
    let initialized = Arc::new(AtomicBool::new(false));
    let in_flight = Arc::new(AtomicUsize::new(0));
    // A cancellation counts only for what is running: one for a request that has
    // ended would stay, and drop the answer to the next request of that id.
    let running: Arc<Mutex<HashSet<String>>> = Arc::new(Mutex::new(HashSet::new()));
    let cancelled: Arc<Mutex<HashSet<String>>> = Arc::new(Mutex::new(HashSet::new()));
    let mut input = std::io::BufReader::new(streams.input);
    loop {
        let mut line = Vec::new();
        match input.by_ref().take(MAX_LINE).read_until(b'\n', &mut line) {
            Ok(0) | Err(_) => return,
            Ok(_) => {}
        }
        if line.last() != Some(&b'\n') && line.len() as u64 >= MAX_LINE {
            send(&output, &json!({"jsonrpc": "2.0", "id": null, "error": {"code": rpc::INVALID_REQUEST, "message": "message too long"}}));
            return;
        }
        if line.iter().all(u8::is_ascii_whitespace) {
            continue;
        }
        let message: Value = match serde_json::from_slice(&line) {
            Ok(message) => message,
            Err(_) => {
                send(&output, &json!({"jsonrpc": "2.0", "id": null, "error": {"code": -32700, "message": "Parse error"}}));
                continue;
            }
        };
        match rpc::classify(&message) {
            Err(why) => {
                let id = message.get("id").filter(|id| id.is_string() || id.is_number()).cloned().unwrap_or(Value::Null);
                send(&output, &rpc::response(&id, Err(why)));
            }
            Ok(Message::Response) => {}
            Ok(Message::Notification { method }) => {
                if method == "notifications/cancelled" {
                    let id = message["params"]["requestId"].to_string();
                    let running = running.lock().unwrap_or_else(|e| e.into_inner());
                    if running.contains(&id) {
                        cancelled.lock().unwrap_or_else(|e| e.into_inner()).insert(id);
                    }
                }
            }
            Ok(Message::Request { id, method }) => {
                let id = id.clone();
                let method = method.to_string();
                let modern = rpc::declared_version(&message).map(str::to_string);
                if method == "initialize" && modern.is_none() {
                    initialized.store(true, Ordering::SeqCst);
                    send(&output, &rpc::response(&id, Ok(rpc::initialize(&message))));
                    continue;
                }
                if let Some(version) = &modern {
                    if let Err(why) = rpc::check_version(version) {
                        send(&output, &rpc::response(&id, Err(why)));
                        continue;
                    }
                } else if !initialized.load(Ordering::SeqCst) && method != "ping" && method != "server/discover" {
                    send(&output, &rpc::response(&id, Err(rpc::error(rpc::INVALID_REQUEST, "no protocol version: send _meta with it, or initialize first"))));
                    continue;
                }
                if in_flight.fetch_add(1, Ordering::SeqCst) >= MAX_IN_FLIGHT {
                    in_flight.fetch_sub(1, Ordering::SeqCst);
                    send(&output, &rpc::response(&id, Err(rpc::error(-32603, "too many requests in flight"))));
                    continue;
                }
                running.lock().unwrap_or_else(|e| e.into_inner()).insert(id.to_string());
                let (output, in_flight, running, cancelled) = (output.clone(), in_flight.clone(), running.clone(), cancelled.clone());
                std::thread::spawn(move || {
                    let answer = rpc::dispatch(&message, &method, modern.is_some());
                    in_flight.fetch_sub(1, Ordering::SeqCst);
                    // A cancelled request gets no further messages. In this order, so
                    // that one seen running is seen here.
                    let dropped = {
                        let mut running = running.lock().unwrap_or_else(|e| e.into_inner());
                        running.remove(&id.to_string());
                        cancelled.lock().unwrap_or_else(|e| e.into_inner()).remove(&id.to_string())
                    };
                    if !dropped {
                        send(&output, &rpc::response(&id, answer));
                    }
                });
            }
        }
    }
}

fn send(output: &Mutex<std::fs::File>, message: &Value) {
    let Ok(mut line) = serde_json::to_vec(message) else { return };
    line.push(b'\n');
    let mut output = output.lock().unwrap_or_else(|e| e.into_inner());
    let _ = output.write_all(&line);
    let _ = output.flush();
}
