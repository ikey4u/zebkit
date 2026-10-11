# Settings shared by every profile. Profiles are CMake initial-cache files
# (cmake -C); a leading underscore marks a fragment that is not a profile.

macro(zeb_option name value)
    set(${name} ${value} CACHE BOOL "" FORCE)
endmacro()

macro(zeb_string name value)
    set(${name} "${value}" CACHE STRING "" FORCE)
endmacro()

# zeb_use_deps(): link the libraries built by scripts/build-deps.sh instead
# of the host's. They are static, so the engine link keeps only what it uses.
macro(zeb_use_deps)
    get_filename_component(_zeb_deps "${CMAKE_CURRENT_LIST_DIR}/../out/deps/prefix" ABSOLUTE)
    if (NOT EXISTS "${_zeb_deps}/lib")
        message(FATAL_ERROR "no dependency prefix at ${_zeb_deps}; run scripts/build-deps.sh first")
    endif ()
    zeb_string(CMAKE_PREFIX_PATH "${_zeb_deps}")
    zeb_string(ICU_ROOT "${_zeb_deps}")
    # Static libraries do not carry their own dependencies, and WebKit's find
    # modules only know the public ones. These go at the end of every link:
    # libsharpyuv for libwebp, brotli for FreeType's WOFF2 support.
    set(_zeb_private_libs "${_zeb_deps}/lib/libsharpyuv.a ${_zeb_deps}/lib/libbrotlidec.a ${_zeb_deps}/lib/libbrotlicommon.a")

    # Media: WebKit links libgstreamer-full, which holds GStreamer's core and
    # calls each plugin's registration function. The plugins themselves, the
    # helper libraries they use and FFmpeg are separate static archives. The
    # linker takes from them only what is referenced.
    file(GLOB _zeb_gst_plugins "${_zeb_deps}/lib/gstreamer-1.0/*.a")
    file(GLOB _zeb_gst_libs "${_zeb_deps}/lib/libgst*-1.0.a")
    list(FILTER _zeb_gst_libs EXCLUDE REGEX "libgstreamer-full-1\\.0\\.a$")
    if (_zeb_gst_plugins)
        foreach (_zeb_lib IN LISTS _zeb_gst_plugins _zeb_gst_libs)
            string(APPEND _zeb_private_libs " ${_zeb_lib}")
        endforeach ()
        foreach (_zeb_lib avfilter avformat avcodec swresample swscale avutil)
            string(APPEND _zeb_private_libs " ${_zeb_deps}/lib/lib${_zeb_lib}.a")
        endforeach ()
        # ALSA is a lazy-loading stand-in from deps/syslibs, like the display
        # and GPU buffer libraries.
        string(APPEND _zeb_private_libs " ${_zeb_deps}/lib/libasound.a")
    endif ()
    # Xlib for the X11 display backend, and the loader behind every
    # stand-in in deps/syslibs.
    string(APPEND _zeb_private_libs " ${_zeb_deps}/lib/libX11.a ${_zeb_deps}/lib/libzebkit-syslib.a")
    zeb_string(CMAKE_C_STANDARD_LIBRARIES "${_zeb_private_libs}")
    zeb_string(CMAKE_CXX_STANDARD_LIBRARIES "${_zeb_private_libs}")
endmacro()

zeb_string(PORT WPE)
zeb_string(CMAKE_C_COMPILER clang)
zeb_string(CMAKE_CXX_COMPILER clang++)
zeb_option(USE_LD_LLD ON)

# Developer tooling and packaging extras that never ship in the engine, so
# turning them off does not distort size comparisons between profiles.
zeb_option(ENABLE_DOCUMENTATION OFF)
zeb_option(ENABLE_INTROSPECTION OFF)
zeb_option(ENABLE_API_TESTS OFF)
zeb_option(ENABLE_LAYOUT_TESTS OFF)
zeb_option(ENABLE_JOURNALD_LOG OFF)
zeb_option(USE_LIBBACKTRACE OFF)
zeb_option(USE_SYSPROF_CAPTURE OFF)
zeb_option(ENABLE_WPE_QT_API OFF)
zeb_option(ENABLE_COG OFF)

# MiniBrowser is the smoke-test shell; scripts/measure.sh does not count it.
zeb_option(ENABLE_MINIBROWSER ON)
