package zebkit

/*
#include "bridge.h"
*/
import "C"

import "strings"

// What the engine has for applications besides views (<zebkit/services.h>):
// compression, digests and encryption, URLs, images, ICU and HTTP. None of
// it needs the engine running but Fetch, which is the engine thread's; any
// goroutine may call the rest.

// Compression is a compressed format.
type Compression int

const (
	CompressGzip       Compression = 1 // RFC 1952, as Content-Encoding: gzip
	CompressZlib       Compression = 2 // RFC 1950, as Content-Encoding: deflate
	CompressDeflateRaw Compression = 3 // RFC 1951, no header
	CompressBrotli     Compression = 4 // RFC 7932
	CompressZstd       Compression = 5 // RFC 8878
)

// Compress compresses data. level -1 is the format's default; otherwise 0-9
// for zlib's formats, 0-11 for Brotli, 1-22 for Zstandard.
func Compress(format Compression, data []byte, level int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_compress(C.zebkit_compression(format), bytesArg(data), C.size_t(len(data)), C.int(level), &size, &message)
	if out == nil {
		return nil, takeError(message, "not compressed")
	}
	return takeBytes(out, size), nil
}

// Decompress decompresses data, failing rather than produce more than
// maxSize bytes (0: 256 MiB).
func Decompress(format Compression, data []byte, maxSize int) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_decompress(C.zebkit_compression(format), bytesArg(data), C.size_t(len(data)), C.size_t(maxSize), &size, &message)
	if out == nil {
		return nil, takeError(message, "not decompressed")
	}
	return takeBytes(out, size), nil
}

// Digest is a hash function.
type Digest int

const (
	SHA1   Digest = 1
	SHA256 Digest = 2
	SHA384 Digest = 3
	SHA512 Digest = 4
)

// Size is the digest's length in bytes: 20, 32, 48 or 64; 0 for an unknown one.
func (d Digest) Size() int { return int(C.zebkit_digest_size(C.zebkit_digest(d))) }

// ComputeDigest is the digest of data.
func ComputeDigest(d Digest, data []byte) ([]byte, error) {
	out := make([]byte, d.Size())
	if len(out) == 0 || !C.zebkit_digest_compute(C.zebkit_digest(d), bytesArg(data), C.size_t(len(data)), (*C.uint8_t)(bytesArg(out))) {
		return nil, refused("no such digest")
	}
	return out, nil
}

// HMAC is the HMAC of data with key.
func HMAC(d Digest, key, data []byte) ([]byte, error) {
	out := make([]byte, d.Size())
	if len(out) == 0 || !C.zebkit_hmac(C.zebkit_digest(d), bytesArg(key), C.size_t(len(key)), bytesArg(data), C.size_t(len(data)),
		(*C.uint8_t)(bytesArg(out))) {
		return nil, refused("no such digest")
	}
	return out, nil
}

// RandomBytes is size bytes from the system's secure random source.
func RandomBytes(size int) ([]byte, error) {
	out := make([]byte, size)
	if size > 0 && !C.zebkit_random_bytes(bytesArg(out), C.size_t(size)) {
		return nil, refused("no random bytes")
	}
	return out, nil
}

// AESGCMSeal encrypts plaintext with AES-GCM: a 16- or 32-byte key, a 12-byte
// nonce never used twice with the key, and ad authenticated but not
// encrypted. It returns the ciphertext followed by the 16-byte tag.
func AESGCMSeal(key, nonce, ad, plaintext []byte) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_aes_gcm_seal(bytesArg(key), C.size_t(len(key)), bytesArg(nonce), C.size_t(len(nonce)),
		bytesArg(ad), C.size_t(len(ad)), bytesArg(plaintext), C.size_t(len(plaintext)), &size, &message)
	if out == nil {
		return nil, takeError(message, "not sealed")
	}
	return takeBytes(out, size), nil
}

// AESGCMOpen decrypts what AESGCMSeal made, failing when anything was changed.
func AESGCMOpen(key, nonce, ad, sealed []byte) ([]byte, error) {
	var size C.size_t
	var message *C.char
	out := C.zebkit_aes_gcm_open(bytesArg(key), C.size_t(len(key)), bytesArg(nonce), C.size_t(len(nonce)),
		bytesArg(ad), C.size_t(len(ad)), bytesArg(sealed), C.size_t(len(sealed)), &size, &message)
	if out == nil {
		return nil, takeError(message, "not opened")
	}
	// An empty plaintext is still a buffer of the engine's to free.
	return takeBytes(out, size), nil
}

// URLPart is a part of a URL (URLGet).
type URLPart int

const (
	URLSpec     URLPart = 0 // the whole URL, canonical
	URLScheme   URLPart = 1 // "https"
	URLUsername URLPart = 2
	URLPassword URLPart = 3
	URLHost     URLPart = 4 // "example.com", "[::1]"; punycode for IDN
	URLPort     URLPart = 5 // "" when it is the scheme's default
	URLPath     URLPart = 6 // "/a/b"
	URLQuery    URLPart = 7 // without the "?"
	URLFragment URLPart = 8 // without the "#"
	URLOrigin   URLPart = 9 // "https://example.com", or "null"
)

