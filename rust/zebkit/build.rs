//! Lets this crate's own examples and tests find the engine when they run
//! from the build tree. An application decides this for itself: it ships
//! the package beside its executable and links with `-Wl,-rpath,$ORIGIN`.

fn main() {
    if let Ok(directory) = std::env::var("DEP_ZEBKIT_LIB_DIR") {
        println!("cargo:rustc-link-arg-examples=-Wl,-rpath,{directory}");
        println!("cargo:rustc-link-arg-tests=-Wl,-rpath,{directory}");
        println!("cargo:rustc-env=ZEBKIT_LIB_DIR={directory}");
    }
}
