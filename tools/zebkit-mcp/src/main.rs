//! zebkit-mcp: the engine's capabilities as MCP tools for an AI agent, over
//! standard input and over HTTP, within bounds its operator sets on the command
//! line. docs/MCP.md is the manual; `--help` the short form.
//!
//! The engine has one thread. Transports read on threads of their own; a tool
//! that needs the engine is posted to its thread, and any other runs on the
//! request's thread.

mod bounds;
mod http;
mod rpc;
mod schema;
mod stdio;
mod tools;
#[cfg(windows)]
mod win;

use bounds::Bounds;
use std::collections::BTreeSet;
use std::net::IpAddr;
use std::sync::OnceLock;

static BOUNDS: OnceLock<Bounds> = OnceLock::new();
/// The server's directory in the system's temporary one: the engine's temporary
/// files, and its profile unless --profile gave one. Gone at any end but a SIGKILL,
/// and then the next start removes it (`remove_abandoned`).
static TEMPORARY: OnceLock<std::path::PathBuf> = OnceLock::new();
/// The engine's loop runs: it can be asked to quit.
static ENGINE_RUNS: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// A start refused from inside the engine, which then ends as it always does.
static REFUSED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
/// The operator's profile is being kept: its data is worth waiting for.
static KEEPS_PROFILE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// A process of the engine's may still be writing as it goes: a few tries.
fn remove_temporary() {
    if let Some(directory) = TEMPORARY.get() {
        for _ in 0..10 {
            if std::fs::remove_dir_all(directory).is_ok() || !directory.exists() {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
    }
}

/// When the engine's end does not come: the process goes, its data with it. A client
/// gives a server about 4 seconds from closing its input before SIGKILL, which would
/// leave the temporary directory, so with nothing to keep this comes before that.
fn exit_soon() {
    std::thread::spawn(|| {
        let keeps = KEEPS_PROFILE.load(std::sync::atomic::Ordering::SeqCst);
        std::thread::sleep(std::time::Duration::from_secs(if keeps { 15 } else { 3 }));
        remove_temporary();
        std::process::exit(if REFUSED.load(std::sync::atomic::Ordering::SeqCst) { 2 } else { 0 });
    });
}

/// `fail` for the engine's thread: exiting there would race its other threads, still
/// writing into the directory being removed.
fn refuse_in_engine(message: &str) {
    eprintln!("zebkit-mcp: {message}");
    REFUSED.store(true, std::sync::atomic::Ordering::SeqCst);
    exit_soon();
    zebkit::post(zebkit::quit);
}

/// The name of a server's directory says whose it is.
const TEMPORARY_PREFIX: &str = "zebkit-mcp-";

/// A directory of a server that was killed, whose process is not there any more.
/// Only this user's can be removed, in a temporary directory that is sticky or private.
#[cfg(unix)]
fn remove_abandoned(parent: &std::path::Path) {
    extern "C" {
        fn kill(pid: i32, signal: i32) -> i32;
    }
    const ESRCH: i32 = 3;
    for entry in std::fs::read_dir(parent).into_iter().flatten().flatten() {
        let name = entry.file_name();
        let Some(pid) = name.to_str().and_then(|name| name.strip_prefix(TEMPORARY_PREFIX)).and_then(|rest| rest.split_once('-')).and_then(|(pid, _)| pid.parse::<i32>().ok()) else { continue };
        let gone = pid > 0 && unsafe { kill(pid, 0) } != 0 && std::io::Error::last_os_error().raw_os_error() == Some(ESRCH);
        if gone && entry.file_type().is_ok_and(|kind| kind.is_dir()) {
            let _ = std::fs::remove_dir_all(entry.path());
        }
    }
}

#[cfg(not(unix))]
fn remove_abandoned(_: &std::path::Path) {}

fn make_temporary() -> std::path::PathBuf {
    let parent = std::env::temp_dir();
    remove_abandoned(&parent);
    let directory = parent.join(format!("{TEMPORARY_PREFIX}{}-{}", std::process::id(), bounds::hex(&zebkit::services::random_bytes(6))));
    #[cfg(unix)]
    {
        use std::os::unix::fs::DirBuilderExt;
        std::fs::DirBuilder::new().mode(0o700).create(&directory).unwrap_or_else(|why| fail(&format!("{}: {why}", directory.display())));
    }
    #[cfg(not(unix))]
    std::fs::create_dir(&directory).unwrap_or_else(|why| fail(&format!("{}: {why}", directory.display())));
    let _ = TEMPORARY.set(directory.clone());
    directory
}

/// The bounds, once the server serves.
pub fn bounds() -> &'static Bounds {
    BOUNDS.get().expect("the bounds are set before anything is served")
}

const HELP: &str = "\
zebkit-mcp: the engine's capabilities as MCP tools, within the bounds given here.

Transports (standard input and output when neither is given):
  --stdio                     Serve on standard input and output.
  --http PORT                 Serve Streamable HTTP at /mcp (0 picks a port).
  --http-bind ADDRESS         Default 127.0.0.1. Any other than a loopback address
                              also needs --http-insecure-remote: there is no TLS.
  --http-insecure-remote      Allow that.
  --http-token-file FILE      The bearer token: read from FILE, private to its owner,
                              or generated into it. Without it, one is generated
                              and printed once on standard error. On Windows a file
                              made here is its owner's only; the access of one that
                              is there is not checked.
  --http-allow-origin ORIGIN  A web client's Origin to accept. By default a request
                              with an Origin other than this server's own is refused.
  --http-allow-host NAME      A Host name besides the loopback names.

Bounds (everything is denied that is not given):
  --tools LIST                Groups or tools, by comma; default all but none widen the
                              rest. Groups: pages pdf files media ui auth settings.
  --deny-tools LIST           Tools or groups taken away from those.
  --allow-origin ORIGIN       A web origin pages and net_fetch may reach, for every
                              request: https://example.com, https://*.example.com, or *
                              for all public ones: not this machine's or private
                              names and addresses. Repeatable. None: no page can
                              load anything. A response from an address of this
                              machine or a local network (loopback, private,
                              link-local, 100.64/10, ...) to a name that is not
                              one of those (DNS rebinding) is refused once its
                              headers come, before the page has any of it. Its
                              request has been sent by then. Chromium's Local
                              Network Access refuses a public page's requests to
                              local addresses before they are sent (fetch, POST,
                              beacons, images, frames, forms into frames), but
                              not its own top-level navigation: a form POST to
                              the page's own frame goes out, its answer refused.
                              A page whose own response was refused never loads,
                              so sends nothing more. All this holds unless:
  --allow-local-network       Public names may be answered from local addresses,
                              as behind a proxy (whose address the network sees)
                              or with names of a local network's DNS; Local
                              Network Access is granted too. Refused with
                              --allow-origin '*': any public page could then
                              reach this machine's and the local network's
                              services through a name that resolves there.
  --allow-local-network-for-any-origin
                              --allow-local-network, with '*' too: only behind a
                              proxy that resolves names itself, with no service
                              of its own or of its network a page must not reach.
  --allow-read DIRECTORY      Files may be read under it. Repeatable. None: none.
  --allow-write DIRECTORY     Files may be read and written under it. Repeatable.
  --windows                   Pages and zebkit.ui have real windows. Default: none.
  --oauth-config FILE         Providers for the device flow (docs/MCP.md).
  --allow-settings LIST       Keys settings_set may change, by comma: thirdPartyCookies
                              dialogs downloads newWindows spareRenderer viewport. None:
                              it refuses them all. downloads and newWindows can then
                              only narrow; nothing here widens the bounds above.

The engine:
  --profile DIRECTORY         Its data, kept. Default: a private temporary directory.
  -- SWITCHES                 Chromium's switches, after --, as --no-sandbox. Those
                              known to pass the bounds are refused; the list is a
                              guard for the operator, not a boundary: give only
                              switches you understand.

  --list-tools                Print the tools as JSON and exit.
  --version, --help
";

struct Options {
    stdio: bool,
    http: Option<http::Config>,
    token_file: Option<String>,
    origins: Vec<String>,
    oauth_config: Option<String>,
    profile: Option<String>,
    engine_switches: Vec<String>,
}

fn fail(message: &str) -> ! {
    eprintln!("zebkit-mcp: {message}");
    remove_temporary();
    std::process::exit(2);
}

fn parse_port(text: &str) -> u16 {
    text.parse().unwrap_or_else(|_| fail(&format!("{text}: not a port")))
}

fn parse(args: &[String], bounds: &mut Bounds) -> Options {
    let mut options = Options {
        stdio: false,
        http: None,
        token_file: None,
        origins: Vec::new(),
        oauth_config: None,
        profile: None,
        engine_switches: Vec::new(),
    };
    let (mut port, mut bind, mut remote) = (None, "127.0.0.1".to_string(), false);
    let (mut allow_origins, mut allow_hosts) = (Vec::new(), Vec::new());
    let (mut tools_given, mut denied): (Option<String>, Vec<String>) = (None, Vec::new());
    let mut any_origin_local = false;
    let mut iterator = args.iter();
    while let Some(arg) = iterator.next() {
        let (name, inline) = match arg.split_once('=') {
            Some((name, value)) if name.starts_with("--") => (name, Some(value.to_string())),
            _ => (arg.as_str(), None),
        };
        let mut value = || -> String {
            inline.clone().or_else(|| iterator.next().cloned()).unwrap_or_else(|| fail(&format!("{name} needs a value")))
        };
        match name {
            "--help" | "-h" => {
                print!("{HELP}");
                std::process::exit(0);
            }
            "--version" => {
                println!("zebkit-mcp {}", env!("CARGO_PKG_VERSION"));
                std::process::exit(0);
            }
            "--list-tools" => {
                let all: Vec<_> = tools::all().iter().map(tools::describe).collect();
                println!("{}", serde_json::to_string_pretty(&all).unwrap_or_default());
                std::process::exit(0);
            }
            "--stdio" => options.stdio = true,
            "--http" => port = Some(parse_port(&value())),
            "--http-bind" => bind = value(),
            "--http-insecure-remote" => remote = true,
            "--http-token-file" => options.token_file = Some(value()),
            "--http-allow-origin" => allow_origins.push(value()),
            "--http-allow-host" => allow_hosts.push(value()),
            "--tools" => tools_given = Some(value()),
            "--deny-tools" => denied.extend(value().split(',').map(str::to_string)),
            "--allow-origin" => options.origins.push(value()),
            "--allow-read" => bounds.grant(&value(), false).unwrap_or_else(|why| fail(&why)),
            "--allow-write" => bounds.grant(&value(), true).unwrap_or_else(|why| fail(&why)),
            "--allow-local-network" => bounds.local_network = true,
            "--allow-local-network-for-any-origin" => (bounds.local_network, any_origin_local) = (true, true),
            "--windows" => bounds.windows = true,
            "--oauth-config" => options.oauth_config = Some(value()),
            "--allow-settings" => {
                for key in value().split(',').map(str::trim).filter(|key| !key.is_empty()) {
                    if !tools::settings::KEYS.contains(&key) {
                        fail(&format!("--allow-settings {key}: not a setting; those are {}", tools::settings::KEYS.join(" ")));
                    }
                    bounds.settings.insert(key.to_string());
                }
            }
            "--profile" => options.profile = Some(value()),
            "--" => {
                options.engine_switches.extend(iterator.by_ref().cloned());
                break;
            }
            _ => fail(&format!("{arg}: unknown; see --help")),
        }
    }
    // Under '*', a rebinding name of any public page would reach local services,
    // Local Network Access granted: not without saying so by name.
    if bounds.local_network && !any_origin_local && options.origins.iter().any(|origin| origin == "*") {
        fail("--allow-local-network with --allow-origin '*' is refused: any public page could then reach this machine's and the local network's services through a name that resolves there. Name the origins, or, only behind a proxy that resolves names itself, give --allow-local-network-for-any-origin");
    }
    for switch in &options.engine_switches {
        // Chromium takes `-name` as it takes `--name`; anything without a dash is
        // not a switch. Programs it would start count too.
        // Prefixes, so that each takes its variants (remote-debugging-port and
        // -pipe and -address, ignore-certificate-errors-spki-list, ...):
        let refused = [
            // Another way in: CDP, Playwright, the profile, the network's routing.
            "remote-debugging", "remote-allow-origins", "host-resolver-rules", "host-rules", "user-data-dir", "proxy-", "no-proxy-server", "playwright-port",
            // Which addresses count as local: Chromium's Local Network Access, behind
            // the bounds' own check of the address that answered (check_address).
            "ip-address-space-overrides",
            // The web's own bounds: the same-origin policy, site isolation, V8.
            "disable-web-security", "disable-site-isolation", "single-process", "js-flags", "allow-file-access",
            // Features the bounds do not know: MojoJS (enable-blink-features=MojoJS)
            // gives pages the browser's own interfaces.
            "disable-features", "enable-features", "enable-blink-features", "force-webrtc-ip-handling-policy",
            // TLS and mixed content: certificates not checked, http taken for a
            // secure origin, http inside https pages, localhost's certificates.
            "ignore-certificate-errors", "unsafely-treat-insecure-origin-as-secure", "allow-running-insecure-content", "allow-insecure-localhost",
            // Files the engine would write outside --allow-write: the network's
            // log, TLS keys, its log, its cache.
            "log-net-log", "ssl-key-log-file", "log-file", "disk-cache-dir",
            // Code the engine would load or programs it would start.
            "load-extension", "disable-extensions-except", "renderer-cmd-prefix", "utility-cmd-prefix", "gpu-launcher", "zygote-cmd-prefix", "browser-subprocess-path",
        ];
        let name = switch.trim_start_matches('-').to_ascii_lowercase();
        if !switch.starts_with('-') || refused.iter().any(|refused| name.starts_with(refused)) {
            fail(&format!("{switch}: refused, it would pass the bounds; see docs/MCP.md"));
        }
    }
    bounds.tools = select_tools(tools_given.as_deref(), &denied);
    if let Some(port) = port {
        let address: IpAddr = bind.parse().unwrap_or_else(|_| fail(&format!("{bind}: not an IP address")));
        if !address.is_loopback() && !remote {
            fail(&format!("--http-bind {bind}: not a loopback address; this needs --http-insecure-remote, and there is no TLS"));
        }
        options.http = Some(http::Config { bind: address, port, token: String::new(), allow_origins, allow_hosts });
    }
    if options.http.is_none() {
        options.stdio = true;
    }
    options
}

fn select_tools(given: Option<&str>, denied: &[String]) -> BTreeSet<String> {
    let all = tools::all();
    let names = |list: &str| -> BTreeSet<String> {
        let mut found = BTreeSet::new();
        for item in list.split(',').map(str::trim).filter(|item| !item.is_empty()) {
            let before = found.len();
            for tool in all.iter().filter(|tool| tool.name == item || tool.group == item) {
                found.insert(tool.name.to_string());
            }
            if found.len() == before {
                fail(&format!("{item}: neither a tool nor a group"));
            }
        }
        found
    };
    let mut selected = match given {
        Some(list) => names(list),
        None => all.iter().map(|tool| tool.name.to_string()).collect(),
    };
    for item in denied {
        for name in names(item) {
            selected.remove(&name);
        }
    }
    selected
}

/// The bearer token, from a private file or new.
fn token(file: Option<&str>) -> (String, bool) {
    let new = || bounds::hex(&zebkit::services::random_bytes(32));
    let Some(path) = file else { return (new(), true) };
    match std::fs::read_to_string(path) {
        Ok(text) => {
            #[cfg(unix)]
            {
                use std::os::unix::fs::MetadataExt;
                let mode = std::fs::metadata(path).map(|meta| meta.mode()).unwrap_or(0o777);
                if mode & 0o077 != 0 {
                    fail(&format!("{path}: readable by others; chmod 600 it"));
                }
            }
            let text = text.trim().to_string();
            if text.len() < 16 {
                fail(&format!("{path}: a token of at least 16 characters is needed"));
            }
            (text, false)
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            let text = new();
            write_private(path, &text).unwrap_or_else(|why| fail(&format!("{path}: {why}")));
            (text, false)
        }
        Err(error) => fail(&format!("{path}: {error}")),
    }
}

#[cfg(unix)]
fn write_private(path: &str, text: &str) -> std::io::Result<()> {
    use std::io::Write;
    use std::os::unix::fs::OpenOptionsExt;
    std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(path)?.write_all(text.as_bytes())
}

#[cfg(windows)]
fn write_private(path: &str, text: &str) -> std::io::Result<()> {
    use std::io::Write;
    win::create_private(std::path::Path::new(path))?.write_all(text.as_bytes())
}

/// SIGTERM and SIGINT end the engine's loop, so that a temporary profile, which has
/// what pages stored, is removed as at any other end.
#[cfg(unix)]
fn quit_on_signals() {
    use std::sync::atomic::{AtomicBool, Ordering};
    static ASKED: AtomicBool = AtomicBool::new(false);
    extern "C" fn asked(_: i32) {
        ASKED.store(true, Ordering::SeqCst);
    }
    extern "C" {
        fn signal(number: i32, handler: extern "C" fn(i32)) -> usize;
    }
    unsafe {
        signal(15, asked);
        signal(2, asked);
    }
    // Once: the engine takes these over as it starts, and this is called again then.
    static WATCHING: AtomicBool = AtomicBool::new(false);
    if WATCHING.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(|| {
        while !ASKED.load(Ordering::SeqCst) {
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        if !ENGINE_RUNS.load(Ordering::SeqCst) {
            remove_temporary();
            std::process::exit(0);
        }
        exit_soon();
        zebkit::post(zebkit::quit);
    });
}

fn main() {
    // On Windows, built with +crt-static as the release is: this executable is the
    // engine's sandboxed children too (docs/WINDOWS.md). First of all.
    #[cfg(all(windows, target_feature = "crt-static"))]
    if let Some(code) = zebkit::win_startup() {
        std::process::exit(code);
    }
    // Before anything could print or start a process.
    let mut streams = {
        let args: Vec<String> = std::env::args().collect();
        let informational = args.iter().any(|arg| arg == "--help" || arg == "-h" || arg == "--version" || arg == "--list-tools");
        let http = args.iter().any(|arg| arg == "--http" || arg.starts_with("--http="));
        (!informational && (!http || args.iter().any(|arg| arg == "--stdio"))).then(stdio::take_streams)
    };

    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut bounds = Bounds::default();
    let mut options = parse(&args, &mut bounds);
    // The engine's data: the operator's profile, or here. Its own temporary
    // files come here in any case: a browser context keeps what pages stored in
    // a directory of the system's temporary one, which it would leave behind at
    // the end. TMPDIR is set while this is the process's only thread (setting
    // the environment with other threads running is unsound); every refusal
    // from here on removes the directory (fail).
    KEEPS_PROFILE.store(options.profile.is_some(), std::sync::atomic::Ordering::SeqCst);
    let temporary = make_temporary();
    std::env::set_var("TMPDIR", &temporary);
    #[cfg(unix)]
    quit_on_signals();

    let oauth_hosts = match &options.oauth_config {
        Some(path) => tools::auth::configure(path).unwrap_or_else(|why| fail(&why)),
        None => Vec::new(),
    };
    let rules = bounds::host_resolver_rules(&options.origins, &oauth_hosts).unwrap_or_else(|why| fail(&why));
    bounds.oauth_hosts = oauth_hosts;

    let http = options.http.take().map(|mut config| {
        let (value, generated) = token(options.token_file.as_deref());
        config.token = value;
        (config, generated)
    });

    let profile = options.profile.clone().unwrap_or_else(|| temporary.join("profile").display().to_string());

    let mut engine_args = vec!["zebkit-mcp".to_string(), format!("--user-data-dir={profile}"), format!("--host-resolver-rules={rules}")];
    // WebRTC's own sockets do not go through what the bounds watch.
    engine_args.push("--force-webrtc-ip-handling-policy=disable_non_proxied_udp".to_string());
    // As Playwright launches Chromium: a page restored whole from the back-forward
    // cache brings no requests for the hooks to see.
    engine_args.push("--disable-features=BackForwardCache".to_string());
    if !bounds.windows {
        engine_args.push("--headless".to_string());
    }
    engine_args.append(&mut options.engine_switches);

    let serve_stdio = options.stdio;
    let origins = std::mem::take(&mut options.origins);

    let code = zebkit::run_with_args(engine_args, move || {
        // The browser's URL parser is there now: read the origins as it reads them.
        let mut bounds = bounds;
        for origin in &origins {
            let rule = match bounds::parse_origin(origin) {
                Ok(rule) => rule,
                Err(why) => return refuse_in_engine(&why),
            };
            if let bounds::OriginRule::Exact(exact) = &rule {
                let host = zebkit::services::url_part(exact, None, zebkit::services::UrlPart::Host).unwrap_or_default();
                if bounds::plain_host(origin).unwrap_or_default() != host {
                    return refuse_in_engine(&format!("{origin}: write the name as the browser reads it, {host}"));
                }
            }
            bounds.origins.push(rule);
        }
        if BOUNDS.set(bounds).is_err() {
            fail("the bounds were set twice");
        }
        ENGINE_RUNS.store(true, std::sync::atomic::Ordering::SeqCst);
        #[cfg(unix)]
        quit_on_signals();
        if let Some((config, generated)) = http {
            let token = config.token.clone();
            let shown = std::net::SocketAddr::new(config.bind, 0).to_string();
            let shown = shown.strip_suffix(":0").unwrap_or(&shown).to_string();
            match http::listen(config) {
                Ok(port) => {
                    // One write: whoever reads the address has the token too.
                    let listening = format!("zebkit-mcp: listening on http://{shown}:{port}/mcp");
                    if generated {
                        eprintln!("{listening}\nzebkit-mcp: bearer token (shown once): {token}");
                    } else {
                        eprintln!("{listening}");
                    }
                }
                Err(why) => return refuse_in_engine(&why),
            }
        }
        if serve_stdio {
            let streams = streams.take().unwrap_or_else(|| fail("standard input and output were not kept"));
            std::thread::spawn(move || {
                stdio::serve(streams);
                // The end of standard input is the end of the server.
                exit_soon();
                zebkit::post(zebkit::quit);
            });
        }
    });
    remove_temporary();
    std::process::exit(if REFUSED.load(std::sync::atomic::Ordering::SeqCst) { 2 } else { code });
}
