# slim: zebkit for Windows. The same functions as slim on Linux
# (profiles/slim.cmake) and on macOS (profiles/macos/slim.xcconfig), from
# WebKit's Windows port, whose own defaults are a different set. Where this
# file and Linux's differ, the reason is written here; docs/WINDOWS.md has
# what is still missing.
#
# Size comes later: this is the feature set, built as the port builds
# (Release, no link-time optimization, the libraries as vcpkg's DLLs).

include(${CMAKE_CURRENT_LIST_DIR}/_common.cmake)

zeb_string(CMAKE_BUILD_TYPE Release)

# --- Media -------------------------------------------------------------------
# <video> and <audio> play through Media Foundation, the system's.
# Not on yet, and on in Linux's slim: Web Audio and Media Source Extensions.
# The port has no implementation of either to switch on (no audio output,
# no decoder for audio files, nothing behind MediaSource); see
# docs/WINDOWS.md, "Media".
zeb_option(ENABLE_VIDEO ON)
zeb_option(ENABLE_MEDIA_SESSION ON)
zeb_option(ENABLE_MEDIA_STREAM OFF)
zeb_option(ENABLE_MEDIA_RECORDER OFF)
zeb_option(ENABLE_ENCRYPTED_MEDIA OFF)
zeb_option(ENABLE_WEB_CODECS OFF)
zeb_option(ENABLE_WEB_RTC OFF)
zeb_option(ENABLE_SPEECH_SYNTHESIS OFF)
zeb_option(ENABLE_MEDIA_STATISTICS OFF)

# --- JavaScriptCore -----------------------------------------------------------
# Every JIT tier, as the port has them and as on macOS. Linux drops the top
# tiers for size after measuring what that costs; nothing is measured here
# yet (docs/WINDOWS.md).
zeb_option(ENABLE_SAMPLING_PROFILER OFF)

# --- Graphics ---------------------------------------------------------------
# The port draws in a GPU process and has no other way; Linux has none.
zeb_option(ENABLE_WEBGL ON)
zeb_option(ENABLE_OFFSCREEN_CANVAS ON)
zeb_option(ENABLE_OFFSCREEN_CANVAS_IN_WORKERS ON)
zeb_option(ENABLE_WEBXR OFF)
zeb_option(USE_AVIF OFF)
zeb_option(USE_JPEGXL OFF)
zeb_option(USE_LCMS OFF)
zeb_string(VCPKG_MANIFEST_FEATURES "web;skia;woff2")

# --- Input and scrolling --------------------------------------------------------
# Scrolling by the keyboard and by a wheel's notches is animated, as on
# Linux. What a CDP client scrolls is exact and is not (patches/2013).
zeb_option(ENABLE_TOUCH_EVENTS ON)
zeb_option(ENABLE_SMOOTH_SCROLLING ON)

# --- Developer and automation surfaces ----------------------------------------
zeb_option(ENABLE_REMOTE_INSPECTOR OFF)
zeb_option(ENABLE_WEBDRIVER OFF)
zeb_option(ENABLE_WEBDRIVER_BIDI OFF)
zeb_option(ENABLE_RESOURCE_USAGE OFF)
zeb_option(ENABLE_MEMORY_SAMPLER OFF)

# --- Web platform features outside the target use case -------------------------
zeb_option(ENABLE_PDFJS OFF)
zeb_option(ENABLE_XSLT OFF)
zeb_option(ENABLE_MHTML OFF)
zeb_option(ENABLE_GAMEPAD OFF)
zeb_option(ENABLE_GEOLOCATION ON)
zeb_option(ENABLE_SPELLCHECK OFF)
zeb_option(ENABLE_ATTACHMENT_ELEMENT OFF)
zeb_option(ENABLE_APPLICATION_MANIFEST OFF)

# The Chrome DevTools Protocol endpoint (src/cdp, docs/CDP.md). It drives the
# page through WebKit's inspector backend, which therefore stays in; the
# remote inspector server and WebDriver do not.
zeb_string(ZEBKIT_CDP_DIR "${CMAKE_CURRENT_LIST_DIR}/../../src/cdp")
