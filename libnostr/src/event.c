#include "nostr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <secp256k1.h>
#include <openssl/sha.h>

extern int hex2bin(unsigned char *bin, const char *hex, size_t bin_len);

// Event-related functions
NostrEvent *create_event() {
    NostrEvent *event = (NostrEvent *)malloc(sizeof(NostrEvent));
    if (!event) return NULL;

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
    if (!event) return NULL;

    if (json_interface && json_interface->serialize) {
        return json_interface->serialize(event);
    }
    return NULL;
}

char *event_get_id(NostrEvent *event) {
    if (!event) return NULL;

    // Serialize the event
    char *serialized = event_serialize(event);
    if (!serialized) return NULL;

    // Hash the serialized event using SHA-256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    // Allocate memory for the event ID (64 hex characters + null terminator)
    char *id = (char *)malloc(SHA256_DIGEST_LENGTH * 2 + 1);
    if (!id) return NULL;  // Check for allocation failure

    // Convert the binary hash to a hex string
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(&id[i * 2], "%02x", hash[i]);
    }
    id[SHA256_DIGEST_LENGTH * 2] = '\0';  // Null terminate the string

    return id;
}

// Check if the signature of a Nostr event is valid
bool event_check_signature(NostrEvent *event) {
    if (!event) return false;

    secp256k1_context *ctx;
    secp256k1_pubkey pubkey;
    secp256k1_ecdsa_signature sig;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned char pubkey_bin[33];  // Compressed public key (33 bytes)
    unsigned char sig_bin[64];     // ECDSA signature (64 bytes)
    int return_val;

    // Serialize the event to obtain its hash
    char *serialized = event_serialize(event);
    if (!serialized) return false;
    SHA256((unsigned char *)serialized, strlen(serialized), hash);  // Hash the serialized event
    free(serialized);

    // Convert the public key from hex to binary
    if (!hex2bin(pubkey_bin, event->pubkey, sizeof(pubkey_bin))) {
        return false;
    }

    // Convert the signature from hex to binary
    if (!hex2bin(sig_bin, event->sig, sizeof(sig_bin))) {
        return false;
    }

    // Create secp256k1 context for signature verification
    ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

    // Parse the public key
    return_val = secp256k1_ec_pubkey_parse(ctx, &pubkey, pubkey_bin, sizeof(pubkey_bin));
    if (return_val == 0) {
        // Failed to parse public key
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Parse the signature
    return_val = secp256k1_ecdsa_signature_parse_compact(ctx, &sig, sig_bin);
    if (return_val == 0) {
        // Failed to parse signature
        secp256k1_context_destroy(ctx);
        return false;
    }

    // Verify the signature
    return_val = secp256k1_ecdsa_verify(ctx, &sig, hash, &pubkey);
    
    // Clean up secp256k1 context
    secp256k1_context_destroy(ctx);

    return return_val == 1;  // Return true if signature is valid
}

// Sign the event
int event_sign(NostrEvent *event, const char *private_key) {
    if (!event || !private_key) return -1;

    unsigned char hash[SHA256_DIGEST_LENGTH];
    char *serialized = event_serialize(event);
    if (!serialized) return -1;

    // Hash the serialized event content
    SHA256((unsigned char *)serialized, strlen(serialized), hash);
    free(serialized);

    secp256k1_context *ctx;
    secp256k1_ecdsa_signature sig;
    unsigned char privkey_bin[32]; // 32-byte private key (256-bit)
    unsigned char sig_bin[64];     // 64-byte signature
    size_t sig_len = sizeof(sig_bin);
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

    // Sign the hash of the event using the private key
    if (secp256k1_ecdsa_sign(ctx, &sig, hash, privkey_bin, NULL, NULL) != 1) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Serialize the signature to a compact form
    if (secp256k1_ecdsa_signature_serialize_compact(ctx, sig_bin, &sig) != 1) {
        secp256k1_context_destroy(ctx);
        return -1;
    }

    // Convert the signature to a hex string and store it in the event
    event->sig = (char *)malloc(sig_len * 2 + 1);
    if (!event->sig) {
        secp256k1_context_destroy(ctx);
        return -1;
    }
    for (size_t i = 0; i < sig_len; i++) {
        sprintf(&event->sig[i * 2], "%02x", sig_bin[i]);
    }
    event->sig[sig_len * 2] = '\0';

    // Generate and set the event ID (you need to define event_get_id logic)
    event->id = event_get_id(event);

    return_val = 0;

cleanup:
    secp256k1_context_destroy(ctx);
    return return_val;
}