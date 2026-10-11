//! Finds a built zebkit and tells the linker about it.
//!
//! The engine is looked for in `ZEBKIT_DIR`, else in `dist/release` of the
//! repository this crate is in (what `mise run r` produces). It is a
//! directory holding `libzebkit.so.1` and the libraries shipped beside it.

use std::env;
use std::path::PathBuf;

fn main() {
    println!("cargo:rerun-if-env-changed=ZEBKIT_DIR");
    let directory = env::var_os("ZEBKIT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap()).join("../../dist/release"));
    let directory = directory.canonicalize().unwrap_or_else(|_| {
        panic!(
            "no zebkit at {}: build one with `mise run r`, or set ZEBKIT_DIR to a package directory",
            directory.display()
        )
    });
    let engine = directory.join("libzebkit.so.1");
    assert!(
        engine.exists(),
        "{} has no libzebkit.so.1; set ZEBKIT_DIR to a zebkit package directory",
        directory.display()
    );
    println!("cargo:rerun-if-changed={}", engine.display());

    println!("cargo:rustc-link-search=native={}", directory.display());
    // The package has the libraries under their run-time names only, with
    // no "libzebkit.so" for the linker to find, hence the exact file names.
    for library in ["libzebkit.so.1", "libgobject-2.0.so.0", "libglib-2.0.so.0"] {
        println!("cargo:rustc-link-lib=dylib:+verbatim={library}");
    }
    // For dependents' build scripts, as DEP_ZEBKIT_LIB_DIR.
    println!("cargo:lib_dir={}", directory.display());
}
