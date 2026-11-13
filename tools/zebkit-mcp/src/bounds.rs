//! The bounds set by whoever runs the server: which tools, which origins,
//! which directories, with or without a window. Nothing an agent or a page
//! says can widen them: they are read once from the command line and never
//! written again.

use std::collections::BTreeSet;
use std::fs::{self, File, Metadata};
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
    /// --allow-local-network: a public name may be answered from this machine or
    /// a local network (`check_address` lets every address pass).
    pub local_network: bool,
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
        let real_parent = fs::canonicalize(parent).map_err(|e| match e.kind() {
            std::io::ErrorKind::NotFound => format!("{path}: its directory does not exist; make it first with file_mkdir"),
            kind => format!("{path}: its directory: {kind}"),
        })?;
        if !inside(&real_parent, &self.write_roots) {
            return Err(format!("{path}: outside the directories granted for writing"));
        }
        let real = real_parent.join(name);
        match fs::symlink_metadata(&real) {
            Ok(meta) if meta.file_type().is_symlink() => Err(format!("{path}: a symbolic link; refused")),
            Ok(meta) if !meta.is_file() => Err(format!("{path}: not a regular file")),
            Ok(meta) if many_links(&real, &meta) => Err(format!("{path}: has other hard links; refused")),
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

    /// Whether a response for `url` may come from `address`, the IP address it
    /// came from after DNS (the network hook's response stage, before the page
    /// has any of the body), as the names' check cannot say: a public name whose
    /// DNS answer is this machine or a local network (DNS rebinding) is refused.
    /// A host local by its name or address was allowed by the operator, by name,
    /// and passes; so does any address with --allow-local-network.
    ///
    /// Only what came over the network has an address: http and https (ws and
    /// wss, should they come here). A response of one of those without an
    /// address is refused, what cannot be told not passing. The engine does not
    /// say whether a response came from the HTTP cache; one that did carries the
    /// address it was fetched from, kept in the cache entry, and is checked by
    /// it. `data:` and `about:` are bytes the page has already and pass, as in
    /// `check_request`; `blob:` passes by its inner URL; any other scheme is
    /// refused (`check_request` has stopped it before).
    pub fn check_address(&self, url: &str, address: Option<&str>) -> Result<(), String> {
        if self.local_network {
            return Ok(());
        }
        let scheme = url.split(':').next().unwrap_or("").to_ascii_lowercase();
        match scheme.as_str() {
            "http" | "https" | "ws" | "wss" => {}
            "data" | "about" => return Ok(()),
            "blob" => return self.check_url(url.get(5..).unwrap_or("")).map(|_| ()),
            _ => return Err(format!("the scheme {scheme}: is refused; only http and https are allowed")),
        }
        let host = url_part(url, None, UrlPart::Host).unwrap_or_default();
        if !host.is_empty() && is_private_host(&host) {
            return Ok(());
        }
        let Some(address) = address else {
            return Err(format!("{host}: the network did not say which address answered; refused without --allow-local-network"));
        };
        match address.trim_start_matches('[').trim_end_matches(']').parse::<std::net::IpAddr>() {
            Ok(ip) if !is_local_address(ip) => Ok(()),
            Ok(ip) => Err(format!("{host} answered from {ip}, an address of this machine or a local network (DNS rebinding); refused without --allow-local-network")),
            Err(_) => Err(format!("{host}: answered from {}, not an IP address; refused", short(address))),
        }
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
    patterns.extend(["169.254.*.*", "192.168.*.*", "192.0.0.*", "192.0.2.*", "192.88.99.*", "198.18.*.*", "198.19.*.*", "198.51.100.*", "203.0.113.*"].map(str::to_string));
    patterns.extend((16..32).map(|second| format!("172.{second}.*.*")));
    patterns.extend((64..128).map(|second| format!("100.{second}.*.*")));
    patterns.extend((224..256).map(|first| format!("{first}.*.*.*")));
    // At least two colons: `fcfoo.example:443`, which the rules also try, has one.
    // Teredo and the other IETF blocks, documentation, discard, SRv6: as text,
    // so with each way its zeros may be written.
    patterns.extend(["2001::*", "2001:0:*:*", "2001:2::*", "2001:2:0:*", "2001:db8:*", "3fff::*", "3fff:?:*", "3fff:??:*", "3fff:???:*", "100::*", "100:0:0:1:*", "5f00:*:*"].map(str::to_string));
    patterns.extend(["::*", "64:ff9b:*:*", "2002:*:*", "fc*:*:*", "fd*:*:*", "fe8*:*:*", "fe9*:*:*", "fea*:*:*", "feb*:*:*", "fec*:*:*", "fed*:*:*", "fee*:*:*", "fef*:*:*", "ff*:*:*"].map(str::to_string));
    patterns
}

/// What was opened is still inside the grant, and is what the check saw.
fn recheck(file: &File, real: &Path, roots: &[PathBuf], path: &str) -> Result<(), String> {
    let again = fs::canonicalize(real).map_err(|e| format!("{path}: {}", e.kind()))?;
    if !inside(&again, roots) {
        return Err(format!("{path}: outside the directories granted"));
    }
    let opened = file.metadata().map_err(|e| e.to_string())?;
    if !opened.is_file() || !same_file(file, &again).map_err(|e| e.to_string())? {
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
/// (DNS rebinding) is not seen here: `check_address` holds that, at the response.
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
        return inner.parse::<std::net::Ipv6Addr>().map_or(true, private_v6);
    }
    false
}

/// An address of this machine, of a local network, or one no public host has,
/// by the IANA special-purpose address registries (IPv4 and IPv6): every block
/// they mark not globally reachable, and an IPv4 address of those inside an
/// IPv6 one. Loopback, private (10/8, 172.16/12, 192.168/16, fc00::/7),
/// link-local (169.254/16, fe80::/10, and the old fec0::/10), shared (100.64/10),
/// unspecified, "this network" (0/8), protocol assignments (192.0.0/24,
/// 2001::/23 but for its global blocks), documentation (192.0.2/24,
/// 198.51.100/24, 203.0.113/24, 2001:db8::/32, 3fff::/20), benchmarking
/// (198.18/15, 2001:2::/48), the old 6to4 relays (192.88.99/24), reserved
/// (240/4), broadcast, multicast, discard (100::/64), the dummy prefix
/// (100:0:0:1::/64), SRv6 (5f00::/16) and Teredo (2001::/32, whose server and
/// client addresses inside cannot be trusted to say where it goes).
pub fn is_local_address(ip: std::net::IpAddr) -> bool {
    match ip {
        std::net::IpAddr::V4(ip) => private_v4(ip),
        std::net::IpAddr::V6(ip) => private_v6(ip),
    }
}

fn private_v6(ip: std::net::Ipv6Addr) -> bool {
    let parts = ip.segments();
    let bytes = ip.octets();
    let inner = |a: u8, b: u8, c: u8, d: u8| private_v4(std::net::Ipv4Addr::new(a, b, c, d));
    let last = || inner(bytes[12], bytes[13], bytes[14], bytes[15]);
    if ip.is_loopback() || ip.is_unspecified() {
        return true;
    }
    // An IPv4 address inside: mapped (::ffff:a.b.c.d), translated (SIIT,
    // ::ffff:0:a.b.c.d), compatible (::a.b.c.d), NAT64 (64:ff9b::/96, by its
    // last 32 bits; the local-use 64:ff9b:1::/48 by RFC 6052's /48 layout, bits
    // 48-63 and 72-87, the octet between them being zero), 6to4 (2002:a.b:c.d::).
    if parts[..4] == [0; 4] && ((parts[4] == 0 && matches!(parts[5], 0 | 0xffff)) || (parts[4] == 0xffff && parts[5] == 0)) {
        return last();
    }
    if parts[..6] == [0x64, 0xff9b, 0, 0, 0, 0] {
        return last();
    }
    if parts[..3] == [0x64, 0xff9b, 1] {
        return inner(bytes[6], bytes[7], bytes[9], bytes[10]);
    }
    if parts[0] == 0x2002 {
        return inner(bytes[2], bytes[3], bytes[4], bytes[5]);
    }
    // 2001::/23, IETF protocol assignments: not global but for the blocks the
    // registry marks so. Teredo (2001::/32) and benchmarking (2001:2::/48) are
    // among those that are not.
    if parts[0] == 0x2001 && parts[1] < 0x200 {
        let global = matches!(parts, [0x2001, 1, 0, 0, 0, 0, 0, 1..=3])
            || parts[1] == 3
            || parts[..3] == [0x2001, 4, 0x112]
            || (0x20..0x40).contains(&parts[1]);
        return !global;
    }
    let first = parts[0];
    // Documentation, discard-only, the dummy prefix, SRv6 SIDs; unique local,
    // link-local and the old site-local, multicast.
    (first == 0x2001 && parts[1] == 0xdb8)
        || (first == 0x3fff && parts[1] < 0x1000)
        || (first == 0x100 && parts[1..3] == [0, 0] && parts[3] <= 1)
        || first == 0x5f00
        || first & 0xfe00 == 0xfc00
        || first & 0xffc0 == 0xfe80
        || first & 0xffc0 == 0xfec0
        || first & 0xff00 == 0xff00
}

fn private_v4(ip: std::net::Ipv4Addr) -> bool {
    let [a, b, c, d] = ip.octets();
    ip.is_loopback() || ip.is_private() || ip.is_link_local() || ip.is_unspecified() || a == 0
        || (a == 100 && (64..128).contains(&b))
        // Protocol assignments, documentation, the old 6to4 relays, benchmarking.
        // (192.0.0.9 and .10, anycast, are global).
        || (a == 192 && b == 0 && ((c == 0 && d != 9 && d != 10) || c == 2))
        || (a == 192 && b == 88 && c == 99)
        || (a == 198 && b == 51 && c == 100)
        || (a == 203 && b == 0 && c == 113)
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

// O_NOFOLLOW and O_NONBLOCK (a pipe swapped in must not block the open): std
// has no names for them, and their values differ by system and architecture.
#[cfg(any(target_os = "macos", target_os = "ios", target_os = "freebsd", target_os = "openbsd", target_os = "netbsd"))]
const NO_FOLLOW_FLAGS: i32 = 0x100 | 0x4;
#[cfg(all(any(target_os = "linux", target_os = "android"),
          any(target_arch = "x86_64", target_arch = "x86", target_arch = "riscv64", target_arch = "loongarch64", target_arch = "s390x")))]
