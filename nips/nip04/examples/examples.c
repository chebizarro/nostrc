#include <nostr/nip04.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostr-utils.h"

int main() {
    // Example placeholder keys (replace with real hex keys for an actual run)
    const char *sender_sk_hex = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *receiver_sk_hex = "0000000000000000000000000000000000000000000000000000000000000002";
    char sender_pk_hex[131];
    char receiver_pk_hex[131];

    // Derive compressed public keys from secret keys
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    unsigned char sk1[32], sk2[32];
    nostr_hex2bin(sk1, sender_sk_hex, sizeof(sk1));
    nostr_hex2bin(sk2, receiver_sk_hex, sizeof(sk2));
    secp256k1_pubkey pk1, pk2;
    if (!secp256k1_ec_pubkey_create(ctx, &pk1, sk1) || !secp256k1_ec_pubkey_create(ctx, &pk2, sk2)) {
        fprintf(stderr, "failed to create pubkeys\n");
        secp256k1_context_destroy(ctx);
        return 1;
    }
    unsigned char out33[33]; size_t outlen = 33;
    secp256k1_ec_pubkey_serialize(ctx, out33, &outlen, &pk1, SECP256K1_EC_COMPRESSED);
    for (size_t i = 0; i < outlen; i++) sprintf(sender_pk_hex + 2*i, "%02x", out33[i]);
    outlen = 33;
    secp256k1_ec_pubkey_serialize(ctx, out33, &outlen, &pk2, SECP256K1_EC_COMPRESSED);
    for (size_t i = 0; i < outlen; i++) sprintf(receiver_pk_hex + 2*i, "%02x", out33[i]);
    sender_pk_hex[66] = '\0';
    receiver_pk_hex[66] = '\0';
    secp256k1_context_destroy(ctx);
    printf("sender_pk: %s\n", sender_pk_hex);
    printf("receiver_pk: %s\n", receiver_pk_hex);

    const char *msg = "Hello, NIP-04!";
    char *content = NULL; char *err = NULL;

    char *xhex = NULL; char *e2 = NULL;
    if (nostr_nip04_shared_secret_hex(receiver_pk_hex, sender_sk_hex, &xhex, &e2) != 0) {
        fprintf(stderr, "shared secret error: %s\n", e2 ? e2 : "unknown");
        free(e2);
    } else {
        printf("shared_x: %s\n", xhex);
        free(xhex);
    }
    if (nostr_nip04_encrypt(msg, receiver_pk_hex, sender_sk_hex, &content, &err) != 0) {
        fprintf(stderr, "encrypt error: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    printf("content: %s\n", content);

    char *pt = NULL;
    if (nostr_nip04_decrypt(content, sender_pk_hex, receiver_sk_hex, &pt, &err) != 0) {
        fprintf(stderr, "decrypt error: %s\n", err ? err : "unknown");
        free(err);
        free(content);
        return 1;
    }
    printf("plaintext: %s\n", pt);

    free(content);
    free(pt);
    return 0;
}
