//! The bounds set by whoever runs the server: which tools, which origins,
//! which directories, with or without a window. Nothing an agent or a page
//! says can widen them: they are read once from the command line and never
//! written again.

use std::collections::BTreeSet;
use std::fs::{self, File, Metadata, OpenOptions};
use std::io::{Read, Write};
use std::path::{Component, Path, PathBuf};
use zebkit::services::{url_part, UrlPart};

/// What read and write take at most from a file, unless a tool's limit is lower.
pub const MAX_FILE: u64 = 256 * 1024 * 1024;

/// One entry of --allow-origin.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum OriginRule {
    /// `*`: any public http or https origin; not loopback or private addresses.
    Public,
    /// `https://example.com`, as the browser canonicalizes it.
    Exact(String),
    /// `https://*.example.com[:port]`: a subdomain, not the name itself.
    Subdomains { scheme: String, suffix: String, port: String },
}

#[derive(Debug, Default)]
pub struct Bounds {
    pub tools: BTreeSet<String>,
    pub origins: Vec<OriginRule>,
    pub read_roots: Vec<PathBuf>,
    pub write_roots: Vec<PathBuf>,
    pub windows: bool,
    /// Hosts of the OAuth providers configured by the operator: resolved for
    /// the device flow; pages and net_fetch are still held to `origins`.
    pub oauth_hosts: Vec<String>,
    /// Keys settings_set may change: none unless the operator names them.
    pub settings: BTreeSet<String>,
}

impl Bounds {
    /// Adds a directory of --allow-read or --allow-write. It must exist; symbolic
    /// links are resolved now, so that later checks compare real paths.
    pub fn grant(&mut self, directory: &str, write: bool) -> Result<(), String> {
        let path = Path::new(directory);
        if !path.is_absolute() {
            return Err(format!("{directory}: a granted directory must be an absolute path"));
        }
        let real = fs::canonicalize(path).map_err(|e| format!("{directory}: {e}"))?;
        if !real.is_dir() {
            return Err(format!("{directory}: not a directory"));
        }
        if real.parent().is_none() {
            return Err(format!("{directory}: granting the whole file system is refused"));
        }
        self.read_roots.push(real.clone());
        if write {
            self.write_roots.push(real);
        }
        Ok(())
    }

    /// A path to read: real, inside a read grant, a regular file (or a directory
    /// when `directory`). Fails unless all of that holds.
    pub fn read_path(&self, path: &str, directory: bool) -> Result<PathBuf, String> {
        let given = plain_path(path)?;
        if self.read_roots.is_empty() {
            return Err("file access is denied: the server was started without --allow-read or --allow-write".to_string());
        }
        let real = fs::canonicalize(given).map_err(|e| format!("{path}: {}", e.kind()))?;
        if !inside(&real, &self.read_roots) {
            return Err(format!("{path}: outside the directories granted"));
        }
        let meta = fs::metadata(&real).map_err(|e| format!("{path}: {}", e.kind()))?;
        if directory && !meta.is_dir() {
            return Err(format!("{path}: not a directory"));
        }
        if !directory && !meta.is_file() {
            return Err(format!("{path}: not a regular file"));
        }
        Ok(real)
    }

    /// A path to write: its parent real and inside a write grant, itself not a
    /// symbolic link, a directory or a special file. It need not exist.
    pub fn write_path(&self, path: &str) -> Result<PathBuf, String> {
        let given = plain_path(path)?;
        if self.write_roots.is_empty() {
            return Err("writing is denied: the server was started without --allow-write".to_string());
        }
        let name = given.file_name().ok_or_else(|| format!("{path}: no file name"))?;
        let parent = given.parent().ok_or_else(|| format!("{path}: no directory"))?;
        let real_parent = fs::canonicalize(parent).map_err(|e| format!("{path}: its directory: {}", e.kind()))?;
        if !inside(&real_parent, &self.write_roots) {
            return Err(format!("{path}: outside the directories granted for writing"));
        }
        let real = real_parent.join(name);
        match fs::symlink_metadata(&real) {
            Ok(meta) if meta.file_type().is_symlink() => Err(format!("{path}: a symbolic link; refused")),
            Ok(meta) if !meta.is_file() => Err(format!("{path}: not a regular file")),
            Ok(meta) if many_links(&meta) => Err(format!("{path}: has other hard links; refused")),
            _ => Ok(real),
        }
    }

