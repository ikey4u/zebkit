//! OAuth's device flow (RFC 8628), a tool of this server: not an API of the
//! engine, three HTTPS requests. The person sees a code and a URL and approves
//! on any device. The tokens stay in this process's memory, on the engine's
//! thread: never in a result, a log, a file or a page. An agent holds the flow's
//! id, and net_fetch adds the token only to requests to that provider's API.

use super::{ok, pages, text, Done, Run, Tool};
use crate::bounds::{self, Bounds};
use crate::schema::{array, described, object, string};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::OnceLock;
use std::time::{Duration, Instant};
use zebkit::services::{url_part, UrlPart};

/// What the operator's file says of a provider. A secret here (Google's device
/// flow has one) is the operator's, and goes only to the token endpoint.
#[derive(Clone)]
struct Provider {
    client_id: String,
    client_secret: Option<String>,
    device_endpoint: String,
    token_endpoint: String,
    api_origins: Vec<String>,
    scopes: Vec<String>,
}

/// What a provider's numbers count for at most: a year.
const MAX_SECONDS: u64 = 365 * 24 * 3600;
const MAX_FLOWS: usize = 64;

static PROVIDERS: OnceLock<HashMap<String, Provider>> = OnceLock::new();

fn known_endpoints(name: &str) -> Option<(&'static str, &'static str, &'static [&'static str])> {
    match name {
        "github" => Some(("https://github.com/login/device/code", "https://github.com/login/oauth/access_token", &["https://api.github.com"])),
        "google" => Some(("https://oauth2.googleapis.com/device/code", "https://oauth2.googleapis.com/token", &["https://www.googleapis.com"])),
        "microsoft" => Some(("https://login.microsoftonline.com/common/oauth2/v2.0/devicecode", "https://login.microsoftonline.com/common/oauth2/v2.0/token", &["https://graph.microsoft.com"])),
        _ => None,
    }
}

/// Reads --oauth-config, before the engine starts, and gives the hosts the
/// device flow must reach.
pub fn configure(path: &str) -> Result<Vec<String>, String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{path}: {e}"))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        if std::fs::metadata(path).map(|meta| meta.mode()).unwrap_or(0o777) & 0o077 != 0 {
            return Err(format!("{path}: readable by others; chmod 600 it"));
        }
    }
    let config: Value = serde_json::from_slice(&bytes).map_err(|_| format!("{path}: not JSON"))?;
    let members = config.as_object().ok_or_else(|| format!("{path}: an object of providers"))?;
    let (mut providers, mut hosts) = (HashMap::new(), Vec::new());
    for (name, entry) in members {
        let known = known_endpoints(name);
        let field = |key: &str, default: Option<&str>| -> Result<String, String> {
            entry[key].as_str().or(default).map(str::to_string).ok_or_else(|| format!("{path}: {name}.{key} is required"))
        };
        let strings = |key: &str| -> Vec<String> {
            entry[key].as_array().into_iter().flatten().filter_map(|value| value.as_str().map(str::to_string)).collect()
        };
        let mut api_origins = strings("apiOrigins");
        if api_origins.is_empty() {
            api_origins = known.map(|known| known.2.iter().map(|origin| origin.to_string()).collect()).unwrap_or_default();
        }
        let provider = Provider {
            client_id: field("clientId", None)?,
            client_secret: entry["clientSecret"].as_str().map(str::to_string),
            device_endpoint: field("deviceEndpoint", known.map(|known| known.0))?,
            token_endpoint: field("tokenEndpoint", known.map(|known| known.1))?,
            api_origins,
            scopes: strings("scopes"),
        };
        for endpoint in [&provider.device_endpoint, &provider.token_endpoint] {
            let host = bounds::plain_host(endpoint.split('/').take(3).collect::<Vec<_>>().join("/").as_str())?;
            let local = matches!(host.as_str(), "127.0.0.1" | "localhost" | "[::1]");
            if !endpoint.starts_with("https://") && !(local && endpoint.starts_with("http://")) {
                return Err(format!("{path}: {name}: {endpoint}: https is required, but for this machine"));
            }
            if !hosts.contains(&host) {
                hosts.push(host);
            }
        }
        providers.insert(name.clone(), provider);
    }
    PROVIDERS.set(providers).map_err(|_| "configured twice".to_string())?;
    Ok(hosts)
}

struct Tokens {
    access: String,
    refresh: Option<String>,
    expires: Option<Instant>,
}

enum Status {
    Pending,
    Approved(Tokens),
    Failed(String),
}

