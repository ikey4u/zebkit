//! What the engine has for applications besides pages: Chromium's
//! compression, cryptography, URL parser, image codecs and network stack,
//! as `<zebkit/services.h>` gives them.
//!
//! Everything but [`fetch`] works without [`crate::run`]; [`fetch`] is the
//! engine's network stack and runs on its thread.

use crate::engine::require_engine_thread;
use crate::{c_string, string_from, sys, take_bytes, take_error, take_string, Bytes, Error};
use std::os::raw::{c_int, c_void};

/// A compressed format.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Compression {
    /// RFC 1952, as `Content-Encoding: gzip`.
    Gzip,
    /// RFC 1950, as `Content-Encoding: deflate`.
    Zlib,
    /// RFC 1951, with no header.
    DeflateRaw,
    /// RFC 7932.
    Brotli,
    /// RFC 8878.
    Zstd,
}

impl Compression {
    fn raw(self) -> sys::zebkit_compression {
        match self {
            Compression::Gzip => sys::ZEBKIT_COMPRESS_GZIP,
            Compression::Zlib => sys::ZEBKIT_COMPRESS_ZLIB,
            Compression::DeflateRaw => sys::ZEBKIT_COMPRESS_DEFLATE_RAW,
            Compression::Brotli => sys::ZEBKIT_COMPRESS_BROTLI,
            Compression::Zstd => sys::ZEBKIT_COMPRESS_ZSTD,
        }
    }
}

/// Compresses `data`. `level` `None` is the format's default; otherwise
/// 0-9 for zlib's formats, 0-11 for Brotli, 1-22 for Zstandard.
pub fn compress(format: Compression, data: &[u8], level: Option<i32>) -> Result<Bytes, Error> {
    let (mut size, mut error) = (0, std::ptr::null_mut());
    unsafe {
        let out =
            sys::zebkit_compress(format.raw(), data.as_ptr().cast(), data.len(), level.unwrap_or(-1), &mut size, &mut error);
        if out.is_null() {
            return Err(take_error(error, "compression failed"));
        }
        Ok(take_bytes(out, size))
    }
}

/// Decompresses `data`, failing rather than produce more than `max_size`
/// bytes (`None`: 256 MiB), against data that expands without end.
pub fn decompress(format: Compression, data: &[u8], max_size: Option<usize>) -> Result<Bytes, Error> {
    let (mut size, mut error) = (0, std::ptr::null_mut());
    unsafe {
        let out = sys::zebkit_decompress(
            format.raw(),
            data.as_ptr().cast(),
            data.len(),
            max_size.unwrap_or(0),
            &mut size,
            &mut error,
        );
        if out.is_null() {
            return Err(take_error(error, "decompression failed"));
        }
        Ok(take_bytes(out, size))
    }
}

/// A digest, of BoringSSL's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Digest {
    Sha1,
    Sha256,
    Sha384,
    Sha512,
}

impl Digest {
    fn raw(self) -> sys::zebkit_digest {
        match self {
            Digest::Sha1 => sys::ZEBKIT_DIGEST_SHA1,
            Digest::Sha256 => sys::ZEBKIT_DIGEST_SHA256,
            Digest::Sha384 => sys::ZEBKIT_DIGEST_SHA384,
            Digest::Sha512 => sys::ZEBKIT_DIGEST_SHA512,
        }
    }

    /// The size of the digest in bytes: 20, 32, 48 or 64.
    pub fn size(self) -> usize {
        unsafe { sys::zebkit_digest_size(self.raw()) }
    }
}

/// The digest of `data`.
pub fn digest(digest: Digest, data: &[u8]) -> Vec<u8> {
    let mut out = vec![0; digest.size()];
    let done = unsafe { sys::zebkit_digest_compute(digest.raw(), data.as_ptr().cast(), data.len(), out.as_mut_ptr()) };
    assert!(done, "zebkit: the digest failed");
    out
}

/// HMAC of `data` with `key`.
pub fn hmac(digest: Digest, key: &[u8], data: &[u8]) -> Vec<u8> {
    let mut out = vec![0; digest.size()];
    let done = unsafe {
        sys::zebkit_hmac(digest.raw(), key.as_ptr().cast(), key.len(), data.as_ptr().cast(), data.len(), out.as_mut_ptr())
    };
    assert!(done, "zebkit: HMAC failed");
    out
}

