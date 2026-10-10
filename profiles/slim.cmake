# slim: the size-targeted build. Budget: 60 MiB uncompressed for the engine
# plus every library we would have to bundle. Flags and linker settings only;
# anything that needs a source change lives in patches/.

include(${CMAKE_CURRENT_LIST_DIR}/_common.cmake)

# --- Compiler and linker -----------------------------------------------------
# The pinned cross toolchain (toolchain.lock, scripts/fetch-toolchain.sh)
# targets the oldest glibc we support, whatever the build host runs. It
# brings its own lld and a static libc++, so the result needs no C++ runtime
# from the target system.
get_filename_component(_zeb_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_zeb_toolchain "${_zeb_root}/out/toolchain/bin")
if (NOT EXISTS "${_zeb_toolchain}/zebkit-cc")
    message(FATAL_ERROR "no toolchain at ${_zeb_toolchain}; run scripts/fetch-toolchain.sh first")
endif ()
zeb_string(CMAKE_C_COMPILER "${_zeb_toolchain}/zebkit-cc")
zeb_string(CMAKE_CXX_COMPILER "${_zeb_toolchain}/zebkit-c++")
zeb_string(CMAKE_AR "${_zeb_toolchain}/zebkit-ar")
zeb_string(CMAKE_RANLIB "${_zeb_toolchain}/zebkit-ranlib")
zeb_option(USE_LD_LLD OFF)
# The toolchain's linker driver crashes on the dependency-file option.
zeb_option(CMAKE_LINK_DEPENDS_USE_LINKER OFF)

