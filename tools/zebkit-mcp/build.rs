//! Finds the engine for the executable: beside it when shipped, and where the
//! build took it from during development. Builds the one C file, media_cut.c,
//! which uses the FFmpeg inside the engine through <zebkit/vendor/ffmpeg.h>.

use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=src/media_cut.c");
    let macos = std::env::var("CARGO_CFG_TARGET_OS").is_ok_and(|system| system == "macos");
    if macos {
        println!("cargo:rustc-link-arg=-Wl,-rpath,@executable_path");
    } else {
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN");
    }
    let directory = PathBuf::from(std::env::var("DEP_ZEBKIT_ENGINE_DIR").expect("the zebkit crate says where the engine is"));
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", directory.display());
    let include = directory.join("include");
    cc::Build::new().file("src/media_cut.c").include(include).warnings(true).compile("media_cut");
}
