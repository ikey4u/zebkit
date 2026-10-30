//! cargo run -p zebkit --example media_tools -- probe INPUT
//! cargo run -p zebkit --example media_tools -- ffmpeg -i INPUT -c copy OUTPUT
//! This example needs no browser initialization or event loop.
use zebkit::media_tools::{self, Completion};

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let borrowed: Vec<&str> = args.iter().skip(1).map(String::as_str).collect();
    let result = match args.first().map(String::as_str) {
        Some("probe") => media_tools::ffprobe(&borrowed).map(|json| println!("{json}")),
        Some("ffmpeg") => media_tools::ffmpeg(&borrowed).map(|completion| {
            if completion == Completion::Cancelled { std::process::exit(130); }
        }),
        _ => {
            eprintln!("usage: media_tools probe INPUT | ffmpeg ARGUMENTS...");
            std::process::exit(2);
        }
    };
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}