zeb_string(CMAKE_BUILD_TYPE MinSizeRel)
# The toolchain's compiler driver turns -Oz into -Os and turns frame pointers
# on. "-Xclang -Oz" reaches the compiler proper, and frame pointers are
# switched back off; together that is about 14% less code.
zeb_string(CMAKE_C_FLAGS_MINSIZEREL "-Oz -Xclang -Oz -fomit-frame-pointer -DNDEBUG")
zeb_string(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -Xclang -Oz -fomit-frame-pointer -DNDEBUG")
zeb_string(LTO_MODE thin)

# No unwind tables: about 7 MiB. Costs native stack traces in crash reports;
# C++ exceptions are already off in WebKit. The prefix map keeps this
# checkout's absolute path out of __FILE__ strings.
set(_zeb_cflags "-fno-asynchronous-unwind-tables -fno-unwind-tables -ffile-prefix-map=${_zeb_root}/=")

# One section per function and data object, so the linker can drop what
# nothing references. WebKit normally adds these itself after probing the
# linker, but its probe does not recognise the cross toolchain's linker.
string(APPEND _zeb_cflags " -ffunction-sections -fdata-sections")

# Minimal debug info. The toolchain's linker strips the symbol table unless
# objects carry some; with -g1 the unstripped build in out/<profile>/lib keeps
# symbols and line tables, which the size tools, the ClassInfo check and crash
# triage need. scripts/package.sh strips all of it from what ships.
string(APPEND _zeb_cflags " -g1")

# The x86 disassembler behind JSC's JIT-dump debugging options: 0.4 MiB.
string(APPEND _zeb_cflags " -DENABLE_ZYDIS=0")

# simdutf's AVX-512 code path does not compile with this toolchain's clang
# (its 512-bit intrinsics now need an extra target feature). The AVX2 and
# SSE paths remain and are chosen at run time.
string(APPEND _zeb_cflags " -DSIMDUTF_IMPLEMENTATION_ICELAKE=0")
zeb_string(CMAKE_C_FLAGS "${_zeb_cflags}")

# C++ additionally gets include/size-prelude.h, which strips the diagnostic
# strings out of assertion failures while keeping the checks.
zeb_string(CMAKE_CXX_FLAGS "${_zeb_cflags} -include ${CMAKE_CURRENT_LIST_DIR}/include/size-prelude.h")

# Linker:
# - unreferenced sections are dropped (see the compiler flags above)
# - symbols named in WebKit's export list but absent from this configuration
#   are not an error
# Not available with this toolchain's linker driver:
# - packed relative relocations (5.7 MiB): they need glibc 2.36 at run time
# - a link map for scripts/mapsize.py, and LLVM function merging (1.2 MiB)
# Not enabled, on purpose:
# - identical-code folding (--icf=safe), 2.2 MiB. Under ThinLTO lld also
#   folded 30 distinct JSC::ClassInfo objects into 3. JavaScriptCore tells
#   types apart by ClassInfo address, so the DFG JIT built a Map for
#   "new Set" and most large sites broke. Do not turn it on.
# - the machine outliner, 1.8 MiB. It roughly doubles JSON and DOM benchmark
#   times.
# - libraries named with -l are looked up in deps/ only; nothing is linked
#   from the host, so the engine cannot pick up a system library by accident
set(_zeb_ldflags "-Wl,--gc-sections -Wl,--undefined-version -L${_zeb_root}/out/deps/prefix/lib")
zeb_string(CMAKE_SHARED_LINKER_FLAGS "${_zeb_ldflags}")
zeb_string(CMAKE_EXE_LINKER_FLAGS "${_zeb_ldflags}")
zeb_string(CMAKE_MODULE_LINKER_FLAGS "${_zeb_ldflags}")

# Installed binaries look for their libraries in their own directory, which
# is what makes the flat package from scripts/package.sh relocatable.
zeb_string(CMAKE_INSTALL_RPATH "\$ORIGIN")

# --- Third-party libraries from deps/, static and size-optimized --------------
zeb_use_deps()

# --- Media -------------------------------------------------------------------
# Playback goes through GStreamer, linked in as one static library with a
# fixed plugin set and FFmpeg's decoders (deps/gstreamer, deps/ffmpeg).
# On: <video>/<audio>, Media Source Extensions, Web Audio, Media Session.
# Off: capture, recording, WebRTC, WebCodecs, encrypted media, speech, and
# GStreamer's GL path (video frames are uploaded from memory instead).
zeb_option(USE_GSTREAMER ON)
zeb_option(USE_GSTREAMER_FULL ON)
zeb_option(USE_GSTREAMER_GL OFF)
zeb_option(USE_GSTREAMER_WEBRTC OFF)
zeb_option(ENABLE_VIDEO ON)
zeb_option(ENABLE_VIDEO_USES_ELEMENT_FULLSCREEN ON)
zeb_option(ENABLE_WEB_AUDIO ON)
zeb_option(ENABLE_MEDIA_SOURCE ON)
zeb_option(ENABLE_MEDIA_SESSION ON)
zeb_option(ENABLE_MEDIA_STREAM OFF)
zeb_option(ENABLE_MEDIA_RECORDER OFF)
zeb_option(ENABLE_ENCRYPTED_MEDIA OFF)
zeb_option(ENABLE_WEB_CODECS OFF)
zeb_option(ENABLE_WEB_RTC OFF)
zeb_option(ENABLE_SPEECH_SYNTHESIS OFF)

# --- JavaScriptCore: keep interpreter + baseline + DFG, drop the top tiers ---
zeb_option(ENABLE_FTL_JIT OFF)
zeb_option(ENABLE_WEBASSEMBLY_BBQJIT OFF)
zeb_option(ENABLE_WEBASSEMBLY_OMGJIT OFF)
zeb_option(ENABLE_SAMPLING_PROFILER OFF)
zeb_option(ENABLE_JAVASCRIPT_SHELL OFF)       # jsc test shell; not shipped, and does not build with the JIT tiers off

# --- Graphics ---------------------------------------------------------------
# WebGL is on. Besides 3D content, scripts on large sites (baidu.com, for
# one) create a WebGL context without checking and throw if they cannot.
zeb_option(ENABLE_WEBGL ON)
zeb_option(ENABLE_WEBXR OFF)
zeb_option(ENABLE_GPU_PROCESS OFF)
zeb_option(USE_AVIF OFF)
zeb_option(USE_JPEGXL OFF)
zeb_option(USE_LCMS OFF)

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
# Geolocation stays on: with it off, navigator.permissions.query() rejects for
# "geolocation" and sites that ask without a catch handler report errors.
zeb_option(ENABLE_GEOLOCATION ON)
zeb_option(ENABLE_SPELLCHECK OFF)
zeb_option(ENABLE_CONTENT_EXTENSIONS OFF)
zeb_option(ENABLE_WK_WEB_EXTENSIONS OFF)
zeb_option(ENABLE_BUBBLEWRAP_SANDBOX OFF)
zeb_option(USE_ATK OFF)
zeb_option(USE_LIBHYPHEN OFF)

# --- Display ------------------------------------------------------------------
# Chosen at run time, in this order: Wayland, X11 (patches/2004), offscreen.
# libwayland-client and libX11 are loaded on demand (deps/syslibs), so a
# backend whose library or server is missing declines and the next is tried.
zeb_option(ENABLE_WPE_PLATFORM ON)
zeb_option(ENABLE_WPE_PLATFORM_WAYLAND ON)
zeb_option(ENABLE_WPE_PLATFORM_X11 ON)
zeb_option(ENABLE_WPE_PLATFORM_HEADLESS ON)
zeb_option(ENABLE_WPE_PLATFORM_DRM OFF)
zeb_option(ENABLE_WPE_LEGACY_API OFF)

# The Chrome DevTools Protocol endpoint (src/cdp, docs/CDP.md). It drives the
# page through WebKit's inspector backend, which therefore stays in; the
# remote inspector server and WebDriver do not.
zeb_string(ZEBKIT_CDP_DIR "${CMAKE_CURRENT_LIST_DIR}/../src/cdp")
