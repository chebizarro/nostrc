#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <secp256k1.h>
#include <ctype.h>
#include <assert.h>


// Simple random filler for testing (you can use a secure RNG in production)
int fill_random(unsigned char *buf, size_t len) {
    FILE *fp = fopen("/dev/urandom", "rb");
    if (!fp) return 0;
    fread(buf, 1, len, fp);
    fclose(fp);
    return 1;
}

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
    unsigned char seckey[32]; // 32 bytes for the secp256k1 private key
    int return_val;
    secp256k1_context *ctx;

    // Create secp256k1 context
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Generate a valid private key
    while (1) {
        if (!fill_random(seckey, sizeof(seckey))) {
            printf("Failed to generate randomness\n");
            secp256k1_context_destroy(ctx);
            return NULL;
        }

        // Verify that the secret key is valid
        if (secp256k1_ec_seckey_verify(ctx, seckey)) {
            break; // Valid private key generated
        }
    }

    // Convert the private key to hex format
    char *priv_key_hex = malloc(65); // 32 bytes * 2 + null terminator
    for (size_t i = 0; i < sizeof(seckey); i++) {
        sprintf(priv_key_hex + (i * 2), "%02x", seckey[i]);
    }

    // Cleanup
    secp256k1_context_destroy(ctx);

    return priv_key_hex;
}

// Get the public key from a private key using libsecp256k1
char *get_public_key(const char *sk) {
    secp256k1_context *ctx;
    secp256k1_pubkey pubkey;
    unsigned char seckey[32]; // 32 bytes for secp256k1 private key
    unsigned char pub_key_bin[33]; // Compressed public key format (33 bytes)
    size_t pub_key_len = 33;
    char *pub_key_hex = malloc(67); // 33 bytes * 2 + null terminator
    int return_val;

    // Convert the private key hex string to binary
    if (!hex2bin(seckey, sk, sizeof(seckey))) {
        free(pub_key_hex);
        return NULL;
    }

    // Create secp256k1 context
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Generate the public key from the private key
    return_val = secp256k1_ec_pubkey_create(ctx, &pubkey, seckey);
    if (!return_val) {
        printf("Failed to create public key\n");
        secp256k1_context_destroy(ctx);
        free(pub_key_hex);
        return NULL;
    }

    // Serialize the public key to compressed format
    return_val = secp256k1_ec_pubkey_serialize(ctx, pub_key_bin, &pub_key_len, &pubkey, SECP256K1_EC_COMPRESSED);
    assert(return_val);
    assert(pub_key_len == 33); // Compressed key should always be 33 bytes

    // Convert the public key to hex format
    for (size_t i = 0; i < pub_key_len; i++) {
        sprintf(pub_key_hex + (i * 2), "%02x", pub_key_bin[i]);
    }

    // Cleanup
    secp256k1_context_destroy(ctx);

    return pub_key_hex;
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
