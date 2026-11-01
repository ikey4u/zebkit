//! Finds the engine for the executable: beside it when shipped, and where the
//! build took it from during development.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    let macos = std::env::var("CARGO_CFG_TARGET_OS").is_ok_and(|system| system == "macos");
    if macos {
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path");
    } else {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    }
    let directory = PathBuf::from(std::env::var("DEP_ZEBKIT_ENGINE_DIR").expect("the zebkit crate says where the engine is"));
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
}