struct Flow {
    provider: String,
    device_code: String,
    interval: u64,
    next_poll: Instant,
    expires: Instant,
    scopes: String,
    status: Status,
    /// A poll is on its way: an answer to a second could overtake it, and a device
    /// code that the first has just used up reads as a failure.
    polling: bool,
}

thread_local! {
    static FLOWS: RefCell<HashMap<String, Flow>> = RefCell::new(HashMap::new());
}

fn provider(name: &str) -> Result<&'static Provider, String> {
    PROVIDERS.get().and_then(|all| all.get(name)).ok_or_else(|| format!("no provider {name}; the server's --oauth-config names those it has"))
}

fn form(pairs: &[(&str, &str)]) -> String {
    let encode = |text: &str| -> String {
        text.bytes().map(|byte| match byte {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'~' => (byte as char).to_string(),
            _ => format!("%{byte:02X}"),
        }).collect()
    };
    pairs.iter().map(|(name, value)| format!("{}={}", encode(name), encode(value))).collect::<Vec<_>>().join("&")
}

/// A POST to a provider's endpoint, as JSON, in a profile of its own with no
/// page in it. Only what the provider's error says of itself comes back.
fn post(endpoint: &str, body: String, done: impl FnOnce(Result<(u64, Value), String>) + 'static) {
    let request = pages::ServiceRequest {
        url: endpoint.to_string(),
        method: "POST".to_string(),
        headers: vec![json!({"name": "Content-Type", "value": "application/x-www-form-urlencoded"}), json!({"name": "Accept", "value": "application/json"})],
        body: Some(super::base64_encode(body.as_bytes())),
        authorization: None,
        timeout: 30_000,
        limit: 1024 * 1024,
    };
    pages::service_fetch("service:oauth", request, move |result| {
        done(result.and_then(|response| {
            if response.get("redirect").is_some() {
                return Err("the provider's endpoint redirects elsewhere; refused".to_string());
            }
            let bytes = super::base64_decode(response["body"].as_str().unwrap_or(""))?;
            let value = serde_json::from_slice(&bytes).map_err(|_| "the provider did not answer with JSON".to_string())?;
            Ok((response["status"].as_u64().unwrap_or(0), value))
        }));
    });
}

fn plain(value: &Value) -> String {
    value.as_str().unwrap_or("").chars().filter(|c| !c.is_control()).take(300).collect()
}

fn auth_device_start(_: &'static Bounds, args: Value, done: Done) {
    let name = text(&args, "provider").to_string();
    let config = match provider(&name) {
        Ok(config) => config,
        Err(why) => return done(Err(why)),
    };
    let scopes: Vec<String> = args["scopes"].as_array().map(|list| list.iter().filter_map(|s| s.as_str().map(str::to_string)).collect()).unwrap_or_else(|| config.scopes.clone());
    if !config.scopes.is_empty() {
        if let Some(wanted) = scopes.iter().find(|scope| !config.scopes.contains(scope)) {
            return done(Err(format!("the scope {wanted} is not among those the operator allows for {name}")));
        }
    }
    let scopes = scopes.join(" ");
    let body = form(&[("client_id", &config.client_id), ("scope", &scopes)]);
    post(&config.device_endpoint, body, move |result| {
        let (status, answer) = match result {
            Ok(found) => found,
            Err(why) => return done(Err(why)),
        };
        let (Some(device_code), Some(user_code)) = (answer["device_code"].as_str(), answer["user_code"].as_str()) else {
            return done(Err(format!("the provider refused ({status}): {} {}", plain(&answer["error"]), plain(&answer["error_description"]))));
        };
        let verification = answer["verification_uri"].as_str().or(answer["verification_url"].as_str()).unwrap_or("");
        // A code to read to a person, on a page they trust: https only.
        if !verification.starts_with("https://") && !verification.starts_with("http://127.0.0.1") {
            return done(Err("the provider gave a verification address that is not https".to_string()));
        }
        // Clamped: an Instant panics on a sum too large, and this is the engine's thread.
        let (interval, lifetime) = (answer["interval"].as_u64().unwrap_or(5).clamp(1, MAX_SECONDS), answer["expires_in"].as_u64().unwrap_or(900).min(MAX_SECONDS));
        let id = format!("f{}", bounds::hex(&zebkit::services::random_bytes(8)));
        let flow = Flow {
            provider: name.clone(),
            device_code: device_code.to_string(),
            interval,
            next_poll: Instant::now() + Duration::from_secs(interval),
            expires: Instant::now() + Duration::from_secs(lifetime),
            scopes,
            status: Status::Pending,
            polling: false,
        };
        let room = FLOWS.with(|flows| {
            let mut flows = flows.borrow_mut();
            let now = Instant::now();
            flows.retain(|_, flow| !matches!(flow.status, Status::Pending) || now < flow.expires);
            if flows.len() >= MAX_FLOWS {
                return false;
            }
            flows.insert(id.clone(), flow);
            true
        });
        if !room {
            return done(Err(format!("at most {MAX_FLOWS} flows at once; auth_forget one")));
        }
        let mut shown = json!({
            "flow": id, "provider": name, "userCode": user_code, "verificationUri": verification,
            "expiresIn": lifetime, "interval": interval,
            "tell": "Show the person this code and address; they approve on any device. Then poll auth_device_poll.",
        });
        if let Some(complete) = answer["verification_uri_complete"].as_str().filter(|uri| uri.starts_with("https://") || uri.starts_with("http://127.0.0.1")) {
            shown["verificationUriComplete"] = json!(complete);
        }
        done(ok(shown));
    });
}

