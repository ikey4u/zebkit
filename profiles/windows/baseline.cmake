# baseline: WebKit's Windows port at its defaults, built Release. This is
# the "before" number every other profile is compared against.

include(${CMAKE_CURRENT_LIST_DIR}/_common.cmake)

zeb_string(CMAKE_BUILD_TYPE Release)
