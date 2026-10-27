/* Owned cookie-key fixture; never installed as an application bridge. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zebkit/zebkit.h>

static const char *url;
static void closed(zebkit_view *view, void *data) {
    (void)view; (void)data;
    zebkit_quit();
}
static void ready(void *data) {
    (void)data;
    if (zebkit_set_cookie_encryption_key(NULL, 0) ||
        zebkit_set_cookie_storage(ZEBKIT_COOKIE_STORAGE_FILE, NULL)) abort();
    zebkit_view *view = zebkit_view_new(url, 640, 480);
    zebkit_view_on_close(view, closed, NULL);
}
int main(int argc, char **argv) {
    int builtin = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--test-builtin-cookie-storage")) builtin = 1;
    if (builtin) {
        if (zebkit_set_cookie_storage((zebkit_cookie_storage)9, NULL)) return 7;
        if (zebkit_set_cookie_storage(ZEBKIT_COOKIE_STORAGE_FILE, "relative")) return 8;
        if (!zebkit_set_cookie_storage(ZEBKIT_COOKIE_STORAGE_FILE, NULL)) return 9;
    } else {
        const char *path = getenv("ZEBKIT_TEST_COOKIE_KEY_FILE");
        unsigned char key[32];
        FILE *file = path ? fopen(path, "rb") : NULL;
        if (!file || fread(key, 1, sizeof(key), file) != sizeof(key) || fgetc(file) != EOF) return 2;
        fclose(file);
        if (zebkit_set_cookie_encryption_key(key, 31)) return 3;
        if (!zebkit_set_cookie_encryption_key(key, sizeof(key))) return 4;
        if (!zebkit_set_cookie_encryption_key(NULL, 0)) return 5;
        if (!zebkit_set_cookie_encryption_key(key, sizeof(key))) return 6;
    }
    url = argv[argc - 1];
    return zebkit_main(argc, argv, ready, NULL);
}
