/*
 * Force-included into every C++ translation unit of size-targeted profiles
 * (see profiles/slim.cmake). It keeps the run-time checks WebKit ships with
 * but drops the diagnostic strings they carry.
 *
 * The C++ standard library's hardening assertions (libstdc++ or libc++) and
 * WTF's RELEASE_ASSERT pass the
 * file name and the full C++ signature of the enclosing function to their
 * failure handler. Every template instantiation gets its own signature
 * string, which added up to about 4 MiB of text plus the code to load it.
 * A failed check still stops the process at the same place; it just traps
 * without saying where.
 */

#pragma once

#if defined(__cplusplus)

/* libstdc++: __glibcxx_assert(cond) expands _GLIBCXX_ASSERT_FAIL when it is
 * used, so redefining it here, before any other standard header is parsed,
 * replaces the verbose handler for the whole translation unit. */
#if __has_include(<bits/c++config.h>)
#include <bits/c++config.h>
#ifdef _GLIBCXX_ASSERT_FAIL
#undef _GLIBCXX_ASSERT_FAIL
#define _GLIBCXX_ASSERT_FAIL(_Condition) __builtin_trap()
#endif
#endif

/* libc++: the same idea. Its hardening checks call _LIBCPP_ASSERTION_HANDLER
 * with a message naming the file, line and failed expression. */
#if __has_include(<__assertion_handler>)
#include <__config>
#include <__assertion_handler>
#undef _LIBCPP_ASSERTION_HANDLER
#define _LIBCPP_ASSERTION_HANDLER(message) __builtin_trap()
#endif

/* WTF: Assertions.h only defines CRASH_WITH_INFO if nobody else has. The
 * extra arguments are still type-checked, then discarded. */
#define CRASH_WITH_INFO(...) do { \
        WTF::isIntegralOrPointerType(__VA_ARGS__); \
        CRASH(); \
    } while (false)

/* Read by the 1xxx patches in patches/: code that this configuration can
 * never reach (inspector agents without a frontend, the accessibility tree
 * without ATK) returns early, so the linker can discard what is behind it. */
#define ZEBKIT_STRIP_UNREACHABLE 1

/* Read by patches/2001: look for helper processes, the injected bundle and
 * GIO plugins next to the engine library before the compiled-in paths. */
#define ZEBKIT_RELOCATABLE 1

/* Read by patches/2004: display, GPU buffer and sound libraries are
 * lazy-loading stand-ins (deps/syslibs), so code may ask whether the real
 * library is there. */
#define ZEBKIT_OPTIONAL_SYSTEM_LIBRARIES 1

#endif /* __cplusplus */
