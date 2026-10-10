# baseline: upstream WPE defaults, built Release. This is the "before" number
# every other profile is compared against, so features stay at their defaults
# except where the host cannot provide a dependency (listed below).

include(${CMAKE_CURRENT_LIST_DIR}/_common.cmake)

zeb_string(CMAKE_BUILD_TYPE Release)

# Deviations from upstream defaults forced by missing host packages.
zeb_option(ENABLE_BUBBLEWRAP_SANDBOX OFF)   # needs bwrap + xdg-dbus-proxy at runtime
zeb_option(ENABLE_SPEECH_SYNTHESIS OFF)     # needs flite or libspiel