// URLGet is a part of url, resolved against base unless base is "", as a
// browser parses and canonicalizes it; false when it is not a valid URL.
func URLGet(url, base string, part URLPart) (string, bool) {
	curl := cString(url)
	defer free(curl)
	cbase := optionalString(base)
	defer free(cbase)
	out := C.zebkit_url_get(curl, cbase, C.zebkit_url_part(part))
	if out == nil {
		return "", false
	}
	return takeString(out), true
}

// RegistrableDomain is the registrable domain of a host or URL by the public
// suffix list ("example.co.uk"); "" for a host that has none; false when invalid.
func RegistrableDomain(hostOrURL string) (string, bool) {
	chost := cString(hostOrURL)
	defer free(chost)
	out := C.zebkit_url_registrable_domain(chost)
	if out == nil {
		return "", false
	}
	return takeString(out), true
}

// Pixels are 8-bit RGBA, not premultiplied, rows top to bottom,
// Width * Height * 4 bytes.
type Pixels struct {
	Width, Height int
	RGBA          []byte
}

// complete says whether the pixels are as many as the size says.
func (p *Pixels) complete() bool {
	return p != nil && p.Width > 0 && p.Height > 0 && uint64(len(p.RGBA)) >= uint64(p.Width)*uint64(p.Height)*4
}

// ImageFormat is a format EncodeImage writes.
type ImageFormat int

const (
	ImagePNG  ImageFormat = 1
	ImageJPEG ImageFormat = 2
	ImageWebP ImageFormat = 3
)

// DecodeImage decodes an image file (PNG, JPEG, WebP, and what else the
// engine's Skia reads).
func DecodeImage(data []byte) (*Pixels, error) {
	var width, height C.int
	var message *C.char
	out := C.zebkit_image_decode(bytesArg(data), C.size_t(len(data)), &width, &height, &message)
	if out == nil {
		return nil, takeError(message, "not decoded")
	}
	return &Pixels{Width: int(width), Height: int(height), RGBA: takeBytes(out, rgbaSize(width, height))}, nil
}

// EncodeImage encodes pixels. quality 0-100 is for JPEG and lossy WebP; 101
// asks WebP for lossless; PNG ignores it.
func EncodeImage(format ImageFormat, image *Pixels, quality int) ([]byte, error) {
	if !image.complete() {
		return nil, refused("fewer pixels than the size says")
	}
	var size C.size_t
	var message *C.char
	out := C.zebkit_image_encode(C.zebkit_image_format(format), (*C.uint8_t)(bytesArg(image.RGBA)), C.int(image.Width),
		C.int(image.Height), C.int(quality), &size, &message)
	if out == nil {
		return nil, takeError(message, "not encoded")
	}
	return takeBytes(out, size), nil
}

// ICUInit loads ICU's data so that ICU can be used without the engine
// running; calling it again, or with the engine running, does no harm.
func ICUInit() error { return check(C.zebkit_icu_init(), "ICU's data was not found") }

// FetchResponse is what Fetch got: Err is set when the request or its body
// failed, and Status is 0 when there was no response.
type FetchResponse struct {
	Status  int
	URL     string // after redirects
	Headers string // "Name: value" lines, separated by "\r\n"
	Body    []byte
	Err     error
}

// HeaderList is Headers parsed: name and value pairs, trimmed, in their
// order, a header given more than once (Set-Cookie) as often as it was.
func (r *FetchResponse) HeaderList() [][2]string {
	var list [][2]string
	for _, line := range strings.Split(r.Headers, "\r\n") {
		name, value, ok := strings.Cut(line, ":")
		if name = strings.TrimSpace(name); ok && name != "" {
			list = append(list, [2]string{name, strings.TrimSpace(value)})
		}
	}
	return list
}

// Fetch sends a request through the engine's network stack (its HTTP/1.1,
// 2 and 3, cache, cookies and proxy), as the browser makes one of its own:
// no CORS, no page. method "" is GET; headers are "Name: value" lines
// separated by "\r\n", or ""; redirects are followed. done is called once,
// on the engine's thread; the engine must be running.
func Fetch(method, url, headers string, body []byte, done func(*FetchResponse)) error {
	onEngineThread()
	if !started.Load() {
		return ErrNotStarted
	}
	cmethod := optionalString(method)
	defer free(cmethod)
	curl := cString(url)
	defer free(curl)
	cheaders := optionalString(headers)
	defer free(cheaders)
	h := onceHandle(func(response *FetchResponse) {
		if done != nil {
			done(response)
		}
	})
	C.zebkit_fetch(cmethod, curl, cheaders, bytesArg(body), C.size_t(len(body)), C.zebkit_response_func(C.zk_fetch), data(h))
	return nil
}
