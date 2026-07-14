#include <nostr/nip04.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    const char *sender_sk_hex = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *receiver_sk_hex = "0000000000000000000000000000000000000000000000000000000000000002";
    const char *sender_pk_hex =   "0279be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";
    const char *receiver_pk_hex = "02c6047f9441ed7d6d3045406e95c07cd85c778e4b8cef3ca7abac09b95c709ee5";

    const char *msg = "Hello, NIP-04!";
    char *content = NULL; char *err = NULL;
    if (nostr_nip04_encrypt(msg, receiver_pk_hex, sender_sk_hex, &content, &err) != 0) {
        fprintf(stderr, "encrypt failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    if (strncmp(content, "v=2:", 4) == 0 || strstr(content, "?iv=") == NULL) {
        fprintf(stderr, "expected standard NIP-04 envelope, got: %s\n", content);
        free(content);
        return 1;
    }

    char *pt = NULL;
    if (nostr_nip04_decrypt(content, sender_pk_hex, receiver_sk_hex, &pt, &err) != 0) {
        fprintf(stderr, "decrypt failed: %s\n", err ? err : "unknown");
        free(err);
        free(content);
        return 1;
    }

    if (strcmp(pt, msg) != 0) {
        fprintf(stderr, "plaintext mismatch: got '%s' exp '%s'\n", pt, msg);
        free(pt); free(content);
        return 1;
    }

    free(pt);
    free(content);

    const char *fixture_msg = "nostr-tools nip04 fixture";
    const char *fixture_content = "EPr2K0Ys12tMRVsLxh6Yw14bN62Ci3kbYGagq7lT3QU=?iv=xF+EsvVJ3AK1K5i9nQKRfg==";
    pt = NULL; err = NULL;
    if (nostr_nip04_decrypt(fixture_content, sender_pk_hex, receiver_sk_hex, &pt, &err) != 0) {
        fprintf(stderr, "nostr-tools fixture decrypt failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    if (strcmp(pt, fixture_msg) != 0) {
        fprintf(stderr, "fixture plaintext mismatch: got '%s' exp '%s'\n", pt, fixture_msg);
        free(pt);
        return 1;
    }
    free(pt);
    return 0;
}
