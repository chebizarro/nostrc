#include <nostr/nip04.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

int main(void) {
    const char *sender_sk_hex = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *sender_pk_hex = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798"; // G
    const char *receiver_sk_hex = "0000000000000000000000000000000000000000000000000000000000000002";
    const char *receiver_pk_hex = "02C6047F9441ED7D6D3045406E95C07CD85C778E4B8CEF3CA7ABAC09B95C709EE5"; // 2*G

    const char *msg = "hi nip04 utf8 ✓";

    char *content = NULL; char *err = NULL; char *pt = NULL;
    int rc = nostr_nip04_encrypt(msg, receiver_pk_hex, sender_sk_hex, &content, &err);
    if (rc != 0) {
        fprintf(stderr, "encrypt failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    assert(content != NULL);

    rc = nostr_nip04_decrypt(content, sender_pk_hex, receiver_sk_hex, &pt, &err);
    if (rc != 0) {
        fprintf(stderr, "decrypt failed: %s\n", err ? err : "unknown");
        free(err);
        free(content);
        return 1;
    }
    assert(pt != NULL);
    assert(strcmp(pt, msg) == 0);

    free(content);
    free(pt);
    return 0;
}
