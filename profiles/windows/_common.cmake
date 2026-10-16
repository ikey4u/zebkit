# Settings shared by every Windows profile. Profiles are CMake initial-cache
# files (cmake -C); a leading underscore marks a fragment that is not a
# profile. The compiler, the SDK and the libraries are not chosen here:
# scripts/build-windows.sh passes them, since they are paths on this machine.

macro(zeb_option name value)
    set(${name} ${value} CACHE BOOL "" FORCE)
endmacro()

macro(zeb_string name value)
    set(${name} "${value}" CACHE STRING "" FORCE)
endmacro()

zeb_string(PORT Win)

# Tests and their tools, which never ship in the engine, so turning them off
# does not distort size comparisons between profiles.
zeb_option(ENABLE_API_TESTS OFF)
zeb_option(ENABLE_LAYOUT_TESTS OFF)

# MiniBrowser is the smoke-test shell; it is not counted in the size.
zeb_option(ENABLE_MINIBROWSER ON)

# Which of the libraries in WebKit's vcpkg.json are built. They follow the
# engine's options: Skia for graphics, and the image and font formats that
# are on. A profile that turns one of those off takes its library out here.
zeb_string(VCPKG_MANIFEST_FEATURES "web;skia;woff2;jpeg-xl;lcms")
