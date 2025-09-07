#include <assert.h>
#include <ctype.h>
#if defined(__has_include)
#  if __has_include(<openssl/rand.h>)
#    include <openssl/rand.h>
#    define HAVE_OPENSSL_RAND 1
#  endif
#endif
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "nostr-utils.h"
#include "nostr-keys.h"
#include "../include/secure_buf.h"

static int rand_bytes_portable(unsigned char *out, size_t n) {
#if defined(HAVE_OPENSSL_RAND)
    return RAND_bytes(out, (int)n) == 1 ? 0 : -1;
#elif defined(__APPLE__)
    extern void arc4random_buf(void *buf, size_t nbytes);
    arc4random_buf(out, n);
    return 0;
#else
    /* Fallback to /dev/urandom */
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return -1;
    size_t r = fread(out, 1, n, f);
    fclose(f);
    return r == n ? 0 : -1;
#endif
}

// Generate a private key using libsecp256k1
char *nostr_key_generate_private(void) {
    secp256k1_context *ctx;
    nostr_secure_buf sb = secure_alloc(32); // Private key (32 bytes for secp256k1)
    unsigned char *privkey = (unsigned char*)sb.ptr;
    char *privkey_hex = malloc(65); // Hex representation of private key (64 chars + null terminator)

    if (!privkey_hex) {
        fprintf(stderr, "Memory allocation failed\n");
        secure_free(&sb);
        return NULL;
    }

    // Create a secp256k1 context for key generation
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        fprintf(stderr, "Failed to create secp256k1 context\n");
        free(privkey_hex);
        secure_free(&sb);
        return NULL;
    }

    // Generate a valid private key
    while (1) {
        // Generate 32 random bytes
        if (rand_bytes_portable(privkey, 32) != 0) {
            fprintf(stderr, "Failed to generate random bytes\n");
            secp256k1_context_destroy(ctx);
            free(privkey_hex);
            secure_free(&sb);
            return NULL;
        }

        // Verify that the private key is valid (must be less than curve order)
        if (secp256k1_ec_seckey_verify(ctx, privkey)) {
            break; // Valid private key found
        }
    }

    // Convert private key to hex string
    for (size_t i = 0; i < 32; i++) {
        snprintf(&privkey_hex[i * 2], 3, "%02x", privkey[i]);
    }
    privkey_hex[64] = '\0'; // Null-terminate the string

    // Clean up secp256k1 context
    secp256k1_context_destroy(ctx);
    secure_free(&sb);

    return privkey_hex;
}

// Get the public key from a private key using libsecp256k1
char *nostr_key_get_public(const char *sk) {
    secp256k1_context *ctx;
    nostr_secure_buf sb = secure_alloc(32); // Private key (32 bytes)
    unsigned char *privkey = (unsigned char*)sb.ptr;
    secp256k1_pubkey pubkey;
    secp256k1_xonly_pubkey xonly_pubkey;
    unsigned char pubkey_bin[32];  // Compressed public key (32 bytes)
    char *pubkey_hex = malloc(65); // Hex representation of public key (64 chars + null terminator)

    if (!pubkey_hex) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    // Convert hex-encoded private key to binary
    if (!nostr_hex2bin(privkey, sk, 32)) {
        fprintf(stderr, "Invalid private key hex\n");
        free(pubkey_hex);
        secure_free(&sb);
        return NULL;
    }

    // Create a secp256k1 context
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        fprintf(stderr, "Failed to create secp256k1 context\n");
        free(pubkey_hex);
        secure_free(&sb);
        return NULL;
    }

    // Create a public key from the private key
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privkey)) {
        fprintf(stderr, "Failed to create public key from private key\n");
        secp256k1_context_destroy(ctx);
        free(pubkey_hex);
        secure_free(&sb);
        return NULL;
    }

    // Convert the public key to an x-only public key
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_pubkey, NULL, &pubkey)) {
        fprintf(stderr, "Failed to convert to x-only public key\n");
        secp256k1_context_destroy(ctx);
        free(pubkey_hex);
        secure_free(&sb);
        return NULL;
    }

    // Serialize the x-only public key (32 bytes)
    secp256k1_xonly_pubkey_serialize(ctx, pubkey_bin, &xonly_pubkey);

    // Convert the public key to hex string
    for (size_t i = 0; i < 32; i++) {
        snprintf(&pubkey_hex[i * 2], 3, "%02x", pubkey_bin[i]);
    }
    pubkey_hex[64] = '\0'; // Null-terminate the string

    // Clean up secp256k1 context
    secp256k1_context_destroy(ctx);
    secure_free(&sb);

    return pubkey_hex;
}

// Validate if a public key is a valid 32-byte x-only hex string (64 chars)
bool nostr_key_is_valid_public_hex(const char *pk) {
    if (!pk) {
        return false;
    }

    size_t len = strlen(pk);
    if (len != 64) { // 32 bytes x-only in hex representation for Nostr
        return false;
    }

    // Check if all characters are valid hexadecimal digits
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit(pk[i])) {
            return false;
        }
    }

    return true;
}

// Validate if a public key is valid using libsecp256k1
bool nostr_key_is_valid_public(const char *pk) {
    secp256k1_context *ctx;
    secp256k1_pubkey pubkey;
    unsigned char pub_key_bin[33]; // Compressed public key (33 bytes)
    int return_val;

    // First check if the provided string is a valid public key hex
    if (!nostr_key_is_valid_public_hex(pk)) {
        return false;
    }

    // Convert the hex-encoded public key to binary form
    if (!nostr_hex2bin(pub_key_bin, pk, sizeof(pub_key_bin))) {
        return false;
    }

    // Create secp256k1 context for public key verification
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

    // Parse the compressed public key
    return_val = secp256k1_ec_pubkey_parse(ctx, &pubkey, pub_key_bin, sizeof(pub_key_bin));
    if (return_val == 0) {
        // Invalid public key
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Public key parsing succeeded, so it's valid
    secp256k1_context_destroy(ctx);
    return true;
}
