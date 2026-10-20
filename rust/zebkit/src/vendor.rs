//! Libraries that are inside the engine, for the application to use too.
//!
//! The engine has whole libraries linked into it. An application that
//! needs one of them does not have to bring a second copy: it is reachable
//! through the engine, under link names of zebkit's own, so that another
//! copy elsewhere in the process cannot collide with it. `docs/VENDOR.md`
//! in the repository has the details.

/// The FFmpeg inside the engine.
///
/// It is Chromium's, with what playing media in a web page needs from it:
/// demuxers for MP4, WebM, Ogg, MP3, AAC, FLAC and WAV, and decoders for H.264,
/// AAC, MP3, Vorbis, FLAC, PCM and Opus (as `libopus`). It has no encoders.
/// VP8, VP9 and AV1 are decoded by libvpx and dav1d, which are inside the
/// engine too, with C headers of their own (`<zebkit/vendor/libvpx.h>`,
/// `<zebkit/vendor/dav1d.h>`).
///
/// What is here in safe Rust is a look at what the library has. Decoding
/// itself is FFmpeg's C API, which is [`crate::sys::ffmpeg`]; declarations
/// are added there as they are needed.
pub mod ffmpeg {
    use crate::string_from;
    use crate::sys::ffmpeg as sys;

    /// What a codec works on.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum MediaType {
        Video,
        Audio,
        Other,
    }

    /// A codec the library was built with.
    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct Codec {
        /// The short name, as in `"h264"`.
        pub name: String,
        /// The descriptive name, as in `"H.264 / AVC / MPEG-4 AVC"`.
        pub long_name: String,
        pub media_type: MediaType,
    }

    /// FFmpeg's version, as in `"8.1.3"`.
    pub fn version() -> String {
        unsafe { string_from(sys::av_version_info()) }.unwrap_or_default()
    }

    /// libavcodec's version as (major, minor, micro).
    pub fn avcodec_version() -> (u32, u32, u32) {
        let version = unsafe { sys::avcodec_version() };
        (version >> 16, (version >> 8) & 0xff, version & 0xff)
    }

    fn codecs(wanted: unsafe extern "C" fn(*const sys::AVCodec) -> std::os::raw::c_int) -> Vec<Codec> {
        let mut found = Vec::new();
        let mut position = std::ptr::null_mut();
        unsafe {
            loop {
                let codec = sys::av_codec_iterate(&mut position);
                if codec.is_null() {
                    break;
                }
                if wanted(codec) == 0 {
                    continue;
                }
                found.push(Codec {
                    name: string_from((*codec).name).unwrap_or_default(),
                    long_name: string_from((*codec).long_name).unwrap_or_default(),
                    media_type: match (*codec).media_type {
                        sys::AVMEDIA_TYPE_VIDEO => MediaType::Video,
                        sys::AVMEDIA_TYPE_AUDIO => MediaType::Audio,
                        _ => MediaType::Other,
                    },
                });
            }
        }
        found
    }

    /// Every decoder the library has.
    pub fn decoders() -> Vec<Codec> {
        codecs(sys::av_codec_is_decoder)
    }

    /// Every encoder the library has.
    pub fn encoders() -> Vec<Codec> {
        codecs(sys::av_codec_is_encoder)
    }

    /// Whether there is a decoder of this short name, as in `"vp9"`.
    pub fn has_decoder(name: &str) -> bool {
        !unsafe { sys::avcodec_find_decoder_by_name(crate::c_string(name).as_ptr()) }.is_null()
    }
}
