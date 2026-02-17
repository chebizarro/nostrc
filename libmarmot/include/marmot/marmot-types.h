/*
 * libmarmot - C implementation of the Marmot protocol (MLS + Nostr)
 *
 * Core type definitions.
 * These mirror the MDK storage traits types for interoperability.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_TYPES_H
#define MARMOT_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ──────────────────────────────────────────────────────────────────────────
 * Nostr event kinds used by Marmot (MIP-00 through MIP-03)
 * ──────────────────────────────────────────────────────────────────────── */

/** Kind 443: MLS Key Package (MIP-00) */
#define MARMOT_KIND_KEY_PACKAGE     443

/** Kind 444: MLS Welcome (MIP-02) — gift-wrapped via NIP-59 */
#define MARMOT_KIND_WELCOME         444

/** Kind 445: MLS Group Message (MIP-03) — gift-wrapped via NIP-59 */
#define MARMOT_KIND_GROUP_MESSAGE   445

/* ──────────────────────────────────────────────────────────────────────────
 * MLS constants
 * ──────────────────────────────────────────────────────────────────────── */

/** Nostr Group Data Extension type (0xF2EE — "Be FREE") */
#define MARMOT_EXTENSION_TYPE       0xF2EE

/** The only required ciphersuite: MLS_128_DHKEMX25519_AES128GCM_SHA256_Ed25519 */
#define MARMOT_CIPHERSUITE          0x0001

/** Current extension format version */
#define MARMOT_EXTENSION_VERSION    2

/* ──────────────────────────────────────────────────────────────────────────
 * Opaque types (forward declarations)
 * ──────────────────────────────────────────────────────────────────────── */

/** Main Marmot library instance */
typedef struct Marmot Marmot;

/** MLS group state (internal) */
typedef struct MarmotMlsGroup MarmotMlsGroup;

/** MLS key package (internal) */
typedef struct MarmotMlsKeyPackage MarmotMlsKeyPackage;

/* ──────────────────────────────────────────────────────────────────────────
 * Group ID
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotGroupId:
 *
 * Variable-length MLS group ID. The MLS protocol assigns group IDs that
 * may vary in length. This struct holds a copy of the raw bytes.
 */
typedef struct {
    uint8_t *data;
    size_t   len;
} MarmotGroupId;

/** Create a MarmotGroupId from raw bytes (copies the data) */
MarmotGroupId marmot_group_id_new(const uint8_t *data, size_t len);

/** Free a MarmotGroupId's internal data */
void marmot_group_id_free(MarmotGroupId *gid);

/** Compare two MarmotGroupIds for equality */
bool marmot_group_id_equal(const MarmotGroupId *a, const MarmotGroupId *b);

/** Get hex string representation (caller frees result) */
char *marmot_group_id_to_hex(const MarmotGroupId *gid);

/* ──────────────────────────────────────────────────────────────────────────
 * Configuration
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotConfig:
 *
 * Configuration for Marmot behavior. All fields have secure defaults
 * matching the MDK reference implementation.
 */
typedef struct {
    /**
     * Maximum age for accepted events in seconds.
     * Events older than this are rejected to prevent replay attacks.
     * Default: 3888000 (45 days)
     */
    uint64_t max_event_age_secs;

    /**
     * Maximum future timestamp skew allowed in seconds.
     * Events too far in the future are rejected.
     * Default: 300 (5 minutes)
     */
    uint64_t max_future_skew_secs;

    /**
     * Number of past message decryption secrets to retain
     * for out-of-order delivery handling.
     * Default: 100
     */
    uint32_t out_of_order_tolerance;

    /**
     * Maximum number of messages that can be skipped before
     * decryption fails (forward ratchet distance).
     * Default: 1000
     */
    uint32_t max_forward_distance;

    /**
     * Number of epoch snapshots to retain for rollback support.
     * Default: 5
     */
    uint32_t epoch_snapshot_retention;

    /**
     * Time-to-live for snapshots in seconds.
     * Snapshots older than this are pruned on startup.
     * Default: 604800 (1 week)
     */
    uint64_t snapshot_ttl_seconds;
} MarmotConfig;

/**
 * marmot_config_default:
 *
 * Initialize a MarmotConfig with secure defaults matching MDK.
 *
 * Returns: MarmotConfig with default values
 */
MarmotConfig marmot_config_default(void);

/* ──────────────────────────────────────────────────────────────────────────
 * Group state
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    MARMOT_GROUP_STATE_ACTIVE   = 0,
    MARMOT_GROUP_STATE_INACTIVE = 1,
    MARMOT_GROUP_STATE_PENDING  = 2,
} MarmotGroupState;

const char *marmot_group_state_to_string(MarmotGroupState state);
MarmotGroupState marmot_group_state_from_string(const char *s);

/* ──────────────────────────────────────────────────────────────────────────
 * Group
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotGroup:
 *
 * Represents a Marmot group with metadata.
 * Mirrors MDK's Group struct for interoperability.
 */
