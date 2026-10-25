#ifndef BLOSSOM_TYPES_H
#define BLOSSOM_TYPES_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <blossom/export.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Integer status permits signer callbacks to propagate caller-specific errors. */
typedef int blossom_error_t;
enum {
    BLOSSOM_OK = 0,
    BLOSSOM_ERR_NOMEM = -1,
    BLOSSOM_ERR_INVALID_ARG = -2,
    BLOSSOM_ERR_NOT_FOUND = -3,
    BLOSSOM_ERR_NETWORK = -4,
    BLOSSOM_ERR_AUTH = -5,
    BLOSSOM_ERR_HASH_MISMATCH = -6,
    BLOSSOM_ERR_NOSTR = -9,
    BLOSSOM_ERR_IO = -11,
    BLOSSOM_ERR_BLOSSOM = -12,
    BLOSSOM_ERR_UPLOAD = -13,
    BLOSSOM_ERR_TIMEOUT = -14,
    BLOSSOM_ERR_TOO_LARGE = -15,
};

typedef struct {
    char sha256[65];    /**< SHA-256 hex of the blob */
    size_t size;        /**< Blob size in bytes */
    char *url;          /**< Full URL to retrieve the blob (owned, may be NULL) */
    char *mime_type;    /**< MIME type (owned, may be NULL) */
    int64_t uploaded;   /**< Upload timestamp (Unix epoch) */
} blossom_blob_descriptor_t;

/**
 * blossom_blob_descriptor_free:
 * @desc: descriptor to free (nullable)
 */
BLOSSOM_API void blossom_blob_descriptor_free(blossom_blob_descriptor_t *desc);

typedef blossom_error_t (*blossom_sign_event_fn)(const char *event_json,
                                               char **out_signed_json,
                                               void *user_data);

/**
 * blossom_signer_t:
 *
 * Signer context providing identity and signing capabilities.
 */
typedef struct {
    const char *pubkey;             /**< Hex public key (64 chars) */
    blossom_sign_event_fn sign;      /**< Signing callback */
    void *user_data;                /**< Opaque context for the signer */
} blossom_signer_t;


#ifdef __cplusplus
}
#endif
#endif
