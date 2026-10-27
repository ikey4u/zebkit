//! For the build script of an application that uses `zebkit`.
//!
//! The engine is not inside the executable. It is a shared library with
//! a helper program, its data files and a few libraries of its own (on
//! macOS, all inside Zebkit.framework), and the executable has to find
//! them when it runs. One call arranges that:
//!
//! ```toml
//! [dependencies]
//! zebkit = "0.0.1-alpha.11"
//!
//! [build-dependencies]
//! zebkit-build = "0.0.1-alpha.11"
//! ```
//!
//! ```no_run
//! // build.rs
//! fn main() {
//!     zebkit_build::bundle();
//! }
//! ```
//!
//! After that `cargo run` works, and so does the executable wherever it is
//! copied, as long as the `zebkit` directory beside it is copied with it:
//!
//! ```text
//! target/release/
//!     my-app
//!     zebkit/              <- the engine: ship this directory with my-app
//!         libzebkit.so.1
//!         zebkit-helper
//!         ...
//! ```
//!
//! On macOS the directory holds `Zebkit.framework`, with the engine inside it.

#![allow(clippy::needless_doctest_main)]

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

/// The name of the directory, beside the executable, that holds the engine.
pub const DIRECTORY: &str = "zebkit";

/// Puts the engine in a `zebkit` directory beside the executables this
/// package builds, and makes them look for it there.
///
/// Call from `build.rs`. Panics, failing the build with the reason, if the
/// engine cannot be put there.
pub fn bundle() {
    let engine = engine_directory();
    let destination = executable_directory().join(DIRECTORY);
    copy_engine(&engine, &destination);
    // Beside the executable first, wherever that ends up; tests and
    // examples are built one directory further down.
    if env::var("CARGO_CFG_TARGET_OS").is_ok_and(|system| system == "macos") {
        // Zebkit.framework, by a run path.
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path/{DIRECTORY}");
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path/../{DIRECTORY}");
        println!(
            "cargo:rerun-if-changed={}",
            engine.join("Zebkit.framework/Versions/A/Zebkit").display()
        );
    } else {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/{DIRECTORY}");
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../{DIRECTORY}");
        println!("cargo:rerun-if-changed={}", engine.join("libzebkit.so.1").display());
    }
    println!("cargo:rerun-if-changed=build.rs");
}

/// Where the engine the `zebkit` crate was built against is.
pub fn engine_directory() -> PathBuf {
    println!("cargo:rerun-if-env-changed=DEP_ZEBKIT_ENGINE_DIR");
    match env::var_os("DEP_ZEBKIT_ENGINE_DIR") {
        Some(directory) => PathBuf::from(directory),
        None => panic!("zebkit-build: the package being built does not depend on the zebkit crate directly (add zebkit to [dependencies])"),
    }
}

/// The directory executables are written to: `target/<profile>`. Cargo
/// does not say where that is; it is three levels above `OUT_DIR`
/// (`target/<profile>/build/<package>-<hash>/out`).
fn executable_directory() -> PathBuf {
    let out = PathBuf::from(env::var_os("OUT_DIR").expect("zebkit-build is for build scripts"));
    out.ancestors()
        .nth(3)
        .map(Path::to_path_buf)
        .unwrap_or_else(|| panic!("zebkit-build: unexpected OUT_DIR {}", out.display()))
}

/// Makes `destination` hold the files of `engine`, without `include/`.
/// Files are hard links where the file system allows, copies otherwise, and
/// are left alone when they are already the same. Symbolic links, which a
/// framework on macOS is full of, stay links.
fn copy_engine(engine: &Path, destination: &Path) {
    let entries = fs::read_dir(engine).unwrap_or_else(|error| panic!("zebkit-build: cannot read {}: {error}", engine.display()));
    fs::create_dir_all(destination).unwrap_or_else(|error| panic!("zebkit-build: cannot create {}: {error}", destination.display()));
    for entry in entries.flatten() {
        let name = entry.file_name();
        let (from, to) = (entry.path(), destination.join(&name));
        // include/ is for compiling against the engine; a marker file of
        // the download is nobody's business. What is inside a framework
        // all stays, its headers too: it is signed as a whole.
        if name == "include" || name.to_string_lossy().starts_with('.') {
            continue;
        }
        if let Ok(target) = fs::read_link(&from) {
            if fs::read_link(&to).ok().as_ref() != Some(&target) {
                let _ = fs::remove_file(&to);
                #[cfg(unix)]
                std::os::unix::fs::symlink(&target, &to)
                    .unwrap_or_else(|error| panic!("zebkit-build: cannot link {}: {error}", to.display()));
            }
            continue;
        }
        if from.is_dir() {
            copy_engine(&from, &to);
            continue;
        }
        if same_file(&from, &to) {
            continue;
        }
        let _ = fs::remove_file(&to);
        if fs::hard_link(&from, &to).is_err() {
            fs::copy(&from, &to)
                .unwrap_or_else(|error| panic!("zebkit-build: cannot copy {} to {}: {error}", from.display(), to.display()));
            // So that the next build sees it is the same and leaves it.
            if let (Ok(file), Ok(time)) = (
                fs::File::options().write(true).open(&to),
                fs::metadata(&from).and_then(|data| data.modified()),
            ) {
                let _ = file.set_modified(time);
            }
        }
    }
}

fn same_file(a: &Path, b: &Path) -> bool {
    match (fs::metadata(a), fs::metadata(b)) {
        (Ok(a), Ok(b)) => a.len() == b.len() && a.modified().ok() == b.modified().ok(),
        _ => false,
    }
}
