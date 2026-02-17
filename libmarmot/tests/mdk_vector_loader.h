/*
 * MDK Test Vector Loader
 * 
 * Simple JSON parser for MDK test vectors without external dependencies.
 * Focuses on key-schedule and crypto-basics vectors for cross-validation.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MDK_VECTOR_LOADER_H
#define MDK_VECTOR_LOADER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum vector counts */
#define MAX_EPOCHS 10
#define MAX_CRYPTO_TESTS 20

/* Key schedule epoch data */
typedef struct {
    uint8_t commit_secret[32];
    uint8_t confirmation_key[32];
    uint8_t encryption_secret[32];
    uint8_t exporter_secret[32];
    uint8_t init_secret[32];
    uint8_t joiner_secret[32];
    uint8_t membership_key[32];
    uint8_t sender_data_secret[32];
    uint8_t welcome_secret[32];
    uint8_t epoch_authenticator[32];
    uint8_t resumption_psk[32];
    uint8_t external_secret[32];
    uint8_t external_pub[32];
    
    /* Exporter test - per MLS spec, label is a string */
    char exporter_label[128];
    uint8_t exporter_context[32];
    uint8_t exporter_secret_out[32];
    uint32_t exporter_length;
    
    /* Group context for derivation */
    uint8_t group_context[512];
    size_t group_context_len;
    
    /* Tree hash for this epoch */
    uint8_t tree_hash[32];
    uint8_t confirmed_transcript_hash[32];
} MdkEpochVector;

/* Key schedule test case */
typedef struct {
    uint32_t cipher_suite;
    uint8_t group_id[32];
    size_t group_id_len;
    uint8_t initial_init_secret[32];
    size_t epoch_count;
    MdkEpochVector epochs[MAX_EPOCHS];
} MdkKeyScheduleVector;

/* Crypto basics test */
typedef struct {
    uint32_t cipher_suite;
    
    /* ExpandWithLabel test */
    uint8_t expand_secret[32];
    uint8_t expand_context[32];
    uint8_t expand_out[32];
    char expand_label[64];
    uint32_t expand_length;
    
    /* DeriveSecret test */
    uint8_t derive_secret[32];
    uint8_t derive_out[32];
    char derive_label[64];
} MdkCryptoBasicsVector;

/* Load key-schedule vectors from JSON file */
int mdk_load_key_schedule_vectors(const char *path, 
                                   MdkKeyScheduleVector *vectors,
                                   size_t *count,
                                   size_t max_count);

/* Load crypto-basics vectors from JSON file */
int mdk_load_crypto_basics_vectors(const char *path,
                                    MdkCryptoBasicsVector *vectors,
                                    size_t *count,
                                    size_t max_count);

/* Helper: decode hex string to bytes */
bool mdk_hex_decode(uint8_t *out, const char *hex, size_t out_len);

/* Helper: find JSON string value by key */
const char *mdk_json_find_string(const char *json, const char *key);

/* Helper: find JSON number value by key */
int mdk_json_find_number(const char *json, const char *key, uint32_t *out);

#endif /* MDK_VECTOR_LOADER_H */
