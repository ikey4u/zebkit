use zebkit::media_tools::{self, Completion};

#[test]
fn native_tools_without_engine_loop() {
    let source = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../tests/engine/pages/media/clip.mp4");
    let source = source.to_str().unwrap();
    assert!(media_tools::probe(source).unwrap().contains("\"codec_name\":\"h264\""));
    assert!(media_tools::probe("bad\0path").is_err());
    let directory = std::env::temp_dir().join(format!("zebkit-rust-media-tools-{}", std::process::id()));
    std::fs::create_dir(&directory).unwrap();
    let output = directory.join("copy.mkv");
    std::fs::write(&output, b"original").unwrap();
    let arguments = ["-y", "-i", source, "-c", "copy", output.to_str().unwrap()];
    let result = media_tools::ffmpeg_with_progress(&arguments, |n, _| n < 5).unwrap();
    assert_eq!(result, Completion::Cancelled);
    assert_eq!(std::fs::read(&output).unwrap(), b"original");
    let panic = std::panic::catch_unwind(|| {
        media_tools::ffmpeg_with_progress(&arguments, |n, _| {
            assert!(n < 5, "intentional callback panic");
            true
        }).unwrap();
    });
    assert!(panic.is_err());
    assert_eq!(std::fs::read(&output).unwrap(), b"original");
    assert_eq!(std::fs::read_dir(&directory).unwrap().count(), 1);
    assert_eq!(media_tools::remux(source, output.to_str().unwrap(), true).unwrap(), Completion::Completed);
    assert!(media_tools::probe(output.to_str().unwrap()).unwrap().contains("matroska"));
    std::fs::remove_file(output).unwrap();
    std::fs::remove_dir(directory).unwrap();
}
