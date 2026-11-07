//! Finds the engine for the executable: beside it when shipped, and where the
//! build took it from during development. ZEBKIT_MCP_SHIPPED=1 (scripts/release.sh)
//! leaves the build's directory out: only beside it.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=ZEBKIT_MCP_SHIPPED");
    let system = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    if system == "windows" {
        // A DLL is found beside the executable; no run paths. A renderer's main
        // thread runs V8 and Blink on this one's stack size, as chrome.exe's.
        println!("cargo:rustc-link-arg=/STACK:0x800000");
        return;
    }
    if system == "macos" {
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path");
    } else {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    }
    if std::env::var("ZEBKIT_MCP_SHIPPED").is_ok_and(|value| value == "1") {
        return;
    }
    let directory = PathBuf::from(std::env::var("DEP_ZEBKIT_ENGINE_DIR").expect("the zebkit crate says where the engine is"));
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
}
