//! Passes on where the engine is, and lets this crate's own examples and
//! tests find it when they run from the build tree.
//!
//! An application's executable has to find the engine too. A build script
//! cannot arrange that for the crates that depend on it; the application's
//! own build script does, with one call to `zebkit_build::bundle()`.

fn main() {
    if let Ok(directory) = std::env::var("DEP_ZEBKIT_LIB_DIR") {
        // For the application's build script, as DEP_ZEBKIT_ENGINE_DIR.
        println!("cargo:dir={directory}");
        println!("cargo:rustc-link-arg-examples=-Wl,-rpath,{directory}");
        println!("cargo:rustc-link-arg-tests=-Wl,-rpath,{directory}");
    }
}