typedef struct {
    /** MLS group ID (primary key, doesn't change) */
    MarmotGroupId mls_group_id;

    /** Nostr group ID used in published events (can change) */
    uint8_t nostr_group_id[32];

    /** Group name (UTF-8, caller-owned) */
    char *name;

    /** Group description (UTF-8, caller-owned) */
    char *description;

    /** Group image hash (32 bytes, or NULL if no image) */
    uint8_t *image_hash;    /* [32] or NULL */

    /** Image encryption key/seed (32 bytes, or NULL) */
    uint8_t *image_key;     /* [32] or NULL */

    /** Image nonce (12 bytes, or NULL) */
    uint8_t *image_nonce;   /* [12] or NULL */

    /** Admin public keys (array of 32-byte x-only pubkeys) */
    uint8_t (*admin_pubkeys)[32];
    size_t   admin_count;

    /** Last message event ID hex (or NULL) */
    char *last_message_id;

    /** Last message timestamp (0 if unset) */
    int64_t last_message_at;

    /** Last message processed timestamp (0 if unset) */
    int64_t last_message_processed_at;

    /** Current MLS epoch */
    uint64_t epoch;

    /** Group state */
    MarmotGroupState state;
} MarmotGroup;

/** Allocate and zero-initialize a MarmotGroup */
MarmotGroup *marmot_group_new(void);

/** Deep-free a MarmotGroup and all its owned data */
void marmot_group_free(MarmotGroup *group);

/* ──────────────────────────────────────────────────────────────────────────
 * Message state
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    MARMOT_MSG_STATE_CREATED            = 0,
    MARMOT_MSG_STATE_PROCESSED          = 1,
    MARMOT_MSG_STATE_DELETED            = 2,
    MARMOT_MSG_STATE_EPOCH_INVALIDATED  = 3,
} MarmotMessageState;

/* ──────────────────────────────────────────────────────────────────────────
 * Message
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotMessage:
 *
 * A decrypted group message.
 * Mirrors MDK's Message struct for interoperability.
 */
typedef struct {
    /** Event ID (32 bytes) */
    uint8_t id[32];

    /** Author pubkey (32 bytes x-only) */
    uint8_t pubkey[32];

    /** Event kind */
    uint32_t kind;

    /** MLS group ID */
    MarmotGroupId mls_group_id;

    /** Sender-assigned timestamp */
    int64_t created_at;

    /** Local processing timestamp */
    int64_t processed_at;

    /** Decrypted content (UTF-8, caller-owned) */
    char *content;

    /** Event tags as JSON string (caller-owned, or NULL) */
    char *tags_json;

    /** Full unsigned event as JSON (caller-owned, or NULL) */
    char *event_json;

    /** Wrapper event ID (32 bytes — the kind:1059 gift wrap) */
    uint8_t wrapper_event_id[32];

    /** MLS epoch when processed (0 if unknown) */
    uint64_t epoch;

    /** Message state */
    MarmotMessageState state;
} MarmotMessage;

/** Allocate and zero-initialize a MarmotMessage */
MarmotMessage *marmot_message_new(void);

/** Deep-free a MarmotMessage and all its owned data */
void marmot_message_free(MarmotMessage *msg);

/* ──────────────────────────────────────────────────────────────────────────
 * Welcome state
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    MARMOT_WELCOME_STATE_PENDING  = 0,
    MARMOT_WELCOME_STATE_ACCEPTED = 1,
    MARMOT_WELCOME_STATE_DECLINED = 2,
} MarmotWelcomeState;

/* ──────────────────────────────────────────────────────────────────────────
 * Welcome
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotWelcome:
 *
 * A received welcome (group invitation).
 * Mirrors MDK's Welcome struct for interoperability.
 */
typedef struct {
    /** Rumor event ID (32 bytes) */
    uint8_t id[32];

    /** Full unsigned event JSON (caller-owned) */
    char *event_json;

    /** MLS group ID */
    MarmotGroupId mls_group_id;

    /** Nostr group ID (32 bytes) */
    uint8_t nostr_group_id[32];

    /** Group name (caller-owned) */
    char *group_name;

    /** Group description (caller-owned) */
    char *group_description;

    /** Group image hash (32 bytes, or NULL) */
    uint8_t *group_image_hash;

    /** Admin public keys (array of 32-byte x-only pubkeys) */
    uint8_t (*group_admin_pubkeys)[32];
    size_t   group_admin_count;

    /** Group relay URLs (null-terminated array of strings, caller-owned) */
    char **group_relays;
    size_t  group_relay_count;

    /** Welcomer's pubkey (32 bytes x-only) */
    uint8_t welcomer[32];

    /** Number of members in the group at invite time */
    uint32_t member_count;

    /** Welcome state */
    MarmotWelcomeState state;

    /** Wrapper event ID (32 bytes — the kind:1059 gift wrap) */
    uint8_t wrapper_event_id[32];
} MarmotWelcome;