    /// Reads a file granted. The file is opened without following a last link
    /// and checked again after opening, so that a link swapped in after the
    /// check is not followed out.
    pub fn read_file(&self, path: &str, limit: u64) -> Result<Vec<u8>, String> {
        let real = self.read_path(path, false)?;
        let mut file = open_no_follow(&real, false, false).map_err(|e| format!("{path}: {}", e.kind()))?;
        recheck(&file, &real, &self.read_roots, path)?;
        let size = file.metadata().map_err(|e| e.to_string())?.len();
        if size > limit {
            return Err(format!("{path}: {size} bytes, over the limit of {limit}"));
        }
        let mut bytes = Vec::with_capacity(size as usize);
        Read::by_ref(&mut file).take(limit + 1).read_to_end(&mut bytes).map_err(|e| format!("{path}: {}", e.kind()))?;
        if bytes.len() as u64 > limit {
            return Err(format!("{path}: over the limit of {limit} bytes"));
        }
        Ok(bytes)
    }

    /// Writes a file granted, through a new file in the same directory, which
    /// then replaces it: a link at the path is replaced, never written through.
    pub fn write_file(&self, path: &str, bytes: &[u8], overwrite: bool) -> Result<PathBuf, String> {
        let real = self.write_path(path)?;
        if !overwrite && fs::symlink_metadata(&real).is_ok() {
            return Err(format!("{path}: exists; pass overwrite to replace it"));
        }
        let parent = real.parent().ok_or_else(|| format!("{path}: no directory"))?;
        let temporary = parent.join(format!(".zebkit-mcp-{}", hex(&zebkit::services::random_bytes(12))));
        let result = (|| -> Result<(), String> {
            let mut file = open_no_follow(&temporary, true, true).map_err(|e| format!("{path}: {}", e.kind()))?;
            recheck(&file, &temporary, &self.write_roots, path)?;
            file.write_all(bytes).map_err(|e| format!("{path}: {}", e.kind()))?;
            if overwrite {
                fs::rename(&temporary, &real).map_err(|e| format!("{path}: {}", e.kind()))
            } else {
                // Fails when something appeared at the path in the meantime.
                fs::hard_link(&temporary, &real).map_err(|e| format!("{path}: {}", e.kind()))
            }
        })();
        let _ = fs::remove_file(&temporary);
        result.map(|_| real)
    }

    /// The URL as the browser reads it, when the bounds allow it for a request or
    /// a navigation; why not otherwise.
    pub fn check_url(&self, url: &str) -> Result<String, String> {
        let spec = url_part(url, None, UrlPart::Spec).ok_or_else(|| format!("not a valid URL: {}", short(url)))?;
        let scheme = url_part(&spec, None, UrlPart::Scheme).unwrap_or_default();
        if scheme != "http" && scheme != "https" {
            return Err(format!("the scheme {scheme}: is refused; only http and https are allowed"));
        }
        let host = url_part(&spec, None, UrlPart::Host).unwrap_or_default();
        let port = url_part(&spec, None, UrlPart::Port).unwrap_or_default();
        let origin = url_part(&spec, None, UrlPart::Origin).unwrap_or_default();
        for rule in &self.origins {
            let allowed = match rule {
                OriginRule::Public => !is_private_host(&host),
                OriginRule::Exact(exact) => *exact == origin,
                OriginRule::Subdomains { scheme: wanted, suffix, port: wanted_port } => {
                    *wanted == scheme
                        && *wanted_port == port
                        && host.len() > suffix.len() + 1
                        && host.ends_with(suffix.as_str())
                        && host.as_bytes()[host.len() - suffix.len() - 1] == b'.'
                }
            };
            if allowed {
                return Ok(spec);
            }
        }
        Err(format!("the origin {origin} is outside the origins allowed"))
    }

    /// Whether a document may be in a frame: an allowed origin, a blank page, or
    /// (in a child frame) something with no network of its own.
    pub fn check_document(&self, url: &str, main_frame: bool) -> Result<(), String> {
        if url == "about:blank" {
            return Ok(());
        }
        let scheme = url.split(':').next().unwrap_or("").to_ascii_lowercase();
        match scheme.as_str() {
            "about" | "data" if !main_frame => Ok(()),
            "blob" => self.check_url(url.get(5..).unwrap_or("")).map(|_| ()),
            _ => self.check_url(url).map(|_| ()),
        }
    }