/// `size` bytes from the system's secure random source.
pub fn random_bytes(size: usize) -> Vec<u8> {
    let mut out = vec![0; size];
    let done = unsafe { sys::zebkit_random_bytes(out.as_mut_ptr().cast(), size) };
    assert!(done, "zebkit: no random bytes");
    out
}

/// AES-GCM with a 16-byte tag: `key` of 16 or 32 bytes (AES-128 or
/// AES-256), a 12-byte `nonce` never used twice with the same key, and
/// `ad` authenticated but not encrypted. Returns the ciphertext followed by
/// the tag.
pub fn aes_gcm_seal(key: &[u8], nonce: &[u8], ad: &[u8], plaintext: &[u8]) -> Result<Bytes, Error> {
    let (mut size, mut error) = (0, std::ptr::null_mut());
    unsafe {
        let out = sys::zebkit_aes_gcm_seal(
            key.as_ptr().cast(),
            key.len(),
            nonce.as_ptr().cast(),
            nonce.len(),
            ad.as_ptr().cast(),
            ad.len(),
            plaintext.as_ptr().cast(),
            plaintext.len(),
            &mut size,
            &mut error,
        );
        if out.is_null() {
            return Err(take_error(error, "sealing failed"));
        }
        Ok(take_bytes(out, size))
    }
}

/// Opens what [`aes_gcm_seal`] sealed; fails when anything was changed.
pub fn aes_gcm_open(key: &[u8], nonce: &[u8], ad: &[u8], sealed: &[u8]) -> Result<Bytes, Error> {
    let (mut size, mut error) = (0, std::ptr::null_mut());
    unsafe {
        let out = sys::zebkit_aes_gcm_open(
            key.as_ptr().cast(),
            key.len(),
            nonce.as_ptr().cast(),
            nonce.len(),
            ad.as_ptr().cast(),
            ad.len(),
            sealed.as_ptr().cast(),
            sealed.len(),
            &mut size,
            &mut error,
        );
        if out.is_null() {
            return Err(take_error(error, "opening failed"));
        }
        Ok(take_bytes(out, size))
    }
}

/// A part of a URL.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UrlPart {
    /// The whole URL, canonical.
    Spec,
    Scheme,
    Username,
    Password,
    /// As in `"example.com"` or `"[::1]"`; punycode for IDN.
    Host,
    /// `""` when it is the scheme's default.
    Port,
    Path,
    /// Without the `?`.
    Query,
    /// Without the `#`.
    Fragment,
    /// As in `"https://example.com"`, or `"null"`.
    Origin,
}

impl UrlPart {
    fn raw(self) -> sys::zebkit_url_part {
        match self {
            UrlPart::Spec => sys::ZEBKIT_URL_SPEC,
            UrlPart::Scheme => sys::ZEBKIT_URL_SCHEME,
            UrlPart::Username => sys::ZEBKIT_URL_USERNAME,
            UrlPart::Password => sys::ZEBKIT_URL_PASSWORD,
            UrlPart::Host => sys::ZEBKIT_URL_HOST,
            UrlPart::Port => sys::ZEBKIT_URL_PORT,
            UrlPart::Path => sys::ZEBKIT_URL_PATH,
            UrlPart::Query => sys::ZEBKIT_URL_QUERY,
            UrlPart::Fragment => sys::ZEBKIT_URL_FRAGMENT,
            UrlPart::Origin => sys::ZEBKIT_URL_ORIGIN,
        }
    }
}

/// A part of `url`, resolved against `base`, as a browser parses and
/// canonicalizes it. `None` when it is not a valid URL.
pub fn url_part(url: &str, base: Option<&str>, part: UrlPart) -> Option<String> {
    let base = base.map(c_string);
    unsafe {
        take_string(sys::zebkit_url_get(
            c_string(url).as_ptr(),
            base.as_ref().map_or(std::ptr::null(), |base| base.as_ptr()),
            part.raw(),
        ))
    }
}

