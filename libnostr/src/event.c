#include "event.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int hex2bin(unsigned char *bin, const char *hex, size_t bin_len);

// Escape special characters in the string according to RFC8259
char *escape_string(const char *str) {
    size_t len = strlen(str);
    size_t capacity = len * 2;            // Start with enough space
    char *escaped = malloc(capacity + 1); // Allocate buffer
    if (!escaped)
        return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (j + 6 > capacity) { // Make sure we have enough space
            capacity *= 2;
            escaped = realloc(escaped, capacity + 1);
            if (!escaped)
                return NULL;
        }

        switch (str[i]) {
        case '\"':
            escaped[j++] = '\\';
            escaped[j++] = '\"';
            break;
        case '\\':
            escaped[j++] = '\\';
            escaped[j++] = '\\';
            break;
        case '\b':
            escaped[j++] = '\\';
            escaped[j++] = 'b';
            break;
        case '\f':
            escaped[j++] = '\\';
            escaped[j++] = 'f';
            break;
        case '\n':
            escaped[j++] = '\\';
            escaped[j++] = 'n';
            break;
        case '\r':
            escaped[j++] = '\\';
            escaped[j++] = 'r';
            break;
        case '\t':
            escaped[j++] = '\\';
            escaped[j++] = 't';
            break;
        default:
            escaped[j++] = str[i]; // No escape needed
            break;
        }
    }

    escaped[j] = '\0'; // Null-terminate the string
    return escaped;
}

// Event-related functions
NostrEvent *create_event() {
    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event)
        return NULL;

    event->id = NULL;
    event->pubkey = NULL;
    event->created_at = 0;
    event->kind = 0;
    event->tags = create_tags(0);
    event->tags->data = NULL;
    event->tags->count = 0;
    event->content = NULL;
    event->sig = NULL;

    return event;
}

void free_event(NostrEvent *event) {
    if (event) {
        free(event->id);
        free(event->pubkey);
        free_tags(event->tags);
        free(event->content);
        free(event->sig);
        free(event);
    }
}

char *event_serialize(NostrEvent *event) {
    if (!event || !event->pubkey || !event->content)
        return NULL;

    // Initial capacity for the serialized string
    size_t capacity = 1024;
    char *result = malloc(capacity);
    if (!result)
        return NULL;

    // Create the header part: [0,"PubKey",CreatedAt,Kind,
    size_t needed = snprintf(
        result, capacity,
        "[0,\"%s\",%ld,%d,",
        event->pubkey, event->created_at, event->kind);
    if (needed >= capacity) {
        capacity = needed + 1;
        result = realloc(result, capacity);
        snprintf(result, capacity, "[0,\"%s\",%ld,%d,", event->pubkey, event->created_at, event->kind);
    }

    // Serialize tags
    char *tags_json = tags_marshal_to_json(event->tags);
    if (!tags_json) {
        free(result);
        return NULL;
    }

    // Reallocate buffer for tags
    needed = strlen(result) + strlen(tags_json) + 2;
    if (needed > capacity) {
        capacity = needed;
        result = realloc(result, capacity);
    }
    strcat(result, tags_json); // Append the serialized tags
    strcat(result, ",");

    // Free the tags JSON string after use
    free(tags_json);

    // Escape and append content
    char *escaped_content = escape_string(event->content);
    if (!escaped_content) {
        free(result);
        return NULL;
    }

    // Reallocate buffer for content
    needed = strlen(result) + strlen(escaped_content) + 2;
    if (needed > capacity) {
        capacity = needed;
        result = realloc(result, capacity);
    }
    strcat(result, "\"");
    strcat(result, escaped_content); // Append escaped content
    strcat(result, "\"]");           // Close the JSON array

    // Free the escaped content string
    free(escaped_content);

    return result; // Return the final serialized JSON string
}

