//! What the FFmpeg inside the engine can decode.
//!
//!     cargo run --example codecs

use zebkit::vendor::ffmpeg;

fn main() {
    println!("FFmpeg {} inside zebkit", ffmpeg::version());
    for codec in ffmpeg::decoders() {
        println!("  {:<12} {:?}: {}", codec.name, codec.media_type, codec.long_name);
    }
    println!("{} encoders", ffmpeg::encoders().len());
}