/** Allocate and zero-initialize a MarmotWelcome */
MarmotWelcome *marmot_welcome_new(void);

/** Deep-free a MarmotWelcome and all its owned data */
void marmot_welcome_free(MarmotWelcome *welcome);

/* ──────────────────────────────────────────────────────────────────────────
 * Group exporter secret
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    MarmotGroupId mls_group_id;
    uint64_t epoch;
    uint8_t  secret[32];
} MarmotExporterSecret;

/* ──────────────────────────────────────────────────────────────────────────
 * Group relay
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    char *relay_url;           /* caller-owned */
    MarmotGroupId mls_group_id;
} MarmotGroupRelay;

/* ──────────────────────────────────────────────────────────────────────────
 * Nostr Group Data Extension (0xF2EE)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotGroupDataExtension:
 *
 * The Marmot Group Data Extension embedded in the MLS GroupContext.
 * Contains Nostr-specific group metadata.
 *
 * This is TLS-serialized (MIP-01) using length-prefixed encoding.
 */
typedef struct {
    /** Extension format version (current: 2) */
    uint16_t version;

    /** Nostr group ID (32 bytes, random) */
    uint8_t nostr_group_id[32];

    /** Group name (UTF-8, caller-owned) */
    char *name;

    /** Group description (UTF-8, caller-owned) */
    char *description;

    /** Admin public keys (array of 32-byte x-only pubkeys) */
    uint8_t (*admins)[32];
    size_t   admin_count;

    /** Relay URLs (null-terminated strings) */
    char **relays;
    size_t  relay_count;

    /** Optional: group image hash (32 bytes) */
    uint8_t *image_hash;    /* [32] or NULL */

    /** Optional: image key/seed (32 bytes) */
    uint8_t *image_key;     /* [32] or NULL */

    /** Optional: image nonce (12 bytes) */
    uint8_t *image_nonce;   /* [12] or NULL */

    /** Optional: image upload key/seed (32 bytes, v2 only) */
    uint8_t *image_upload_key; /* [32] or NULL */
} MarmotGroupDataExtension;

/** Allocate and zero-initialize a MarmotGroupDataExtension */
MarmotGroupDataExtension *marmot_group_data_extension_new(void);

/** Deep-free a MarmotGroupDataExtension */
void marmot_group_data_extension_free(MarmotGroupDataExtension *ext);

/** Serialize extension to TLS wire format. Caller frees *out_data. */
int marmot_group_data_extension_serialize(const MarmotGroupDataExtension *ext,
                                           uint8_t **out_data, size_t *out_len);

/** Deserialize extension from TLS wire format. Caller frees result. */
MarmotGroupDataExtension *marmot_group_data_extension_deserialize(
    const uint8_t *data, size_t len);

/* ──────────────────────────────────────────────────────────────────────────
 * Pagination
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    MARMOT_SORT_CREATED_AT_FIRST   = 0,
    MARMOT_SORT_PROCESSED_AT_FIRST = 1,
} MarmotSortOrder;

typedef struct {
    size_t limit;
    size_t offset;
    MarmotSortOrder sort_order;
} MarmotPagination;

/** Default pagination (limit=1000, offset=0, sort=created_at) */
MarmotPagination marmot_pagination_default(void);

/* ──────────────────────────────────────────────────────────────────────────
 * Message processing result
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    MARMOT_RESULT_APPLICATION_MESSAGE = 0,
    MARMOT_RESULT_COMMIT              = 1,
    MARMOT_RESULT_PROPOSAL            = 2,
    MARMOT_RESULT_UNPROCESSABLE       = 3,
    MARMOT_RESULT_OWN_MESSAGE         = 4,
} MarmotMessageResultType;

/**
 * MarmotMessageResult:
 *
 * Result of processing an incoming group message.
 */
typedef struct {
    MarmotMessageResultType type;

    /** Valid when type == MARMOT_RESULT_APPLICATION_MESSAGE */
    struct {
        /** Decrypted inner event JSON (caller-owned) */
        char *inner_event_json;
        /** Sender pubkey hex (caller-owned) */
        char *sender_pubkey_hex;
    } app_msg;

    /** Valid when type == MARMOT_RESULT_COMMIT */
    struct {
        /** Updated group info (caller-owned, may be NULL) */
        MarmotGroup *updated_group;
    } commit;
} MarmotMessageResult;

