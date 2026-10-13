//! Finds the engine, downloading it if need be, and tells the linker.
//!
//! In this order:
//!
//! 1. `ZEBKIT_DIR`: a directory holding the engine (`libzebkit.so.1` on
//!    Linux, `Zebkit.framework` on macOS) and what is shipped beside it.
//!    For an engine built by hand, and for building offline.
//! 2. `dist/release` of the zebkit repository, when this crate is being
//!    built from inside it (what `mise run r` produces).
//! 3. The release archive for this crate's version and the target, from
//!    GitHub. It is downloaded once into a cache directory, checked against
//!    the SHA-256 in `engines.txt`, and unpacked there.
//!
//! The download uses the system's `curl`, `sha256sum` (`shasum` on macOS),
//! `tar` and `xz`, so that this crate needs no others to build. On macOS
//! the system's compiler is used as well, for `src/cocoa/shim.m`.
//!
//! Environment:
//!
//! - `ZEBKIT_DIR`: see above.
//! - `ZEBKIT_CACHE_DIR`: where downloads are kept. Default
//!   `$XDG_CACHE_HOME/zebkit`, else `~/.cache/zebkit`.
//! - `ZEBKIT_DOWNLOAD_URL`: where the archives are, for a mirror. Default
//!   `https://github.com/ikey4u/zebkit/releases/download/v<version>`.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const RELEASES: &str = "https://github.com/ikey4u/zebkit/releases/download";

fn main() {
    for variable in ["ZEBKIT_DIR", "ZEBKIT_CACHE_DIR", "ZEBKIT_DOWNLOAD_URL"] {
        println!("cargo:rerun-if-env-changed={variable}");
    }
    println!("cargo:rerun-if-changed=engines.txt");
    // docs.rs builds the documentation and links nothing.
    if env::var_os("DOCS_RS").is_some() {
        return;
    }

    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let macos = env::var("CARGO_CFG_TARGET_OS").is_ok_and(|system| system == "macos");
    // What a package directory is known by.
    let engine_name = if macos { "Zebkit.framework" } else { "libzebkit.so.1" };
    let local = manifest.join("../../dist/release");
    let directory = if let Some(given) = env::var_os("ZEBKIT_DIR") {
        PathBuf::from(given)
    } else if local.join(engine_name).exists() {
        local
    } else {
        download(&manifest)
    };
    let directory = directory
        .canonicalize()
        .unwrap_or_else(|_| fail(&format!("there is no directory {}", directory.display())));
    let engine = directory.join(engine_name);
    if !engine.exists() {
        fail(&format!(
            "{} has no {engine_name}; it is not a zebkit package directory",
            directory.display()
        ));
    }

    if macos {
        println!("cargo:rerun-if-changed={}", engine.join("Versions/A/Zebkit").display());
        println!("cargo:rustc-link-search=framework={}", directory.display());
        println!("cargo:rustc-link-lib=framework=Zebkit");
        println!("cargo:rustc-link-lib=framework=Cocoa");
        // For this crate's own tests, which cargo does not help to find a
        // framework as it helps to find a library. Dependents say this
        // themselves (zebkit-build).
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
        println!(
            "cargo:rustc-link-arg=-Wl,-dyld_env,DYLD_FRAMEWORK_PATH={}/Zebkit.framework/Versions/A/Frameworks",
            directory.display()
        );
        compile_shim(&manifest, &directory);
    } else {
        println!("cargo:rerun-if-changed={}", engine.display());
        println!("cargo:rustc-link-search=native={}", directory.display());
        // The package has the libraries under their run-time names only, with
        // no "libzebkit.so" for the linker to find, hence the exact file names.
        for library in ["libzebkit.so.1", "libgobject-2.0.so.0", "libglib-2.0.so.0"] {
            println!("cargo:rustc-link-lib=dylib:+verbatim={library}");
        }
    }
    // For dependents' build scripts, as DEP_ZEBKIT_LIB_DIR.
    println!("cargo:lib_dir={}", directory.display());
}

/// macOS: the engine's API is Objective-C, which Rust cannot declare.
/// `src/cocoa/shim.m` puts what this crate uses behind C functions; it is
/// compiled here, against the engine's own headers, into a static library.
fn compile_shim(manifest: &Path, engine: &Path) {
    let source = manifest.join("src/cocoa/shim.m");
    println!("cargo:rerun-if-changed={}", source.display());
    let out = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let (object, library) = (out.join("shim.o"), out.join("libzebkit_shim.a"));
    run(
        Command::new("cc")
            .args(["-c", "-fobjc-arc", "-O2", "-Wall", "-Wno-deprecated-declarations", "-F"])
            .arg(engine)
            .arg(&source)
            .arg("-o")
            .arg(&object),
        "cannot compile src/cocoa/shim.m; this needs Xcode's command line tools",
    );
    let _ = fs::remove_file(&library);
    run(
        Command::new("ar").arg("rcs").arg(&library).arg(&object),
        "cannot make a library of the compiled shim",
    );
    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=zebkit_shim");
}