    /// A Content-Security-Policy for documents and workers, naming where they may
    /// connect, WebSockets included. `*` can have none, a policy not being able to
    /// say "not private": the resolver rules hold it (`host_resolver_rules`).
    pub fn connect_policy(&self) -> Option<String> {
        let mut sources = Vec::new();
        for rule in &self.origins {
            let (scheme, host) = match rule {
                OriginRule::Public => return None,
                OriginRule::Exact(origin) => match origin.split_once("://") {
                    Some((scheme, host)) => (scheme.to_string(), host.to_string()),
                    None => continue,
                },
                OriginRule::Subdomains { scheme, suffix, port } => {
                    let port = if port.is_empty() { String::new() } else { format!(":{port}") };
                    (scheme.clone(), format!("*.{suffix}{port}"))
                }
            };
            sources.push(format!("{scheme}://{host}"));
            sources.push(format!("{}://{host}", if scheme == "https" { "wss" } else { "ws" }));
        }
        Some(format!("connect-src {} blob: data:", if sources.is_empty() { "'none'".to_string() } else { sources.join(" ") }))
    }

    /// A request of a page, or of its workers.
    pub fn check_request(&self, url: &str) -> Result<(), String> {
        let scheme = url.split(':').next().unwrap_or("").to_ascii_lowercase();
        match scheme.as_str() {
            // Bytes the page has already; nothing goes out.
            "data" | "about" => Ok(()),
            "blob" => self.check_url(url.get(5..).unwrap_or("")).map(|_| ()),
            _ => self.check_url(url).map(|_| ()),
        }
    }

}

/// The host of an entry of --allow-origin, by plain text: the rules below go on
/// the engine's command line, before the browser's URL parser may be used.
/// `parse_origin` then checks the browser reads the same host.
pub fn plain_host(origin: &str) -> Result<String, String> {
    let rest = origin.split_once("://").map_or(origin, |(_, rest)| rest);
    if !(origin.starts_with("http://") || origin.starts_with("https://")) {
        return Err(format!("{origin}: not an http or https origin, like https://example.com"));
    }
    if !origin.is_ascii() || rest.contains('@') {
        return Err(format!("{origin}: not an origin; write it as https://example.com, a name in punycode"));
    }
    let host = rest.trim_end_matches('/');
    let host = if host.starts_with('[') {
        host.split_once(']').map(|(ip, _)| format!("{ip}]")).unwrap_or_default()
    } else {
        host.split(':').next().unwrap_or("").to_string()
    };
    Ok(host.to_ascii_lowercase())
}

/// --host-resolver-rules for the whole network stack: what the view hooks do
/// not see (WebSockets, service and shared workers) still resolves only the
/// allowed names. Names, not ports or schemes: those are for the hooks.
pub fn host_resolver_rules(origins: &[String], oauth_hosts: &[String]) -> Result<String, String> {
    let public = origins.iter().any(|origin| origin == "*");
    let mut allowed = oauth_hosts.to_vec();
    for origin in origins.iter().filter(|origin| *origin != "*") {
        allowed.push(plain_host(origin)?);
    }
    allowed.retain(|host| !host.is_empty());
    // The rules see an IPv6 address without its brackets. An EXCLUDE holds for
    // every MAP, whatever their order.
    let excluded = allowed.iter().map(|host| format!("EXCLUDE {}", host.trim_start_matches('[').trim_end_matches(']')));
    let mut rules: Vec<String> = if public { private_host_patterns().iter().map(|pattern| format!("MAP {pattern} ~NOTFOUND")).collect() } else { vec!["MAP * ~NOTFOUND".to_string()] };
    rules.extend(excluded);
    Ok(rules.join(", "))
}

