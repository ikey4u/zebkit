/* Puts a client certificate and its key into an NSS database, as certutil and
 * pk12util would (tests/engine/client_certificate_stores.py; the machine need
 * not have NSS's tools):
 *   nss_import <database directory> <certificate.der> <key.pk8.der> <nickname>
 * The database is made when it is not there, without a password. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cert.h>
#include <keyhi.h>
#include <nss.h>
#include <pk11pub.h>
#include <prerror.h>
#include <secitem.h>

static int read_file(const char *path, SECItem *item) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    item->type = siBuffer;
    item->data = malloc(size > 0 ? (size_t)size : 1);
    item->len = (unsigned)fread(item->data, 1, (size_t)size, file);
    fclose(file);
    return item->len == (unsigned)size;
}

static int fail(const char *what) {
    fprintf(stderr, "nss_import: %s (NSS error %d)\n", what, PR_GetError());
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: nss_import <db dir> <cert.der> <key.pk8.der> <nickname>\n");
        return 2;
    }
    char database[4096];
    snprintf(database, sizeof database, "sql:%s", argv[1]);
    if (NSS_Initialize(database, "", "", SECMOD_DB, 0) != SECSuccess)
        return fail("NSS_Initialize");
    PK11SlotInfo *slot = PK11_GetInternalKeySlot();
    if (!slot) return fail("no key slot");
    if (PK11_NeedUserInit(slot) && PK11_InitPin(slot, NULL, "") != SECSuccess)
        return fail("PK11_InitPin");
    SECItem certificate, key;
    if (!read_file(argv[2], &certificate)) return fail("the certificate file");
    if (!read_file(argv[3], &key)) return fail("the key file");
    SECItem nickname = {siBuffer, (unsigned char *)argv[4], (unsigned)strlen(argv[4])};
    if (PK11_ImportDERPrivateKeyInfo(slot, &key, &nickname, NULL, PR_TRUE, PR_TRUE, KU_ALL, NULL) != SECSuccess)
        return fail("PK11_ImportDERPrivateKeyInfo");
    if (PK11_ImportDERCert(slot, &certificate, CK_INVALID_HANDLE, argv[4], PR_FALSE) != SECSuccess)
        return fail("PK11_ImportDERCert");
    PK11_FreeSlot(slot);
    if (NSS_Shutdown() != SECSuccess) return fail("NSS_Shutdown");
    printf("ok imported %s into %s\n", argv[4], argv[1]);
    return 0;
}
