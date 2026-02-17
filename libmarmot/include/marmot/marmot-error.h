/*
 * libmarmot - C implementation of the Marmot protocol (MLS + Nostr)
 *
 * Error codes and error handling.
 * Mirrors MDK error variants for interoperability.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_ERROR_H
#define MARMOT_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * MarmotError:
 *
 * Error codes returned by libmarmot functions.
 * Zero indicates success; negative values indicate errors.
 */
typedef enum {
    /* Success */
    MARMOT_OK                           =  0,

    /* General errors */
    MARMOT_ERR_INVALID_ARG              = -1,
    MARMOT_ERR_MEMORY                   = -2,
    MARMOT_ERR_NOT_IMPLEMENTED          = -3,
    MARMOT_ERR_INTERNAL                 = -4,
    MARMOT_ERR_UNSUPPORTED              = -5,
    MARMOT_ERR_INVALID_INPUT            = -6,  /* alias for INVALID_ARG */

    /* Hex / encoding errors */
    MARMOT_ERR_HEX                      = -10,
    MARMOT_ERR_BASE64                   = -11,
    MARMOT_ERR_UTF8                     = -12,
    MARMOT_ERR_TLS_CODEC                = -13,

    /* Key / crypto errors */
    MARMOT_ERR_KEYS                     = -20,
    MARMOT_ERR_CRYPTO                   = -21,
    MARMOT_ERR_NIP44                    = -22,
    MARMOT_ERR_SIGNATURE                = -23,

    /* Event errors */
    MARMOT_ERR_EVENT                    = -30,
    MARMOT_ERR_EVENT_BUILD              = -31,
    MARMOT_ERR_UNEXPECTED_EVENT         = -32,
    MARMOT_ERR_MISSING_RUMOR_EVENT_ID   = -33,
    MARMOT_ERR_AUTHOR_MISMATCH         = -34,
    MARMOT_ERR_INVALID_TIMESTAMP        = -35,

    /* Group errors */
    MARMOT_ERR_GROUP                    = -40,
    MARMOT_ERR_GROUP_NOT_FOUND          = -41,
    MARMOT_ERR_GROUP_EXPORTER_SECRET    = -42,
    MARMOT_ERR_OWN_LEAF_NOT_FOUND      = -43,
    MARMOT_ERR_OWN_COMMIT_PENDING      = -44,
    MARMOT_ERR_COMMIT_FROM_NON_ADMIN   = -45,
    MARMOT_ERR_PROTOCOL_GROUP_MISMATCH = -46,

    /* Key package errors */
    MARMOT_ERR_KEY_NOT_FOUND            = -49,
    MARMOT_ERR_KEY_PACKAGE              = -50,
    MARMOT_ERR_KEY_PACKAGE_IDENTITY     = -51,
    MARMOT_ERR_IDENTITY_CHANGE          = -52,

    /* Message errors */
    MARMOT_ERR_MESSAGE                  = -60,
    MARMOT_ERR_OWN_MESSAGE              = -61,
    MARMOT_ERR_FROM_NON_MEMBER          = -62,
    MARMOT_ERR_MESSAGE_NOT_FOUND        = -63,
    MARMOT_ERR_WRONG_EPOCH              = -64,
    MARMOT_ERR_WRONG_GROUP_ID           = -65,
    MARMOT_ERR_USE_AFTER_EVICTION       = -66,

    /* Welcome errors */
    MARMOT_ERR_WELCOME                  = -70,
    MARMOT_ERR_WELCOME_INVALID          = -71,
    MARMOT_ERR_WELCOME_PREVIOUSLY_FAILED = -72,
    MARMOT_ERR_WELCOME_NOT_FOUND        = -73,

    /* Extension errors */
    MARMOT_ERR_EXTENSION                = -80,
    MARMOT_ERR_EXTENSION_NOT_FOUND      = -81,
    MARMOT_ERR_EXTENSION_TYPE           = -82,
    MARMOT_ERR_EXTENSION_VERSION        = -83,
    MARMOT_ERR_EXTENSION_FORMAT         = -84,

    /* Image errors */
    MARMOT_ERR_INVALID_IMAGE_HASH_LEN   = -140,
    MARMOT_ERR_INVALID_IMAGE_KEY_LEN    = -141,
    MARMOT_ERR_INVALID_IMAGE_NONCE_LEN  = -142,
    MARMOT_ERR_INVALID_IMAGE_UPLOAD_LEN = -143,

    /* Serialization errors */
    MARMOT_ERR_DESERIALIZATION           = -90,
    MARMOT_ERR_SERIALIZATION             = -91,

    /* Validation errors */
    MARMOT_ERR_VALIDATION                = -95,
    MARMOT_ERR_MLS                       = -96,

    /* Storage errors */
    MARMOT_ERR_STORAGE                  = -100,
    MARMOT_ERR_STORAGE_NOT_FOUND        = -101,
    MARMOT_ERR_STORAGE_CONSTRAINT       = -102,

    /* MLS protocol errors */
    MARMOT_ERR_MLS_LIBRARY              = -110,
    MARMOT_ERR_MLS_CREATE_MESSAGE       = -111,
    MARMOT_ERR_MLS_EXPORT_SECRET        = -112,
    MARMOT_ERR_MLS_MERGE_COMMIT         = -113,
    MARMOT_ERR_MLS_SELF_UPDATE          = -114,
    MARMOT_ERR_MLS_ADD_MEMBERS          = -115,
    MARMOT_ERR_MLS_PROCESS_MESSAGE      = -116,
    MARMOT_ERR_MLS_FRAMING              = -117,

    /* Snapshot errors */
    MARMOT_ERR_SNAPSHOT_FAILED          = -120,

    /* Missing group ID tag */
    MARMOT_ERR_MISSING_GROUP_ID_TAG     = -130,
    MARMOT_ERR_INVALID_GROUP_ID_FORMAT  = -131,
    MARMOT_ERR_MULTIPLE_GROUP_ID_TAGS   = -132,
} MarmotError;

/**
 * marmot_error_string:
 * @error: error code
 *
 * Get a human-readable description of an error code.
 *
 * Returns: (transfer none): static string describing the error
 */
const char *marmot_error_string(MarmotError error);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_ERROR_H */