/// What `is_private_host` refuses, as patterns of the resolver's rules: what a
/// frame's hooks do not see under `*` (WebSockets, a preconnect) is held by these.
/// Patterns know no digits, so `10.a.b.c`, a public name of that shape, goes with
/// them; and they cannot say "a name with no dot".
fn private_host_patterns() -> Vec<String> {
    let mut patterns: Vec<String> = PRIVATE_SUFFIXES.iter().map(|suffix| format!("*{suffix}")).collect();
    patterns.push("localhost".to_string());
    for first in ["0", "10", "127"] {
        patterns.push(format!("{first}.*.*.*"));
    }
    patterns.extend(["169.254.*.*", "192.168.*.*", "192.0.0.*", "198.18.*.*", "198.19.*.*"].map(str::to_string));
    patterns.extend((16..32).map(|second| format!("172.{second}.*.*")));
    patterns.extend((64..128).map(|second| format!("100.{second}.*.*")));
    patterns.extend((224..256).map(|first| format!("{first}.*.*.*")));
    // At least two colons: `fcfoo.example:443`, which the rules also try, has one.
    patterns.extend(["::*", "64:ff9b:*:*", "2002:*:*", "fc*:*:*", "fd*:*:*", "fe8*:*:*", "fe9*:*:*", "fea*:*:*", "feb*:*:*", "fec*:*:*", "fed*:*:*", "fee*:*:*", "fef*:*:*", "ff*:*:*"].map(str::to_string));
    patterns
}

/// What was opened is still inside the grant, and is what the check saw.
fn recheck(file: &File, real: &Path, roots: &[PathBuf], path: &str) -> Result<(), String> {
    let again = fs::canonicalize(real).map_err(|e| format!("{path}: {}", e.kind()))?;
    if !inside(&again, roots) {
        return Err(format!("{path}: outside the directories granted"));
    }
    let (opened, named) = (file.metadata().map_err(|e| e.to_string())?, fs::metadata(&again).map_err(|e| e.to_string())?);
    if !opened.is_file() || !same_file(&opened, &named) {
        return Err(format!("{path}: changed while it was opened; refused"));
    }
    Ok(())
}

/// Parses an entry of --allow-origin.
pub fn parse_origin(text: &str) -> Result<OriginRule, String> {
    if text == "*" {
        return Ok(OriginRule::Public);
    }
    if let Some((scheme, rest)) = text.split_once("://*.") {
        let probe = format!("{scheme}://{rest}");
        let origin = url_part(&probe, None, UrlPart::Origin).ok_or_else(|| format!("{text}: not an origin"))?;
        let host = url_part(&origin, None, UrlPart::Host).unwrap_or_default();
        if (scheme != "http" && scheme != "https") || !host.contains('.') || is_ip_literal(&host) || rest.contains('/') {
            return Err(format!("{text}: not an origin with a domain of at least two labels"));
        }
        let port = url_part(&origin, None, UrlPart::Port).unwrap_or_default();
        return Ok(OriginRule::Subdomains { scheme: scheme.to_string(), suffix: host, port });
    }
    let origin = url_part(text, None, UrlPart::Origin).filter(|origin| origin != "null");
    let scheme = url_part(text, None, UrlPart::Scheme).unwrap_or_default();
    let path = url_part(text, None, UrlPart::Path).unwrap_or_default();
    match origin {
        Some(origin) if (scheme == "http" || scheme == "https") && (path.is_empty() || path == "/") => Ok(OriginRule::Exact(origin)),
        _ => Err(format!("{text}: not an http or https origin, like https://example.com")),
    }
}

fn is_ip_literal(host: &str) -> bool {
    host.starts_with('[') || host.parse::<std::net::Ipv4Addr>().is_ok()
}

/// Names only a local network has an answer for.
const PRIVATE_SUFFIXES: &[&str] = &[".localhost", ".local", ".internal", ".localdomain", ".lan", ".home", ".corp", ".intranet", ".home.arpa"];