/// The registrable domain of a host or URL, by the public suffix list:
/// `"example.co.uk"` for `"a.b.example.co.uk"`. `""` for a host that has
/// none (an IP address, `localhost`, a public suffix itself); `None` when
/// it is not valid.
pub fn registrable_domain(host_or_url: &str) -> Option<String> {
    unsafe { take_string(sys::zebkit_url_registrable_domain(c_string(host_or_url).as_ptr())) }
}

/// An image as 8-bit RGBA, not premultiplied, rows top to bottom.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Image {
    pub width: u32,
    pub height: u32,
    /// `width * height * 4` bytes.
    pub rgba: Bytes,
}

/// A format to encode an image in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImageFormat {
    Png,
    Jpeg,
    WebP,
}

/// Decodes an image file: PNG, JPEG, WebP, and what else the engine's
/// Skia reads.
pub fn decode_image(data: &[u8]) -> Result<Image, Error> {
    let (mut width, mut height, mut error) = (0, 0, std::ptr::null_mut());
    unsafe {
        let pixels = sys::zebkit_image_decode(data.as_ptr().cast(), data.len(), &mut width, &mut height, &mut error);
        if pixels.is_null() {
            return Err(take_error(error, "the image could not be decoded"));
        }
        let (width, height) = (width.max(0) as u32, height.max(0) as u32);
        Ok(Image { width, height, rgba: take_bytes(pixels, width as usize * height as usize * 4) })
    }
}

/// Encodes an image. `quality` 0-100 is for JPEG and lossy WebP; 101 asks
/// WebP for lossless. PNG does not use it.
pub fn encode_image(format: ImageFormat, image: &Image, quality: u8) -> Result<Bytes, Error> {
    if image.rgba.len() != image.width as usize * image.height as usize * 4 {
        return Err(Error::new("the pixels are not width * height * 4 bytes"));
    }
    let format = match format {
        ImageFormat::Png => sys::ZEBKIT_IMAGE_PNG,
        ImageFormat::Jpeg => sys::ZEBKIT_IMAGE_JPEG,
        ImageFormat::WebP => sys::ZEBKIT_IMAGE_WEBP,
    };
    let (mut size, mut error) = (0, std::ptr::null_mut());
    unsafe {
        let out = sys::zebkit_image_encode(
            format,
            image.rgba.as_ptr(),
            c_int::try_from(image.width).unwrap_or(c_int::MAX),
            c_int::try_from(image.height).unwrap_or(c_int::MAX),
            c_int::from(quality),
            &mut size,
            &mut error,
        );
        if out.is_null() {
            return Err(take_error(error, "the image could not be encoded"));
        }
        Ok(take_bytes(out, size))
    }
}

/// Loads ICU's data, so that the ICU inside the engine can be used without
/// [`crate::run`]. Calling it again, or with the engine running, does no
/// harm.
pub fn icu_init() -> bool {
    unsafe { sys::zebkit_icu_init() }
}

/// An answer to [`fetch`]; its body is the engine's for as long as the
/// function it is given to runs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Response<'a> {
    /// The HTTP status, or 0 when there was no response.
    pub status: u16,
    /// After redirects.
    pub url: String,
    /// Each header as it came, name and value.
    pub headers: Vec<(String, String)>,
    pub body: &'a [u8],
    /// Why there was no response.
    pub error: Option<String>,
}