fn status_of(id: &str, flow: &Flow) -> Value {
    let state = match &flow.status {
        Status::Pending => "pending",
        Status::Approved(_) => "approved",
        Status::Failed(_) => "failed",
    };
    let mut answer = json!({"flow": id, "provider": flow.provider, "status": state, "scopes": flow.scopes});
    if let Status::Failed(why) = &flow.status {
        answer["reason"] = json!(why);
    }
    answer
}

/// Asks the token endpoint once, no sooner than the provider allows. The answer
/// says how the flow stands, and never holds the token.
fn auth_device_poll(_: &'static Bounds, args: Value, done: Done) {
    let id = text(&args, "flow").to_string();
    let due = FLOWS.with(|flows| {
        let mut flows = flows.borrow_mut();
        let flow = flows.get_mut(&id).ok_or_else(|| format!("no flow {id}"))?;
        if !matches!(flow.status, Status::Pending) {
            return Ok(Err(status_of(&id, flow)));
        }
        let now = Instant::now();
        if now >= flow.expires {
            flow.status = Status::Failed("expired".to_string());
            return Ok(Err(status_of(&id, flow)));
        }
        if now < flow.next_poll || flow.polling {
            let mut answer = status_of(&id, flow);
            answer["retryAfter"] = json!(flow.next_poll.saturating_duration_since(now).as_secs() + 1);
            return Ok(Err(answer));
        }
        flow.polling = true;
        flow.next_poll = now + Duration::from_secs(flow.interval);
        Ok(Ok((flow.provider.clone(), flow.device_code.clone())))
    });
    let (name, device_code) = match due {
        Err(why) => return done(Err(why)),
        Ok(Err(answer)) => return done(ok(answer)),
        Ok(Ok(found)) => found,
    };
    let config = match provider(&name) {
        Ok(config) => config,
        Err(why) => return done(Err(why)),
    };
    let grant = "urn:ietf:params:oauth:grant-type:device_code";
    let mut pairs = vec![("grant_type", grant), ("device_code", device_code.as_str()), ("client_id", config.client_id.as_str())];
    if let Some(secret) = &config.client_secret {
        pairs.push(("client_secret", secret));
    }
    let polled = id.clone();
    post(&config.token_endpoint, form(&pairs), move |result| {
        FLOWS.with(|flows| {
            if let Some(flow) = flows.borrow_mut().get_mut(&polled) {
                flow.polling = false;
            }
        });
        let (_, answer) = match result {
            Ok(found) => found,
            Err(why) => return done(Err(why)),
        };
        let shown = FLOWS.with(|flows| {
            let mut flows = flows.borrow_mut();
            let Some(flow) = flows.get_mut(&id) else { return Err(format!("no flow {id}")) };
            if let Some(access) = answer["access_token"].as_str() {
                flow.status = Status::Approved(Tokens {
                    access: access.to_string(),
                    refresh: answer["refresh_token"].as_str().map(str::to_string),
                    expires: answer["expires_in"].as_u64().map(|seconds| Instant::now() + Duration::from_secs(seconds.min(MAX_SECONDS))),
                });
                flow.device_code.clear();
            } else {
                match answer["error"].as_str().unwrap_or("") {
                    "authorization_pending" => {}
                    "slow_down" => {
                        flow.interval = (flow.interval + 5).min(MAX_SECONDS);
                        flow.next_poll = Instant::now() + Duration::from_secs(flow.interval);
                    }
                    "access_denied" => flow.status = Status::Failed("the person declined".to_string()),
                    "expired_token" => flow.status = Status::Failed("expired".to_string()),
                    other => flow.status = Status::Failed(format!("{} {}", other.chars().take(100).collect::<String>(), plain(&answer["error_description"]))),
                }
            }
            Ok(status_of(&id, flow))
        });
        done(shown.and_then(ok));
    });
}

