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
        // For everything of this crate that is linked: its examples, its
        // tests and the unit tests in the library. Not for its dependents.
        println!("cargo:rustc-link-arg=-Wl,-rpath,{directory}");
    }
}