/// The engine for this crate's version and the target, from the cache or
/// from the release page. Returns the directory it is unpacked in.
fn download(manifest: &Path) -> PathBuf {
    let version = env::var("CARGO_PKG_VERSION").unwrap();
    let target = env::var("TARGET").unwrap();
    let engines = fs::read_to_string(manifest.join("engines.txt")).unwrap_or_default();
    let checksum = engines
        .lines()
        .filter(|line| !line.starts_with('#'))
        .map(|line| line.split_whitespace().collect::<Vec<_>>())
        .find(|fields| fields.len() == 3 && fields[0] == version && fields[1] == target)
        .map(|fields| fields[2].to_string())
        .unwrap_or_else(|| {
            let known: Vec<&str> = engines
                .lines()
                .filter(|line| !line.starts_with('#'))
                .filter_map(|line| line.split_whitespace().nth(1))
                .collect();
            fail(&format!(
                "there is no prebuilt zebkit {version} for {target} (there is one for: {}).\n\
                 Build the engine from https://github.com/ikey4u/zebkit and set ZEBKIT_DIR to its package directory",
                if known.is_empty() { "none".to_string() } else { known.join(", ") }
            ))
        });

    let name = format!("zebkit-{version}-{target}");
    let cache = cache_directory();
    let directory = cache.join(&name);
    // The marker is written last, so a directory that has it is complete.
    let marker = directory.join(".zebkit-complete");
    if fs::read_to_string(&marker).is_ok_and(|recorded| recorded.trim() == checksum) {
        return directory;
    }

    fs::create_dir_all(&cache).unwrap_or_else(|error| fail(&format!("cannot create {}: {error}", cache.display())));
    let base = env::var("ZEBKIT_DOWNLOAD_URL").unwrap_or_else(|_| format!("{RELEASES}/v{version}"));
    let url = format!("{}/{name}.tar.xz", base.trim_end_matches('/'));
    // Other builds may be doing the same at this moment: each works in a
    // place of its own and moves the result in at the end.
    let scratch = cache.join(format!(".{name}.{}", std::process::id()));
    let _ = fs::remove_dir_all(&scratch);
    fs::create_dir_all(&scratch).unwrap_or_else(|error| fail(&format!("cannot create {}: {error}", scratch.display())));
    let archive = scratch.join("archive.tar.xz");

    println!("cargo:warning=downloading zebkit {version} for {target} (about 25 MiB, once) from {url}");
    run(
        Command::new("curl").args(["--fail", "--location", "--silent", "--show-error", "--retry", "3", "--output"]).arg(&archive).arg(&url),
        &format!("cannot download {url}.\nIf this machine has no access to it, download the file elsewhere, unpack it and set ZEBKIT_DIR to the directory"),
    );
    // macOS has shasum where Linux has sha256sum; both print the sum first.
    let mut sum = if cfg!(target_os = "macos") {
        Command::new("shasum")
    } else {
        Command::new("sha256sum")
    };
    if cfg!(target_os = "macos") {
        sum.args(["-a", "256"]);
    }
    let output = sum
        .arg(&archive)
        .output()
        .unwrap_or_else(|error| fail(&format!("cannot compute the archive's SHA-256: {error}")));
    let actual = String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .unwrap_or_default()
        .to_string();
    if actual != checksum {
        let _ = fs::remove_dir_all(&scratch);
        fail(&format!(
            "{url} is not the file this crate was published with:\n  expected sha256 {checksum}\n  got             {actual}"
        ));
    }
    run(
        Command::new("tar").arg("-xJf").arg(&archive).arg("-C").arg(&scratch),
        "cannot unpack the engine; this needs tar and xz",
    );
    let unpacked = scratch.join(&name);
    fs::write(unpacked.join(".zebkit-complete"), &checksum)
        .unwrap_or_else(|error| fail(&format!("the archive has no {name}/ directory: {error}")));
    let _ = fs::remove_dir_all(&directory);
    // Losing this race to another build is fine: what is there is the same.
    if fs::rename(&unpacked, &directory).is_err() && !marker.exists() {
        fail(&format!("cannot move the engine into {}", directory.display()));
    }
    let _ = fs::remove_dir_all(&scratch);
    directory
}

fn cache_directory() -> PathBuf {
    if let Some(given) = env::var_os("ZEBKIT_CACHE_DIR") {
        return PathBuf::from(given);
    }
    if let Some(cache) = env::var_os("XDG_CACHE_HOME").filter(|value| !value.is_empty()) {
        return PathBuf::from(cache).join("zebkit");
    }
    match env::var_os("HOME") {
        Some(home) => PathBuf::from(home).join(".cache").join("zebkit"),
        None => fail("no place to keep the downloaded engine: set ZEBKIT_CACHE_DIR"),
    }
}

fn run(command: &mut Command, problem: &str) {
    match command.status() {
        Ok(status) if status.success() => {}
        Ok(_) => fail(problem),
        Err(error) => fail(&format!("{problem}\n({:?} could not be run: {error})", command.get_program())),
    }
}

fn fail(message: &str) -> ! {
    eprintln!("\nzebkit-rs: {message}\n");
    std::process::exit(1);
}
