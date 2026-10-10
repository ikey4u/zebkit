# slim-dev: the slim profile without link-time optimization.
#
# Not for shipping: it is several MiB larger and slower. It exists because a
# slim link re-optimizes the whole engine and takes minutes, whatever
# changed. Without LTO a one-file change relinks in seconds, which is what
# work on src/cdp or on a patch needs. Anything found here must be confirmed
# with slim before it counts.

include(${CMAKE_CURRENT_LIST_DIR}/slim.cmake)

zeb_string(LTO_MODE "")