/// A host `*` does not reach: names of this machine, private and link-local
/// addresses. The name is the browser's canonical one, so `2130706433` has been
/// read as 127.0.0.1 by now. A public name that resolves to a private address
/// (DNS rebinding) is not seen here.
pub fn is_private_host(host: &str) -> bool {
    let host = host.trim_end_matches('.').to_ascii_lowercase();
    if host == "localhost"
        || PRIVATE_SUFFIXES.iter().any(|suffix| host.ends_with(suffix))
        || (!host.contains('.') && !host.starts_with('['))
    {
        return true;
    }
    if let Ok(ip) = host.parse::<std::net::Ipv4Addr>() {
        return private_v4(ip);
    }
    if let Some(inner) = host.strip_prefix('[').and_then(|h| h.strip_suffix(']')) {
        let Ok(ip) = inner.parse::<std::net::Ipv6Addr>() else { return true };
        let parts = ip.segments();
        // An IPv4 address inside: mapped (::ffff:a.b.c.d), compatible (::a.b.c.d),
        // NAT64 (64:ff9b::a.b.c.d), 6to4 (2002:a.b:c.d::).
        let (hi, lo) = (parts[6], parts[7]);
        let inner = |hi: u16, lo: u16| private_v4(std::net::Ipv4Addr::from(((hi as u32) << 16) | lo as u32));
        if parts[..5] == [0; 5] && (parts[5] == 0xffff || parts[5] == 0) && !ip.is_loopback() && !ip.is_unspecified() {
            return inner(hi, lo);
        }
        if parts[0] == 0x64 && parts[1] == 0xff9b {
            return inner(hi, lo);
        }
        if parts[0] == 0x2002 {
            return inner(parts[1], parts[2]);
        }
        let first = parts[0];
        // Loopback, unique local, link-local and the old site-local, multicast.
        return ip.is_loopback() || ip.is_unspecified() || first & 0xfe00 == 0xfc00 || first & 0xff80 == 0xfe80 || first & 0xffc0 == 0xfec0 || first & 0xff00 == 0xff00;
    }
    false
}

fn private_v4(ip: std::net::Ipv4Addr) -> bool {
    let [a, b, c, _] = ip.octets();
    ip.is_loopback() || ip.is_private() || ip.is_link_local() || ip.is_unspecified() || a == 0
        || (a == 100 && (64..128).contains(&b))
        || (a == 192 && b == 0 && c == 0)
        || (a == 198 && (b == 18 || b == 19))
        // Multicast, reserved, broadcast.
        || a >= 224
}

fn short(text: &str) -> String {
    text.chars().take(200).collect()
}

/// An absolute path with nothing that a later reader of it could take otherwise.
fn plain_path(path: &str) -> Result<&Path, String> {
    if path.is_empty() || path.contains('\0') {
        return Err("a path must not be empty or hold a zero byte".to_string());
    }
    let given = Path::new(path);
    if !given.is_absolute() {
        return Err(format!("{path}: must be an absolute path"));
    }
    if given.components().any(|part| part == Component::ParentDir) {
        return Err(format!("{path}: .. is refused"));
    }
    Ok(given)
}

fn inside(real: &Path, roots: &[PathBuf]) -> bool {
    roots.iter().any(|root| real.starts_with(root))
}

pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|byte| format!("{byte:02x}")).collect()
}

#[cfg(unix)]
fn open_no_follow(path: &Path, write: bool, create_new: bool) -> std::io::Result<File> {
    use std::os::unix::fs::OpenOptionsExt;
    // O_NOFOLLOW and O_NONBLOCK (a pipe swapped in must not block the open):
    // std has no names for them.
    #[cfg(target_os = "macos")]
    const FLAGS: i32 = 0x100 | 0x4;
    #[cfg(not(target_os = "macos"))]
    const FLAGS: i32 = 0o400000 | 0o4000;
    let mut options = OpenOptions::new();
    options.read(!write).write(write).create_new(create_new).custom_flags(FLAGS);
    if write {
        options.mode(0o600);
    }
    options.open(path)
}

#[cfg(not(unix))]
fn open_no_follow(path: &Path, write: bool, create_new: bool) -> std::io::Result<File> {
    OpenOptions::new().read(!write).write(write).create_new(create_new).open(path)
}

#[cfg(unix)]
fn same_file(a: &Metadata, b: &Metadata) -> bool {
    use std::os::unix::fs::MetadataExt;
    a.dev() == b.dev() && a.ino() == b.ino()
}

#[cfg(not(unix))]
fn same_file(a: &Metadata, b: &Metadata) -> bool {
    a.len() == b.len()
}

#[cfg(unix)]
fn many_links(meta: &Metadata) -> bool {
    use std::os::unix::fs::MetadataExt;
    meta.nlink() > 1
}

#[cfg(not(unix))]
fn many_links(_meta: &Metadata) -> bool {
    false
}
