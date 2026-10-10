/*
 * The run-time half of the lazy system-library stubs (see gen.py).
 *
 * zeb_syslib_resolve() is called the first time a stubbed function is used.
 * It loads the library if that has not been tried yet, looks the function
 * up, and stores its address in the stub's pointer. When the library or the
 * function is missing it stores a function that reports failure instead.
 *
 * zebkit_syslib_available() lets a backend ask up front whether a library can
 * be loaded at all, and zebkit_syslib_has_symbol() whether it is new enough
 * to have a particular function.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LIBRARIES 16

static struct {
    const char* soname;
    void* handle;
} libraries[MAX_LIBRARIES];
static int libraryCount;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static long missingZero(void) { return 0; }
static long missingNegative(void) { return -1; }
static const char* missingString(void) { return ""; }

/* ZEBKIT_WITHOUT_LIBRARIES="libX11.so.6:libwayland-client.so.0" makes the
 * named libraries count as not installed. It exists so that a machine that
 * has them can test, or be made to behave like, one that does not. */
static int withheld(const char* soname)
{
    const char* list = getenv("ZEBKIT_WITHOUT_LIBRARIES");
    size_t length = strlen(soname);
    while (list && *list) {
        const char* end = strchr(list, ':');
        size_t itemLength = end ? (size_t)(end - list) : strlen(list);
        if (itemLength == length && !strncmp(list, soname, length))
            return 1;
        list = end ? end + 1 : NULL;
    }
    return 0;
}

static void* load(const char* soname)
{
    for (int i = 0; i < libraryCount; i++) {
        if (!strcmp(libraries[i].soname, soname))
            return libraries[i].handle;
    }
    /* RTLD_GLOBAL: the GL driver must see the same libwayland-client we use. */
    void* handle = withheld(soname) ? NULL : dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
    /* ZEBKIT_SYSLIB_LOG=1 reports each first use, for diagnosing which
     * optional libraries a machine provides. */
    if (getenv("ZEBKIT_SYSLIB_LOG"))
        fprintf(stderr, "zebkit: optional library %s %s\n", soname, handle ? "loaded" : "not available");
    if (libraryCount < MAX_LIBRARIES) {
        libraries[libraryCount].soname = soname;
        libraries[libraryCount].handle = handle;
        libraryCount++;
    }
    return handle;
}

__attribute__((visibility("hidden")))
void zeb_syslib_resolve(const char* soname, const char* symbol, void** pointer, int missingKind)
{
    pthread_mutex_lock(&lock);
    void* handle = load(soname);
    void* address = handle ? dlsym(handle, symbol) : NULL;
    if (!address) {
        switch (missingKind) {
        case 1: address = (void*)missingNegative; break;
        case 2: address = (void*)missingString; break;
        default: address = (void*)missingZero; break;
        }
    }
    __atomic_store_n(pointer, address, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&lock);
}

__attribute__((visibility("default")))
int zebkit_syslib_available(const char* soname)
{
    pthread_mutex_lock(&lock);
    void* handle = load(soname);
    pthread_mutex_unlock(&lock);
    return handle != NULL;
}

__attribute__((visibility("default")))
int zebkit_syslib_has_symbol(const char* soname, const char* symbol)
{
    pthread_mutex_lock(&lock);
    void* handle = load(soname);
    pthread_mutex_unlock(&lock);
    return handle && dlsym(handle, symbol);
}