fn auth_status(_: &'static Bounds, _: Value, done: Done) {
    let providers: Vec<String> = PROVIDERS.get().map(|all| all.keys().cloned().collect()).unwrap_or_default();
    let flows: Vec<Value> = FLOWS.with(|flows| flows.borrow().iter().map(|(id, flow)| status_of(id, flow)).collect());
    done(ok(json!({"providers": providers, "flows": flows})));
}

fn auth_forget(_: &'static Bounds, args: Value, done: Done) {
    let id = text(&args, "flow");
    match FLOWS.with(|flows| flows.borrow_mut().remove(id)) {
        Some(_) => done(ok(json!({"forgotten": id}))),
        None => done(Err(format!("no flow {id}"))),
    }
}

/// A flow's token for one request, once approved: for net_fetch.
pub struct Credential {
    access: String,
    origins: Vec<String>,
    provider: String,
}

impl Credential {
    /// The header, for a URL of the provider's API; a refusal for any other, so
    /// that a redirect or a mistaken URL does not carry the token elsewhere.
    pub fn header_for(&self, url: &str) -> Result<String, String> {
        let origin = url_part(url, None, UrlPart::Origin).unwrap_or_default();
        if self.origins.iter().any(|allowed| url_part(allowed, None, UrlPart::Origin).as_deref() == Some(origin.as_str())) {
            Ok(format!("Bearer {}", self.access))
        } else {
            Err(format!("the token of {} is only sent to its API, not to {origin}", self.provider))
        }
    }

    /// `text` without the token in it.
    pub fn redact(&self, text: &str) -> String {
        text.replace(&self.access, "[token]")
    }

    pub fn redact_bytes(&self, bytes: &[u8]) -> Vec<u8> {
        let token = self.access.as_bytes();
        if token.is_empty() {
            return bytes.to_vec();
        }
        let (mut out, mut at) = (Vec::with_capacity(bytes.len()), 0);
        while at < bytes.len() {
            if bytes[at..].starts_with(token) {
                out.extend_from_slice(b"[token]");
                at += token.len();
            } else {
                out.push(bytes[at]);
                at += 1;
            }
        }
        out
    }
}

pub fn credential(id: &str) -> Result<Credential, String> {
    FLOWS.with(|flows| {
        let flows = flows.borrow();
        let flow = flows.get(id).ok_or_else(|| format!("no flow {id}"))?;
        let Status::Approved(tokens) = &flow.status else { return Err(format!("the flow {id} is not approved")) };
        if tokens.expires.is_some_and(|expires| Instant::now() >= expires) {
            // A refresh token is kept, unused: a new flow is the plain way back.
            let refreshable = tokens.refresh.is_some();
            return Err(format!("the token of {id} has expired{}; start a new flow", if refreshable { " (refresh is not implemented)" } else { "" }));
        }
        let config = provider(&flow.provider)?;
        Ok(Credential { access: tokens.access.clone(), origins: config.api_origins.clone(), provider: flow.provider.clone() })
    })
}

fn tool(name: &'static str, description: &'static str, read_only: bool, schema: Value, run: fn(&'static Bounds, Value, Done)) -> Tool {
    Tool { name, group: "auth", description, schema, read_only, destructive: false, run: Run::Engine(run) }
}

pub fn tools() -> Vec<Tool> {
    let flow = || ("flow", described(string(), "A flow, from auth_device_start"));
    vec![
        tool("auth_device_start", "Sign in to a provider's API without a screen (OAuth's device flow, RFC 8628): gives a code and an address to show the person, who approves on any device. The token stays in the server: net_fetch uses it by the flow's id.", false,
            object(&[("provider", described(string(), "A name in the server's --oauth-config")), ("scopes", array(string(), 64))], &["provider"]), auth_device_start),
        tool("auth_device_poll", "How a flow stands: pending, approved or failed. Asks the provider no sooner than it allows.", false, object(&[flow()], &["flow"]), auth_device_poll),
        tool("auth_status", "The providers configured and the flows of this server; never a token.", true, object(&[], &[]), auth_status),
        tool("auth_forget", "Drop a flow and its token from the server.", false, object(&[flow()], &["flow"]), auth_forget),
    ]
}
