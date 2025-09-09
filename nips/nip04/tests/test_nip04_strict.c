#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nostr/nip04.h>
#include <secure_buf.h>

/* This test assumes NIP04_STRICT_AEAD_ONLY=ON at build time.
 * It verifies that legacy ?iv= envelopes are rejected by decrypt APIs. */
int main(void) {
    const char *legacy_env = "ZmFrZWN0?iv=ZmFrZWl2"; /* fake base64 parts; strict path will reject before parsing */

    /* Non-secure decrypt should fail */
    char *pt = NULL; char *err = NULL;
    int rc = nostr_nip04_decrypt(
        legacy_env,
        "02aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", /* sender pub (hex) */
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",   /* receiver sk (hex) */
        &pt,
        &err);
    if (rc == 0) {
        fprintf(stderr, "strict decrypt unexpectedly succeeded (non-secure)\n");
        free(pt);
        return 1;
    }
    if (err) free(err);

    /* Secure decrypt should fail */
    nostr_secure_buf sb = secure_alloc(32);
    if (!sb.ptr) {
        fprintf(stderr, "secure_alloc failed\n");
        return 1;
    }
    memset(sb.ptr, 0x42, 32);
    pt = NULL; err = NULL;
    rc = nostr_nip04_decrypt_secure(
        legacy_env,
        "02aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", /* sender pub (hex) */
        &sb,
        &pt,
        &err);
    secure_free(&sb);
    if (rc == 0) {
        fprintf(stderr, "strict decrypt unexpectedly succeeded (secure)\n");
        free(pt);
        return 1;
    }
    if (err) free(err);

    printf("ok\n");
    return 0;
}
