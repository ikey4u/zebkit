/* The C side of the Go binding's libraries (library.go): the engine's
 * callback types, passing an outcome, or a host.call() of the library's, on
 * to the Go function its handle names. */
#include <stdint.h>
#include <zebkit/library.h>

#include "_cgo_export.h"

void zk_library_told(zebkit_library *library, zebkit_library_code code, const char *json, const zebkit_part *parts,
                     size_t count, void *data) {
    (void)library;
    goZkLibraryTold((int)code, (char *)json, (zebkit_part *)parts, count, (uintptr_t)data);
}

void zk_library_host(zebkit_library *library, uint64_t request, const char *name, const char *args,
                     const zebkit_part *parts, size_t count, void *data) {
    (void)library;
    goZkLibraryHost(request, (char *)name, (char *)args, (zebkit_part *)parts, count, (uintptr_t)data);
}
