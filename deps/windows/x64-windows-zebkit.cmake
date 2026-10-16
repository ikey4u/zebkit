# The vcpkg triplet for the libraries WebKit's Windows port links (the
# manifest is WebKit's own vcpkg.json): x86_64 Windows, built on Linux with
# clang-cl against Microsoft's SDK and C++ runtime. It is WebKit's
# x64-windows-webkit triplet with the tools taken from out/toolchain/windows
# (scripts/fetch-toolchain-windows.sh) instead of from /usr/bin.

set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)

get_filename_component(_zeb_toolchain "${CMAKE_CURRENT_LIST_DIR}/../../out/toolchain/windows" ABSOLUTE)
set(_zeb_llvm "${_zeb_toolchain}/llvm/bin")
set(_zeb_sdk_inc "${_zeb_toolchain}/sysroot/sdk/include")
set(_zeb_crt_inc "${_zeb_toolchain}/sysroot/crt/include")
set(_zeb_sdk_lib "${_zeb_toolchain}/sysroot/sdk/lib")
set(_zeb_crt_lib "${_zeb_toolchain}/sysroot/crt/lib")

# vcpkg's own Windows toolchain wants a Visual Studio prompt; WebKit's
# stand-in for it does not. It is the same file for every WebKit tree, so
# the one in the Windows tree is used.
file(GLOB _zeb_chainload "${CMAKE_CURRENT_LIST_DIR}/../../upstream/webkit-*-windows/WebKitLibraries/toolchains/windows-cross.cmake")
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${_zeb_chainload}")

set(ENV{CC} "${_zeb_llvm}/clang-cl")
set(ENV{CXX} "${_zeb_llvm}/clang-cl")
set(ENV{LDFLAGS} "")

set(VCPKG_C_FLAGS "-imsvc ${_zeb_crt_inc} -imsvc ${_zeb_sdk_inc}/ucrt -imsvc ${_zeb_sdk_inc}/um -imsvc ${_zeb_sdk_inc}/shared -imsvc ${_zeb_sdk_inc}/winrt")
set(VCPKG_CXX_FLAGS "${VCPKG_C_FLAGS}")
set(VCPKG_LINKER_FLAGS "/LIBPATH:${_zeb_crt_lib}/x64 /LIBPATH:${_zeb_sdk_lib}/ucrt/x64 /LIBPATH:${_zeb_sdk_lib}/um/x64")

# For the build systems that read these instead of taking flags.
set(ENV{INCLUDE} "${_zeb_crt_inc};${_zeb_sdk_inc}/ucrt;${_zeb_sdk_inc}/um;${_zeb_sdk_inc}/shared;${_zeb_sdk_inc}/winrt")
set(ENV{LIB} "${_zeb_crt_lib}/x64;${_zeb_sdk_lib}/ucrt/x64;${_zeb_sdk_lib}/um/x64")

# The host's packages are never the ones to find. scripts/build-windows.sh
# sets PKG_CONFIG_LIBDIR to the directory vcpkg installs into.
set(VCPKG_ENV_PASSTHROUGH_UNTRACKED PKG_CONFIG_LIBDIR)

find_program(_zeb_ninja NAMES ninja ninja-build REQUIRED)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    -DCMAKE_MAKE_PROGRAM=${_zeb_ninja}
    -DCMAKE_AR=${_zeb_llvm}/llvm-lib
    -DCMAKE_LINKER=${_zeb_llvm}/lld-link
    -DCMAKE_MT=${_zeb_llvm}/llvm-mt
    -DCMAKE_RC_COMPILER=${_zeb_llvm}/llvm-rc
    "-DCMAKE_RC_FLAGS=-I ${_zeb_crt_inc} -I ${_zeb_sdk_inc}/ucrt -I ${_zeb_sdk_inc}/um -I ${_zeb_sdk_inc}/shared")

set(CMAKE_EXECUTABLE_SUFFIX_C ".exe")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".exe")

# ICU's data library, built on Linux, exports nothing unless told to.
if (PORT STREQUAL "icu")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/icu-data-export.cmake")
endif ()

# As in WebKit's triplet.
if (PORT STREQUAL "highway" OR PORT STREQUAL "pixman")
    set(VCPKG_LIBRARY_LINKAGE static)
endif ()
if (PORT STREQUAL "zlib-ng")
    set(ZLIB_COMPAT ON)
endif ()
