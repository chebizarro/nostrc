#include <nostr/nip04.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nostr-utils.h"
#include <secure_buf.h>

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

    /* Note: nostr_nip04_shared_secret_hex() is deprecated; avoid exposing raw ECDH secrets. */
    // Encrypt using secure API with sender secret in secure memory
    nostr_secure_buf sb_sender = secure_alloc(32);
    if (!sb_sender.ptr || !nostr_hex2bin((unsigned char*)sb_sender.ptr, sender_sk_hex, 32)) {
        fprintf(stderr, "secure alloc/hex2bin failed\n");
        if (sb_sender.ptr) secure_free(&sb_sender);
        return 1;
    }
    if (nostr_nip04_encrypt_secure(msg, receiver_pk_hex, &sb_sender, &content, &err) != 0) {
        fprintf(stderr, "encrypt error: %s\n", err ? err : "unknown");
        free(err);
        secure_free(&sb_sender);
        return 1;
    }
    secure_free(&sb_sender);
    printf("content: %s\n", content);

    char *pt = NULL;
    // Decrypt using secure API with receiver secret in secure memory
    nostr_secure_buf sb_receiver = secure_alloc(32);
    if (!sb_receiver.ptr || !nostr_hex2bin((unsigned char*)sb_receiver.ptr, receiver_sk_hex, 32)) {
        fprintf(stderr, "secure alloc/hex2bin failed\n");
        if (sb_receiver.ptr) secure_free(&sb_receiver);
        free(content);
        return 1;
    }
    if (nostr_nip04_decrypt_secure(content, sender_pk_hex, &sb_receiver, &pt, &err) != 0) {
        fprintf(stderr, "decrypt error: %s\n", err ? err : "unknown");
        free(err);
        free(content);
        secure_free(&sb_receiver);
        return 1;
    }
    secure_free(&sb_receiver);
    printf("plaintext: %s\n", pt);

    free(content);
    free(pt);
    // Wipe stack copies of secrets
    volatile unsigned char *p1 = sk1; for (size_t i=0;i<sizeof sk1;i++) p1[i]=0;
    volatile unsigned char *p2 = sk2; for (size_t i=0;i<sizeof sk2;i++) p2[i]=0;
    return 0;
}