/// Sends a request through the engine's network stack: HTTP/1.1, HTTP/2
/// and HTTP/3, its cache, its cookies and its proxy settings, as the
/// browser makes a request of its own (no CORS, no page). Redirects are
/// followed; a body over 256 MiB fails. `done` is called on the engine's
/// thread, which this is for.
pub fn fetch<F: FnOnce(Response<'_>) + 'static>(
    method: &str,
    url: &str,
    headers: &[(&str, &str)],
    body: &[u8],
    done: F,
) {
    require_engine_thread("services::fetch()");
    fetch_in(std::ptr::null_mut(), method, url, headers, body, done);
}

pub(crate) fn fetch_in<F: FnOnce(Response<'_>) + 'static>(
    context: *mut sys::zebkit_context,
    method: &str,
    url: &str,
    headers: &[(&str, &str)],
    body: &[u8],
    done: F,
) {
    let headers: String = headers.iter().map(|(name, value)| format!("{name}: {value}\r\n")).collect();
    let data = Box::into_raw(Box::new(done));
    let (method, url, headers) = (c_string(method), c_string(url), c_string(&headers));
    unsafe {
        if context.is_null() {
            sys::zebkit_fetch(method.as_ptr(), url.as_ptr(), headers.as_ptr(), body.as_ptr().cast(), body.len(), on_response::<F>, data.cast())
        } else {
            sys::zebkit_fetch_in(context, method.as_ptr(), url.as_ptr(), headers.as_ptr(), body.as_ptr().cast(), body.len(), on_response::<F>, data.cast())
        }
    }
}

// ---- connection reuse (docs/CONNECTION_REUSE.md) ----

/// Empties the engine profile's HTTP cache (on disk under the profile unless
/// the startup setting `httpCache` says otherwise) and forgets what the
/// network learnt of servers (HTTP/2, alternative services, HSTS), so that
/// nothing of the sites visited stays there; cookies and storage are not
/// touched. `done` runs on the engine's thread once that is done. An error,
/// and `done` dropped, while the engine stops. [`crate::Context::clear_http_cache`]
/// for a context's.
pub fn clear_http_cache<F: FnOnce() + 'static>(done: F) -> Result<(), Error> {
    require_engine_thread("services::clear_http_cache()");
    clear_http_cache_in(std::ptr::null_mut(), done)
}

pub(crate) fn clear_http_cache_in<F: FnOnce() + 'static>(context: *mut sys::zebkit_context, done: F) -> Result<(), Error> {
    let data = Box::into_raw(Box::new(done));
    if unsafe { sys::zebkit_clear_http_cache(context, Some(on_cleared::<F>), data.cast()) } {
        Ok(())
    } else {
        drop(unsafe { Box::from_raw(data) });
        Err(Error::new("the engine refused: it is stopping"))
    }
}

unsafe extern "C" fn on_cleared<F: FnOnce()>(data: *mut c_void) {
    let done = Box::from_raw(data.cast::<F>());
    crate::guard(|| done());
}

/// Opens `count` (1 to 6) connections to `url`'s origin in the engine
/// profile's network, TLS included, for a page load or [`fetch`] soon after;
/// no request is sent, and one no request uses is closed after about 60 s.
/// An error for a URL that is not http(s), a count out of range, or while
/// the engine stops. [`crate::Context::preconnect`] for a context's; the
/// startup setting `preconnect` does this as the engine starts.
pub fn preconnect(url: &str, count: u32) -> Result<(), Error> {
    require_engine_thread("services::preconnect()");
    preconnect_in(std::ptr::null_mut(), url, count)
}

pub(crate) fn preconnect_in(context: *mut sys::zebkit_context, url: &str, count: u32) -> Result<(), Error> {
    let count = c_int::try_from(count).unwrap_or(c_int::MAX);
    if unsafe { sys::zebkit_preconnect(context, c_string(url).as_ptr(), count) } {
        Ok(())
    } else {
        Err(Error::new("not preconnected: an http(s) URL and a count from 1 to 6, with the engine running"))
    }
}

unsafe extern "C" fn on_response<F: FnOnce(Response<'_>)>(response: *const sys::zebkit_response, data: *mut c_void) {
    let done = Box::from_raw(data.cast::<F>());
    let response = &*response;
    let headers = string_from(response.headers)
        .unwrap_or_default()
        .split("\r\n")
        .filter_map(|line| line.split_once(':'))
        .map(|(name, value)| (name.trim().to_string(), value.trim().to_string()))
        .collect();
    let body = if response.body.is_null() { &[][..] } else { std::slice::from_raw_parts(response.body, response.body_size) };
    let response = Response {
        status: u16::try_from(response.status).unwrap_or(0),
        url: string_from(response.url).unwrap_or_default(),
        headers,
        body,
        error: string_from(response.error),
    };
    crate::guard(|| done(response));
}