const NO_FOLLOW_FLAGS: i32 = 0o400000 | 0o4000;
#[cfg(all(any(target_os = "linux", target_os = "android"),
          any(target_arch = "aarch64", target_arch = "arm", target_arch = "powerpc", target_arch = "powerpc64")))]
const NO_FOLLOW_FLAGS: i32 = 0o100000 | 0o4000;
#[cfg(all(unix, not(any(target_os = "macos", target_os = "ios", target_os = "freebsd", target_os = "openbsd", target_os = "netbsd",
          all(any(target_os = "linux", target_os = "android"),
              any(target_arch = "x86_64", target_arch = "x86", target_arch = "riscv64", target_arch = "loongarch64",
                  target_arch = "s390x", target_arch = "aarch64", target_arch = "arm", target_arch = "powerpc",
                  target_arch = "powerpc64"))))))]
compile_error!("O_NOFOLLOW and O_NONBLOCK are not known for this target: add them to bounds.rs");

#[cfg(unix)]
fn open_no_follow(path: &Path, write: bool, create_new: bool) -> std::io::Result<File> {
    use std::os::unix::fs::OpenOptionsExt;
    let mut options = fs::OpenOptions::new();
    options.read(!write).write(write).create_new(create_new).custom_flags(NO_FOLLOW_FLAGS);
    if write {
        options.mode(0o600);
    }
    options.open(path)
}

