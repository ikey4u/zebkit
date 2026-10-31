/* The Keychain calls of whatever loads this library, answered from files in
 * $ZEBKIT_TEST_KEYCHAIN_DIR, so that the engine's tests leave a person's login
 * Keychain alone and raise no prompt: every persistent profile asks it for a key
 * under the program's name (src/engine/cookies_mac.mm). Nothing here reaches the
 * real Keychain. scripts/engine-test.sh loads it through DYLD_INSERT_LIBRARIES;
 * ENGINE_TEST_REAL_KEYCHAIN=1 goes without it.
 *
 * Built as a program as well (-DMACOS_KEYCHAIN_LAUNCH): it sets the variable and
 * runs its arguments, as the last step before the program under test, since macOS
 * clears DYLD_* on the way through its own protected programs (bash, env, perl).
 * With -DMACOS_KEYCHAIN_CHECK: finds an item only the files have, to prove that
 * the calls are redirected before a test is let near the Keychain. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(MACOS_KEYCHAIN_LAUNCH)
int main(int argc, char **argv) {
    const char *library = getenv("ZEBKIT_TEST_KEYCHAIN_LIBRARY");
    if (argc < 2 || !library) {
        fprintf(stderr, "usage: ZEBKIT_TEST_KEYCHAIN_LIBRARY=<dylib> %s <command...>\n", argv[0]);
        return 2;
    }
    setenv("DYLD_INSERT_LIBRARIES", library, 1);
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 127;
}
#else
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#if defined(MACOS_KEYCHAIN_CHECK)
int main(void) {
    const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData};
    const void *values[] = {kSecClassGenericPassword, CFSTR("zebkit-test-check"), CFSTR("zebkit-test-check"), kCFBooleanTrue};
    CFDictionaryRef query = CFDictionaryCreate(NULL, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef found = NULL;
    OSStatus status = SecItemCopyMatching(query, &found);
    if (status != errSecSuccess || !found)
        return 1;
    const void *data = CFDictionaryGetValue((CFDictionaryRef)found, kSecValueData);
    if (!data || CFDataGetLength((CFDataRef)data) != 5)
        return 1;
    puts("ok");
    return 0;
}
#else
/* The item's file: service and account as hexadecimal. 0 when there is none to name. */
static int item_path(CFDictionaryRef query, char *path, size_t size) {
    const char *directory = getenv("ZEBKIT_TEST_KEYCHAIN_DIR");
    CFStringRef parts[2] = {CFDictionaryGetValue(query, kSecAttrService), CFDictionaryGetValue(query, kSecAttrAccount)};
    if (!directory || !parts[0] || !parts[1])
        return 0;
    size_t used = (size_t)snprintf(path, size, "%s/", directory);
    for (int i = 0; i < 2; i++) {
        char text[512];
        if (!CFStringGetCString(parts[i], text, sizeof text, kCFStringEncodingUTF8))
            return 0;
        for (const char *c = text; *c && used + 3 < size; c++)
            used += (size_t)snprintf(path + used, size - used, "%02x", (unsigned char)*c);
        if (i == 0 && used + 2 < size)
            path[used++] = '-', path[used] = 0;
    }
    return 1;
}

static OSStatus write_item(const char *path, CFDictionaryRef attributes) {
    CFDataRef data = CFDictionaryGetValue(attributes, kSecValueData);
    FILE *file = data ? fopen(path, "wb") : NULL;
    if (!file)
        return errSecParam;
    fwrite(CFDataGetBytePtr(data), 1, (size_t)CFDataGetLength(data), file);
    fclose(file);
    return errSecSuccess;
}

static OSStatus private_copy_matching(CFDictionaryRef query, CFTypeRef *result) {
    char path[2048];
    if (result)
        *result = NULL;
    if (!item_path(query, path, sizeof path))
        return errSecItemNotFound;
    FILE *file = fopen(path, "rb");
    if (!file)
        return errSecItemNotFound;
    unsigned char bytes[4096];
    size_t size = fread(bytes, 1, sizeof bytes, file);
    fclose(file);
    if (result) {
        CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)size);
        const void *keys[] = {kSecValueData};
        const void *values[] = {data};
        *result = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(data);
    }
    return errSecSuccess;
}

static OSStatus private_add(CFDictionaryRef attributes, CFTypeRef *result) {
    char path[2048];
    if (result)
        *result = NULL;
    if (!item_path(attributes, path, sizeof path))
        return errSecParam;
    return access(path, F_OK) == 0 ? errSecDuplicateItem : write_item(path, attributes);
}

static OSStatus private_update(CFDictionaryRef query, CFDictionaryRef attributes) {
    char path[2048];
    if (!item_path(query, path, sizeof path) || access(path, F_OK) != 0)
        return errSecItemNotFound;
    return write_item(path, attributes);
}

static OSStatus private_delete(CFDictionaryRef query) {
    char path[2048];
    if (!item_path(query, path, sizeof path) || unlink(path) != 0)
        return errSecItemNotFound;
    return errSecSuccess;
}

#define INTERPOSE(replacement, original)                                              \
    __attribute__((used, section("__DATA,__interpose"))) static const struct {        \
        const void *with, *what;                                                      \
    } interpose_##original = {(const void *)replacement, (const void *)original}

INTERPOSE(private_copy_matching, SecItemCopyMatching);
INTERPOSE(private_add, SecItemAdd);
INTERPOSE(private_update, SecItemUpdate);
INTERPOSE(private_delete, SecItemDelete);
#endif
#endif
