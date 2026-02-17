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

/* Tree math test */
typedef struct {
    uint32_t n_leaves;
    uint32_t n_nodes;
    uint32_t root;
    uint32_t *left;
    uint32_t *right;
    uint32_t *parent;
    uint32_t *sibling;
    size_t array_size;
} MdkTreeMathVector;

/* Messages test */
typedef struct {
    uint8_t mls_welcome[4096];
    size_t mls_welcome_len;
    uint8_t mls_group_info[4096];
    size_t mls_group_info_len;
    uint8_t mls_key_package[4096];
    size_t mls_key_package_len;
} MdkMessagesVector;

/* Deserialization test */
typedef struct {
    uint8_t vlbytes_header[16];
    size_t vlbytes_header_len;
    uint32_t length;
} MdkDeserializationVector;

/* PSK secret test */
typedef struct {
    uint32_t cipher_suite;
    struct {
        uint8_t psk_id[256];
        size_t psk_id_len;
        uint8_t psk[64];
        uint8_t psk_nonce[64];
    } psks[16];
    size_t psk_count;
    uint8_t psk_secret[32];
} MdkPskSecretVector;

/* Secret tree test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t encryption_secret[32];
    uint8_t sender_data_secret[32];
    uint8_t sender_data_ciphertext[256];
    size_t sender_data_ciphertext_len;
    uint8_t sender_data_key[32];
    uint8_t sender_data_nonce[32];
    size_t n_leaves;
} MdkSecretTreeVector;

/* Transcript hashes test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t confirmation_key[32];
    uint8_t authenticated_content[2048];
    size_t authenticated_content_len;
    uint8_t interim_transcript_hash_before[32];
    uint8_t confirmed_transcript_hash_after[32];
    uint8_t interim_transcript_hash_after[32];
} MdkTranscriptHashesVector;

/* Welcome test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t init_priv[128];
    size_t init_priv_len;
    uint8_t signer_pub[128];
    size_t signer_pub_len;
    uint8_t key_package[2048];
    size_t key_package_len;
    uint8_t welcome[4096];
    size_t welcome_len;
} MdkWelcomeVector;

/* Message protection test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t group_id[32];
    size_t group_id_len;
    uint64_t epoch;
    uint8_t tree_hash[32];
    uint8_t confirmed_transcript_hash[32];
    uint8_t signature_priv[128];
    size_t signature_priv_len;
    uint8_t signature_pub[128];
    size_t signature_pub_len;
    uint8_t encryption_secret[32];
    uint8_t sender_data_secret[32];
    uint8_t membership_key[32];
    uint8_t proposal[1024];
    size_t proposal_len;
    uint8_t proposal_pub[2048];
    size_t proposal_pub_len;
    uint8_t proposal_priv[2048];
    size_t proposal_priv_len;
} MdkMessageProtectionVector;

/* Tree operations test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t tree_before[8192];
    size_t tree_before_len;
    uint8_t proposal[2048];
    size_t proposal_len;
    uint32_t proposal_sender;
    uint8_t tree_hash_before[32];
    uint8_t tree_after[8192];
    size_t tree_after_len;
    uint8_t tree_hash_after[32];
} MdkTreeOperationsVector;

/* Tree validation test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t tree[16384];
    size_t tree_len;
    uint8_t group_id[32];
    size_t group_id_len;
} MdkTreeValidationVector;

/* TreeKEM test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t group_id[32];
    size_t group_id_len;
    uint64_t epoch;
    uint8_t confirmed_transcript_hash[32];
    uint8_t ratchet_tree[16384];
    size_t ratchet_tree_len;
} MdkTreeKEMVector;

/* Passive client test */
typedef struct {
    uint32_t cipher_suite;
    uint8_t key_package[2048];
    size_t key_package_len;
    uint8_t signature_priv[128];
    size_t signature_priv_len;
    uint8_t encryption_priv[128];
    size_t encryption_priv_len;
    uint8_t init_priv[128];
    size_t init_priv_len;
    uint8_t welcome[4096];
    size_t welcome_len;
    uint8_t ratchet_tree[16384];
    size_t ratchet_tree_len;
    uint8_t initial_epoch_authenticator[32];
    size_t epoch_count;
} MdkPassiveClientVector;

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

/* Load tree math vectors from JSON file */
int mdk_load_tree_math_vectors(const char *path, 
                                MdkTreeMathVector *vectors,
                                size_t *count,
                                size_t max_count);

/* Load messages vectors from JSON file */
int mdk_load_messages_vectors(const char *path,
                               MdkMessagesVector *vectors,
                               size_t *count,
                               size_t max_count);

/* Load deserialization vectors from JSON file */
int mdk_load_deserialization_vectors(const char *path,
                                      MdkDeserializationVector *vectors,
                                      size_t *count,
                                      size_t max_count);

/* Load PSK secret vectors from JSON file */
int mdk_load_psk_secret_vectors(const char *path,
                                 MdkPskSecretVector *vectors,
                                 size_t *count,
                                 size_t max_count);

/* Load secret tree vectors from JSON file */
int mdk_load_secret_tree_vectors(const char *path,
                                  MdkSecretTreeVector *vectors,
                                  size_t *count,
                                  size_t max_count);

/* Load transcript hashes vectors from JSON file */
int mdk_load_transcript_hashes_vectors(const char *path,
                                        MdkTranscriptHashesVector *vectors,
                                        size_t *count,
                                        size_t max_count);

/* Load welcome vectors from JSON file */
int mdk_load_welcome_vectors(const char *path,
                              MdkWelcomeVector *vectors,
                              size_t *count,
                              size_t max_count);

/* Load message protection vectors from JSON file */
int mdk_load_message_protection_vectors(const char *path,
                                         MdkMessageProtectionVector *vectors,
                                         size_t *count,
                                         size_t max_count);

/* Load tree operations vectors from JSON file */
int mdk_load_tree_operations_vectors(const char *path,
                                      MdkTreeOperationsVector *vectors,
                                      size_t *count,
                                      size_t max_count);

/* Load tree validation vectors from JSON file */
int mdk_load_tree_validation_vectors(const char *path,
                                      MdkTreeValidationVector *vectors,
                                      size_t *count,
                                      size_t max_count);

/* Load TreeKEM vectors from JSON file */
int mdk_load_treekem_vectors(const char *path,
                              MdkTreeKEMVector *vectors,
                              size_t *count,
                              size_t max_count);

/* Load passive client vectors from JSON file */
int mdk_load_passive_client_vectors(const char *path,
                                     MdkPassiveClientVector *vectors,
                                     size_t *count,
                                     size_t max_count);

/* Helper: decode hex string to bytes */
bool mdk_hex_decode(uint8_t *out, const char *hex, size_t out_len);

/* Helper: find JSON string value by key */
const char *mdk_json_find_string(const char *json, const char *key);

/* Helper: find JSON number value by key */
int mdk_json_find_number(const char *json, const char *key, uint32_t *out);

/* Cleanup functions */
void mdk_free_tree_math_vector(MdkTreeMathVector *vec);

#endif /* MDK_VECTOR_LOADER_H */