#[cfg(windows)]
fn open_no_follow(path: &Path, write: bool, create_new: bool) -> std::io::Result<File> {
    crate::win::open_no_follow(path, write, create_new)
}

/// Whether file, opened, is the file at path.
#[cfg(unix)]
fn same_file(file: &File, path: &Path) -> std::io::Result<bool> {
    use std::os::unix::fs::MetadataExt;
    let (a, b) = (file.metadata()?, fs::metadata(path)?);
    Ok(a.dev() == b.dev() && a.ino() == b.ino())
}

#[cfg(windows)]
fn same_file(file: &File, path: &Path) -> std::io::Result<bool> {
    let (a, b) = (crate::win::identity(file)?, crate::win::identity_at(path)?);
    Ok(!b.reparse && a.volume == b.volume && a.index == b.index)
}

/// Whether the file at path has other names (hard links).
#[cfg(unix)]
fn many_links(_path: &Path, meta: &Metadata) -> bool {
    use std::os::unix::fs::MetadataExt;
    meta.nlink() > 1
}

#[cfg(windows)]
fn many_links(path: &Path, _meta: &Metadata) -> bool {
    // What cannot be told is refused.
    crate::win::identity_at(path).map_or(true, |identity| identity.links > 1)
}

#[cfg(not(any(unix, windows)))]
compile_error!("the bounds' file checks are written for unix and Windows");

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::IpAddr;

    fn local(address: &str) -> bool {
        is_local_address(address.parse::<IpAddr>().expect(address))
    }

    #[test]
    fn local_addresses() {
        for address in [
            "127.0.0.1", "127.255.0.9", "10.0.0.1", "10.255.255.255", "172.16.0.1", "172.31.255.254", "192.168.1.1",
            "169.254.169.254", "100.64.0.1", "100.127.255.255", "0.0.0.0", "0.1.2.3", "224.0.0.1", "255.255.255.255",
            "::1", "::", "fc00::1", "fd12:3456::1", "fe80::1", "febf::1", "fec0::1", "ff02::1",
            "::ffff:127.0.0.1", "::ffff:10.1.2.3", "::ffff:169.254.169.254", "::ffff:100.64.0.1", "::ffff:0.0.0.0",
            "::127.0.0.1", "64:ff9b::7f00:1", "2002:c0a8:101::",
            // The review's gaps: Teredo (any), SIIT, local-use NAT64 by its /48 layout.
            "2001::1", "2001:0:4136:e378:8000:63bf:3fff:fdd2", "2001:0:808:808::", "::ffff:0:127.0.0.1",
            "::ffff:0:10.0.0.1", "::ffff:0:7f00:1", "64:ff9b:1:7f00:0:100::", "64:ff9b:1:7f00:0:100:808:808", "64:ff9b:1:a00:1::", "64:ff9b:1:c0a8:1:100::",
            // The IANA registries: protocol assignments, documentation, benchmarking,
            // 6to4 relays, discard, the dummy prefix, SRv6, ORCHID.
            "192.0.0.1", "192.0.0.170", "192.0.2.1", "198.51.100.7", "203.0.113.255", "192.88.99.1", "198.18.0.1", "198.19.255.255",
            "240.0.0.1", "2001:2::1", "2001:10::1", "2001:db8::1", "3fff::1", "3fff:fff:ffff::1", "100::1", "100::1:2:3:4",
            "100:0:0:1::1", "5f00::1", "fec0::1", "::ffff:192.0.2.1", "64:ff9b::c000:201", "2002:c000:201::",
        ] {
            assert!(local(address), "{address} is local");
        }
        for address in [
            "8.8.8.8", "1.1.1.1", "93.184.216.34", "172.15.255.255", "172.32.0.1", "100.63.255.255", "100.128.0.1",
            "169.253.0.1", "192.169.0.1", "11.0.0.1", "2606:4700::1111", "2001:4860:4860::8888", "::ffff:8.8.8.8",
            "64:ff9b::808:808", "2002:808:808::",
            // Public by the registries; the IPv4 inside the /48 local-use NAT64.
            "64:ff9b:1:808:8:800::", "::ffff:0:8.8.8.8", "192.0.0.9", "192.0.0.10", "192.31.196.1", "192.52.193.1",
            "192.175.48.1", "2001:1::1", "2001:1::2", "2001:1::3", "2001:3::1", "2001:4:112::1", "2001:20::1",
            "2001:30::1", "2001:200::1", "2620:4f:8000::1", "3fff:1000::1", "100:0:0:2::1", "2400:cb00::1",
        ] {
            assert!(!local(address), "{address} is public");
        }
    }

    #[test]
    fn addresses_by_scheme() {
        let bounds = Bounds { origins: vec![OriginRule::Exact("http://127.0.0.1:8000".to_string())], ..Bounds::default() };
        // A data: or about: document (a child frame) has no address, and needs none.
        assert!(bounds.check_address("data:text/html,<p>x", None).is_ok());
        assert!(bounds.check_address("DATA:text/plain,x", None).is_ok());
        assert!(bounds.check_address("about:srcdoc", None).is_ok());
        // blob: by its inner URL, as a request.
        assert!(bounds.check_address("blob:http://127.0.0.1:8000/0b1c", None).is_ok());
        assert!(bounds.check_address("blob:https://example.com/0b1c", None).is_err());
        assert!(bounds.check_address("blob:null/0b1c", None).is_err());
        // http, https, ws, wss: a public name needs a public address; none fails closed.
        for url in ["http://example.com/", "https://example.com/", "ws://example.com/", "wss://example.com/"] {
            assert!(bounds.check_address(url, Some("93.184.216.34")).is_ok(), "{url}");
            assert!(bounds.check_address(url, Some("[2606:4700::1111]")).is_ok(), "{url}");
            assert!(bounds.check_address(url, Some("127.0.0.1")).is_err(), "{url}");
            assert!(bounds.check_address(url, Some("[::ffff:0:7f00:1]")).is_err(), "{url}");
            assert!(bounds.check_address(url, Some("2001::1")).is_err(), "{url}");
            assert!(bounds.check_address(url, None).is_err(), "{url}");
            assert!(bounds.check_address(url, Some("proxy")).is_err(), "{url}");
        }
        // A host local by its own name or address passes, whatever answered.
        assert!(bounds.check_address("http://127.0.0.1:8000/", Some("127.0.0.1")).is_ok());
        assert!(bounds.check_address("http://localhost/", None).is_ok());
        assert!(bounds.check_address("http://[2001:db8::1]/", Some("2001:db8::1")).is_ok());
        // Other schemes never reach this stage; if they did, refused.
        assert!(bounds.check_address("file:///etc/passwd", None).is_err());
        assert!(bounds.check_address("chrome://settings", None).is_err());
    }

    #[test]
    fn private_hosts() {
        for host in ["localhost", "a.localhost", "printer", "127.0.0.1", "[::1]", "[2001::1]", "[::ffff:0:a00:1]", "[2001:db8::1]", "192.0.2.1"] {
            assert!(is_private_host(host), "{host}");
        }
        for host in ["example.com", "8.8.8.8", "[2001:4860:4860::8888]", "[64:ff9b:1:808:8:800::]"] {
            assert!(!is_private_host(host), "{host}");
        }
    }

    #[test]
    fn resolver_patterns() {
        let rules = host_resolver_rules(&["*".to_string()], &[]).unwrap();
        for pattern in ["MAP 2001::* ~NOTFOUND", "MAP 2001:0:*:* ~NOTFOUND", "MAP 2001:db8:* ~NOTFOUND", "MAP 192.0.2.* ~NOTFOUND", "MAP 64:ff9b:*:* ~NOTFOUND"] {
            assert!(rules.contains(pattern), "{pattern}");
        }
        assert!(!rules.contains("MAP 2001:*:* "));
    }

    #[test]
    fn addresses_with_the_switch() {
        let bounds = Bounds { local_network: true, ..Bounds::default() };
        assert!(bounds.check_address("https://example.com/", Some("127.0.0.1")).is_ok());
        assert!(bounds.check_address("https://example.com/", None).is_ok());
    }
}