/** Free a MarmotMessageResult's owned data */
void marmot_message_result_free(MarmotMessageResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Group creation result
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotCreateGroupResult:
 *
 * Result of creating a new group. Contains the group metadata,
 * welcome rumors for each invited member, and the evolution event.
 */
typedef struct {
    /** The created group */
    MarmotGroup *group;

    /** Welcome rumor events (unsigned, one per invited member) */
    char **welcome_rumor_jsons;  /* JSON strings, caller-owned */
    size_t  welcome_count;

    /** Evolution event JSON (the kind:445 commit, caller-owned) */
    char *evolution_event_json;
} MarmotCreateGroupResult;

/** Free a MarmotCreateGroupResult's owned data */
void marmot_create_group_result_free(MarmotCreateGroupResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Group update types
 * ──────────────────────────────────────────────────────────────────────── */

typedef enum {
    MARMOT_UPDATE_ADD_MEMBERS     = 1,
    MARMOT_UPDATE_REMOVE_MEMBERS  = 2,
    MARMOT_UPDATE_RENAME          = 3,
    MARMOT_UPDATE_DESCRIPTION     = 4,
    MARMOT_UPDATE_ADD_ADMINS      = 5,
    MARMOT_UPDATE_REMOVE_ADMINS   = 6,
    MARMOT_UPDATE_ADD_RELAYS      = 7,
    MARMOT_UPDATE_REMOVE_RELAYS   = 8,
    MARMOT_UPDATE_SELF_UPDATE     = 9,
} MarmotUpdateType;

/* ──────────────────────────────────────────────────────────────────────────
 * Key package creation result
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    /** Key package event JSON (kind:443, unsigned, caller-owned) */
    char *event_json;

    /** Key package reference (hash, 32 bytes) */
    uint8_t key_package_ref[32];
} MarmotKeyPackageResult;

/** Free a MarmotKeyPackageResult's owned data */
void marmot_key_package_result_free(MarmotKeyPackageResult *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Outgoing message result
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    /** Group message event JSON (kind:445, unsigned, caller-owned) */
    char *event_json;

    /** Stored message record */
    MarmotMessage *message;
} MarmotOutgoingMessage;

/** Free a MarmotOutgoingMessage's owned data */
void marmot_outgoing_message_free(MarmotOutgoingMessage *result);

/* ──────────────────────────────────────────────────────────────────────────
 * Group config for creation
 * ──────────────────────────────────────────────────────────────────────── */

typedef struct {
    char *name;
    char *description;
    uint8_t (*admin_pubkeys)[32];
    size_t   admin_count;
    char **relay_urls;
    size_t  relay_count;
} MarmotGroupConfig;

/* ──────────────────────────────────────────────────────────────────────────
 * MIP-04: Encrypted Media types
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * MarmotImetaInfo:
 *
 * Metadata about an encrypted media file, stored in the Nostr event's
 * "imeta" tag (NIP-94). Used for decryption.
 */
typedef struct {
    char    *mime_type;      /**< MIME type (e.g., "image/png") */
    char    *filename;       /**< Original filename (optional) */
    char    *url;            /**< URL where encrypted file is hosted (optional) */
    size_t   original_size;  /**< Unencrypted file size in bytes */
    uint8_t  file_hash[32];  /**< SHA-256 of the plaintext file */
    uint8_t  nonce[12];      /**< ChaCha20-Poly1305 nonce used for encryption */
    uint64_t epoch;          /**< MLS epoch when the encryption key was derived */
} MarmotImetaInfo;

/**
 * MarmotEncryptedMedia:
 *
 * Result of marmot_encrypt_media(). Contains the encrypted data
 * and metadata needed for upload and sharing.
 */
typedef struct {
    uint8_t        *encrypted_data;  /**< Encrypted file bytes (caller frees) */
    size_t          encrypted_len;   /**< Length of encrypted_data */
    uint8_t         nonce[12];       /**< ChaCha20-Poly1305 nonce */
    uint8_t         file_hash[32];   /**< SHA-256 of original file */
    size_t          original_size;   /**< Original file size */
    MarmotImetaInfo imeta;           /**< Metadata for the Nostr event tag */
} MarmotEncryptedMedia;

/** Clear and free all data within an encrypted media result */
void marmot_encrypted_media_clear(MarmotEncryptedMedia *result);

#ifdef __cplusplus
}
#endif

#endif /* MARMOT_TYPES_H */
