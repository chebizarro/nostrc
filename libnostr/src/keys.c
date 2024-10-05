#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <ctype.h>
#include <assert.h>
#include <openssl/rand.h>


// Convert hex string to binary
int hex2bin(unsigned char *bin, const char *hex, size_t bin_len) {
    if (strlen(hex) != bin_len * 2) return 0;
    for (size_t i = 0; i < bin_len; i++) {
        sscanf(hex + 2 * i, "%2hhx", &bin[i]);
    }
    return 1;
}

// Generate a private key using libsecp256k1
char *generate_private_key() {
    secp256k1_context *ctx;
    unsigned char privkey[32];  // Private key (32 bytes for secp256k1)
    char *privkey_hex = malloc(65); // Hex representation of private key (64 chars + null terminator)

    if (!privkey_hex) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    // Create a secp256k1 context for key generation
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        fprintf(stderr, "Failed to create secp256k1 context\n");
        free(privkey_hex);
        return NULL;
    }

    // Generate a valid private key
    while (1) {
        // Generate 32 random bytes
        if (RAND_bytes(privkey, sizeof(privkey)) != 1) {
            fprintf(stderr, "Failed to generate random bytes\n");
            secp256k1_context_destroy(ctx);
            free(privkey_hex);
            return NULL;
        }

        // Verify that the private key is valid (must be less than curve order)
        if (secp256k1_ec_seckey_verify(ctx, privkey)) {
            break;  // Valid private key found
        }
    }

    // Convert private key to hex string
    for (size_t i = 0; i < 32; i++) {
        sprintf(&privkey_hex[i * 2], "%02x", privkey[i]);
    }
    privkey_hex[64] = '\0';  // Null-terminate the string

    // Clean up secp256k1 context
    secp256k1_context_destroy(ctx);

    return privkey_hex;
}

// Get the public key from a private key using libsecp256k1
char *get_public_key(const char *sk) {
    secp256k1_context *ctx;
    unsigned char privkey[32];  // Private key (32 bytes)
    secp256k1_pubkey pubkey;
    secp256k1_xonly_pubkey xonly_pubkey;
    unsigned char pubkey_bin[32];  // Compressed public key (32 bytes)
    char *pubkey_hex = malloc(65); // Hex representation of public key (64 chars + null terminator)

    if (!pubkey_hex) {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    // Convert hex-encoded private key to binary
    if (!hex2bin(privkey, sk, 32)) {
        fprintf(stderr, "Invalid private key hex\n");
        free(pubkey_hex);
        return NULL;
    }

    // Create a secp256k1 context
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        fprintf(stderr, "Failed to create secp256k1 context\n");
        free(pubkey_hex);
        return NULL;
    }

    // Create a public key from the private key
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privkey)) {
        fprintf(stderr, "Failed to create public key from private key\n");
        secp256k1_context_destroy(ctx);
        free(pubkey_hex);
        return NULL;
    }

    // Convert the public key to an x-only public key
    if (!secp256k1_xonly_pubkey_from_pubkey(ctx, &xonly_pubkey, NULL, &pubkey)) {
        fprintf(stderr, "Failed to convert to x-only public key\n");
        secp256k1_context_destroy(ctx);
        free(pubkey_hex);
        return NULL;
    }

    // Serialize the x-only public key (32 bytes)
    secp256k1_xonly_pubkey_serialize(ctx, pubkey_bin, &xonly_pubkey);

    // Convert the public key to hex string
    for (size_t i = 0; i < 32; i++) {
        sprintf(&pubkey_hex[i * 2], "%02x", pubkey_bin[i]);
    }
    pubkey_hex[64] = '\0';  // Null-terminate the string

    // Clean up secp256k1 context
    secp256k1_context_destroy(ctx);

    return pubkey_hex;

}

// Validate if a public key is a valid 32-byte hex string
bool is_valid_public_key_hex(const char *pk) {
    if (!pk) {
        return false;
    }

    size_t len = strlen(pk);
    if (len != 66) { // 33 bytes in hex representation for compressed key
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
bool is_valid_public_key(const char *pk) {
    secp256k1_context *ctx;
    secp256k1_pubkey pubkey;
    unsigned char pub_key_bin[33]; // Compressed public key (33 bytes)
    int return_val;

    // First check if the provided string is a valid public key hex
    if (!is_valid_public_key_hex(pk)) {
        return false;
    }

    // Convert the hex-encoded public key to binary form
    if (!hex2bin(pub_key_bin, pk, sizeof(pub_key_bin))) {
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