char *event_get_id(NostrEvent *event) {
    if (!event)
        return NULL;

    // Serialize the event
    char *serialized = event_serialize(event);
    if (!serialized)
        return NULL;

    // Hash the serialized event using SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    // Allocate memory for the event ID (64 hex characters + null terminator)
    char *id = (char *)malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    if (!id)
        return NULL; // Check for allocation failure

    // Convert the binary hash to a hex string
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(&id[i * 2], "%02x", hash[i]);
    }
    id[SHA256_DIGEST_LENGTH * 2] = '\0'; // Null terminate the string

    return id;
}

bool event_check_signature(NostrEvent *event) {
    if (!event) {
        fprintf(stderr, "Event is null\n");
        return false;
    }

    // Decode public key from hex
    unsigned char pubkey_bin[32]; // 32 bytes for schnorr pubkey
    if (!hex2bin(pubkey_bin, event->pubkey, sizeof(pubkey_bin))) {
        fprintf(stderr, "Invalid public key hex\n");
        return false;
    }

    // Decode signature from hex
    unsigned char sig_bin[64]; // 64 bytes for schnorr signature
    if (!hex2bin(sig_bin, event->sig, sizeof(sig_bin))) {
        fprintf(stderr, "Invalid signature hex\n");
        return false;
    }

    // Create secp256k1 context
    secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        fprintf(stderr, "Failed to create secp256k1 context\n");
        return false;
    }

    // Parse the public key using secp256k1_xonly_pubkey_parse (for Schnorr signatures)
    secp256k1_xonly_pubkey pubkey;
    if (!secp256k1_xonly_pubkey_parse(ctx, &pubkey, pubkey_bin)) {
        fprintf(stderr, "Failed to parse public key\n");
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Serialize the event content and create a hash
    char *serialized = event_serialize(event);
    if (!serialized) {
        fprintf(stderr, "Failed to serialize event\n");
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Calculate the SHA-256 hash of the serialized event
    unsigned char hash[32];
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    /*** Verification ***/

    // Verify the signature using secp256k1_schnorrsig_verify
    int verified = secp256k1_schnorrsig_verify(ctx, sig_bin, hash, 32, &pubkey);

    // Clean up
    secp256k1_context_destroy(ctx);

    if (verified) {
        return true;
    } else {
        fprintf(stderr, "Signature verification failed\n");
        return false;
    }
}

// Sign the event
int event_sign(NostrEvent *event, const char *private_key) {
    if (!event || !private_key)
        return -1;

    unsigned char hash[32]; // Schnorr requires a 32-byte hash
    char *serialized = event_serialize(event);
    if (!serialized)
        return -1;

    // Hash the serialized event content
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    secp256k1_context *ctx;
    secp256k1_keypair keypair;
    unsigned char privkey_bin[32];    // 32-byte private key (256-bit)
    unsigned char sig_bin[64];        // 64-byte Schnorr signature
    unsigned char auxiliary_rand[32]; // Auxiliary randomness
    int return_val = -1;

    // Convert the private key from hex to binary
    if (!hex2bin(privkey_bin, private_key, sizeof(privkey_bin))) {
        return -1;
    }

    // Create secp256k1 context for signing
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);

    // Verify that the private key is valid
    if (!secp256k1_ec_seckey_verify(ctx, privkey_bin)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Create a keypair from the private key
    if (!secp256k1_keypair_create(ctx, &keypair, privkey_bin)) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Generate 32 bytes of randomness for Schnorr signing
    if (RAND_bytes(auxiliary_rand, sizeof(auxiliary_rand)) != 1) {
        fprintf(stderr, "Failed to generate random bytes\n");
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Sign the hash using Schnorr signatures (BIP-340)
    if (secp256k1_schnorrsig_sign32(ctx, sig_bin, hash, &keypair, auxiliary_rand) != 1) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Convert the signature to a hex string and store it in the event
    event->sig = (char *)malloc(64 * 2 + 1);
    if (!event->sig) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    for (size_t i = 0; i < 64; i++) {
        sprintf(&event->sig[i * 2], "%02x", sig_bin[i]);
    }
    event->sig[64 * 2] = '\0';

    // Generate and set the event ID
    event->id = event_get_id(event);

    return_val = 0;

cleanup:
    secp256k1_context_destroy(ctx);
    return return_val;
}