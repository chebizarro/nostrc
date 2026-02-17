/*
 * libmarmot - Error string implementation
 *
 * SPDX-License-Identifier: MIT
 */

#include <marmot/marmot-error.h>

const char *
marmot_error_string(MarmotError error)
{
    switch (error) {
    case MARMOT_OK:                          return "success";

    /* General */
    case MARMOT_ERR_INVALID_ARG:             return "invalid argument";
    case MARMOT_ERR_MEMORY:                  return "out of memory";
    case MARMOT_ERR_NOT_IMPLEMENTED:         return "not implemented";
    case MARMOT_ERR_INTERNAL:                return "internal error";
    case MARMOT_ERR_UNSUPPORTED:             return "unsupported feature or version";
    case MARMOT_ERR_INVALID_INPUT:           return "invalid input";

    /* Encoding */
    case MARMOT_ERR_HEX:                     return "hex encoding/decoding error";
    case MARMOT_ERR_BASE64:                  return "base64 encoding/decoding error";
    case MARMOT_ERR_UTF8:                    return "invalid UTF-8";
    case MARMOT_ERR_TLS_CODEC:              return "TLS serialization error";

    /* Crypto */
    case MARMOT_ERR_KEYS:                    return "key error";
    case MARMOT_ERR_CRYPTO:                  return "cryptographic error";
    case MARMOT_ERR_NIP44:                   return "NIP-44 encryption/decryption error";
    case MARMOT_ERR_SIGNATURE:               return "signature verification failed";

    /* Event */
    case MARMOT_ERR_EVENT:                   return "event error";
    case MARMOT_ERR_EVENT_BUILD:             return "event build error";
    case MARMOT_ERR_UNEXPECTED_EVENT:        return "unexpected event kind";
    case MARMOT_ERR_MISSING_RUMOR_EVENT_ID:  return "missing rumor event ID";
    case MARMOT_ERR_AUTHOR_MISMATCH:        return "author mismatch";
    case MARMOT_ERR_INVALID_TIMESTAMP:       return "invalid event timestamp";

    /* Group */
    case MARMOT_ERR_GROUP:                   return "group error";
    case MARMOT_ERR_GROUP_NOT_FOUND:         return "group not found";
    case MARMOT_ERR_GROUP_EXPORTER_SECRET:   return "exporter secret error";
    case MARMOT_ERR_OWN_LEAF_NOT_FOUND:     return "own leaf node not found in ratchet tree";
    case MARMOT_ERR_OWN_COMMIT_PENDING:     return "own commit is pending — call merge first";
    case MARMOT_ERR_COMMIT_FROM_NON_ADMIN:  return "commit from non-admin member";
    case MARMOT_ERR_PROTOCOL_GROUP_MISMATCH: return "protocol group ID mismatch";

    /* Key package */
    case MARMOT_ERR_KEY_NOT_FOUND:           return "matching key not found";
    case MARMOT_ERR_KEY_PACKAGE:             return "key package error";
    case MARMOT_ERR_KEY_PACKAGE_IDENTITY:    return "key package identity mismatch";
    case MARMOT_ERR_IDENTITY_CHANGE:         return "identity change not allowed";

    /* Message */
    case MARMOT_ERR_MESSAGE:                 return "message error";
    case MARMOT_ERR_OWN_MESSAGE:             return "received own message";
    case MARMOT_ERR_FROM_NON_MEMBER:         return "message from non-member";
    case MARMOT_ERR_MESSAGE_NOT_FOUND:       return "message not found";
    case MARMOT_ERR_WRONG_EPOCH:             return "wrong epoch";
    case MARMOT_ERR_WRONG_GROUP_ID:          return "wrong group ID";
    case MARMOT_ERR_USE_AFTER_EVICTION:      return "use after eviction";

    /* Welcome */
    case MARMOT_ERR_WELCOME:                 return "welcome error";
    case MARMOT_ERR_WELCOME_INVALID:         return "invalid welcome message";
    case MARMOT_ERR_WELCOME_PREVIOUSLY_FAILED: return "welcome previously failed";
    case MARMOT_ERR_WELCOME_NOT_FOUND:       return "welcome not found";

    /* Extension */
    case MARMOT_ERR_EXTENSION:               return "extension error";
    case MARMOT_ERR_EXTENSION_NOT_FOUND:     return "extension not found";
    case MARMOT_ERR_EXTENSION_TYPE:          return "wrong extension type";
    case MARMOT_ERR_EXTENSION_VERSION:       return "unsupported extension version";
    case MARMOT_ERR_EXTENSION_FORMAT:        return "malformed extension data";

    /* Serialization */
    case MARMOT_ERR_DESERIALIZATION:          return "deserialization error";
    case MARMOT_ERR_SERIALIZATION:           return "serialization error";

    /* Validation */
    case MARMOT_ERR_VALIDATION:              return "validation error";
    case MARMOT_ERR_MLS:                     return "MLS protocol error";

    /* Image */
    case MARMOT_ERR_INVALID_IMAGE_HASH_LEN:  return "invalid image hash length (expected 32)";
    case MARMOT_ERR_INVALID_IMAGE_KEY_LEN:   return "invalid image key length (expected 32)";
    case MARMOT_ERR_INVALID_IMAGE_NONCE_LEN: return "invalid image nonce length (expected 12)";
    case MARMOT_ERR_INVALID_IMAGE_UPLOAD_LEN: return "invalid image upload key length (expected 32)";

    /* Storage */
    case MARMOT_ERR_STORAGE:                 return "storage error";
    case MARMOT_ERR_STORAGE_NOT_FOUND:       return "storage: not found";
    case MARMOT_ERR_STORAGE_CONSTRAINT:      return "storage: constraint violation";

    /* MLS protocol */
    case MARMOT_ERR_MLS_LIBRARY:             return "MLS library error";
    case MARMOT_ERR_MLS_CREATE_MESSAGE:      return "MLS: failed to create message";
    case MARMOT_ERR_MLS_EXPORT_SECRET:       return "MLS: failed to export secret";
    case MARMOT_ERR_MLS_MERGE_COMMIT:        return "MLS: failed to merge commit";
    case MARMOT_ERR_MLS_SELF_UPDATE:         return "MLS: self-update failed";
    case MARMOT_ERR_MLS_ADD_MEMBERS:         return "MLS: failed to add members";
    case MARMOT_ERR_MLS_PROCESS_MESSAGE:     return "MLS: failed to process message";
    case MARMOT_ERR_MLS_FRAMING:             return "MLS: framing error";

    /* Media (MIP-04) */
    case MARMOT_ERR_MEDIA_DECRYPT:           return "media decryption failed";
    case MARMOT_ERR_MEDIA_HASH_MISMATCH:     return "media file hash mismatch";

    /* Snapshot */
    case MARMOT_ERR_SNAPSHOT_FAILED:         return "snapshot operation failed";

    /* Group ID tag */
    case MARMOT_ERR_MISSING_GROUP_ID_TAG:    return "missing group ID tag";
    case MARMOT_ERR_INVALID_GROUP_ID_FORMAT: return "invalid group ID format";
    case MARMOT_ERR_MULTIPLE_GROUP_ID_TAGS:  return "multiple group ID tags";

    default: return "unknown error";
    }
}
