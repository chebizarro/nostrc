# libmarmot API Reference

Complete API reference for the libmarmot C library and its GObject wrapper (`marmot-gobject`).

## Table of Contents

- [Core Types](#core-types)
- [Error Handling](#error-handling)
- [Lifecycle](#lifecycle)
- [MIP-00: Credentials & KeyPackages](#mip-00-credentials--keypackages)
- [MIP-01: Group Construction](#mip-01-group-construction)
- [MIP-02: Welcome Events](#mip-02-welcome-events)
- [MIP-03: Group Messages](#mip-03-group-messages)
- [MIP-04: Encrypted Media](#mip-04-encrypted-media)
- [Group Queries](#group-queries)
- [Storage Interface](#storage-interface)
- [Storage Backends](#storage-backends)
- [Configuration](#configuration)
- [GObject Wrapper (marmot-gobject)](#gobject-wrapper-marmot-gobject)
- [Thread Safety](#thread-safety)

---

## Core Types

### `Marmot`
The main library handle. Opaque struct managing MLS group state, storage, and configuration.

### `MarmotGroupId`
Variable-length MLS group ID. The MLS protocol assigns group IDs that may vary in length.

```c
typedef struct {
    uint8_t *data;
    size_t   len;
} MarmotGroupId;

MarmotGroupId  marmot_group_id_new(const uint8_t *data, size_t len);  // copies data
void           marmot_group_id_free(MarmotGroupId *gid);
char          *marmot_group_id_to_hex(const MarmotGroupId *gid);      // caller frees
bool           marmot_group_id_equal(const MarmotGroupId *a, const MarmotGroupId *b);
```

### `MarmotGroup`
Represents a Marmot group with its Nostr and MLS identifiers plus metadata.

```c
typedef struct {
    MarmotGroupId  mls_group_id;              // Variable-length MLS group ID
    uint8_t        nostr_group_id[32];        // Nostr group ID (h-tag)
    char          *name;                      // Group name (nullable, caller-owned)
    char          *description;               // Group description (nullable, caller-owned)
    uint8_t       (*admin_pubkeys)[32];       // Array of 32-byte x-only pubkeys
    size_t         admin_count;
    uint8_t       *image_hash;                // [32] or NULL
    uint8_t       *image_key;                 // [32] or NULL
    uint8_t       *image_nonce;               // [12] or NULL
    char          *last_message_id;           // Last message event ID hex (nullable)
    int64_t        last_message_at;           // Last message timestamp (0 if unset)
    int64_t        last_message_processed_at; // 0 if unset
    uint64_t       epoch;                     // Current MLS epoch number
    MarmotGroupState state;                   // ACTIVE, INACTIVE, PENDING
} MarmotGroup;

MarmotGroup *marmot_group_new(void);
void         marmot_group_free(MarmotGroup *group);
```

### `MarmotMessage`
Represents a decrypted group message.

```c
typedef struct {
    uint8_t         id[32];              // Event ID (32 bytes)
    uint8_t         pubkey[32];          // Author pubkey (32 bytes x-only)
    uint32_t        kind;                // Inner event kind
    MarmotGroupId   mls_group_id;        // Source group
    int64_t         created_at;          // Sender-assigned timestamp
    int64_t         processed_at;        // Local processing timestamp
    char           *content;             // Decrypted content (nullable, caller-owned)
    char           *tags_json;           // Event tags as JSON string (nullable)
    char           *event_json;          // Full unsigned event as JSON (nullable)
    uint8_t         wrapper_event_id[32]; // The kind:1059 gift wrap event ID
    uint64_t        epoch;               // MLS epoch when processed
    MarmotMessageState state;            // CREATED, PROCESSED, DELETED, EPOCH_INVALIDATED
} MarmotMessage;

MarmotMessage *marmot_message_new(void);
void           marmot_message_free(MarmotMessage *msg);
```

### `MarmotWelcome`
Represents a received Welcome event (group invitation).

```c
typedef struct {
    uint8_t          id[32];                   // Rumor event ID
    char            *event_json;               // Full unsigned event JSON (caller-owned)
    MarmotGroupId    mls_group_id;             // Target group
    uint8_t          nostr_group_id[32];       // Nostr group ID
    char            *group_name;               // Preview name (nullable)
    char            *group_description;        // Preview description (nullable)
    uint8_t         *group_image_hash;         // [32] or NULL
    uint8_t         (*group_admin_pubkeys)[32]; // Admin pubkeys array
    size_t           group_admin_count;
    char           **group_relays;             // Relay URL strings
    size_t           group_relay_count;
    uint8_t          welcomer[32];             // Inviter pubkey (32 bytes x-only)
    uint32_t         member_count;             // Estimated member count
    MarmotWelcomeState state;                  // PENDING, ACCEPTED, DECLINED
    uint8_t          wrapper_event_id[32];     // The kind:1059 gift wrap event ID
} MarmotWelcome;

MarmotWelcome *marmot_welcome_new(void);
void           marmot_welcome_free(MarmotWelcome *welcome);
```

### Enums

```c
typedef enum {
    MARMOT_GROUP_STATE_ACTIVE   = 0,
    MARMOT_GROUP_STATE_INACTIVE = 1,
    MARMOT_GROUP_STATE_PENDING  = 2,
} MarmotGroupState;

typedef enum {
    MARMOT_MSG_STATE_CREATED            = 0,
    MARMOT_MSG_STATE_PROCESSED          = 1,
    MARMOT_MSG_STATE_DELETED            = 2,
    MARMOT_MSG_STATE_EPOCH_INVALIDATED  = 3,
} MarmotMessageState;

typedef enum {
    MARMOT_WELCOME_STATE_PENDING  = 0,
    MARMOT_WELCOME_STATE_ACCEPTED = 1,
    MARMOT_WELCOME_STATE_DECLINED = 2,
} MarmotWelcomeState;

typedef enum {
    MARMOT_RESULT_APPLICATION_MESSAGE = 0,
    MARMOT_RESULT_COMMIT              = 1,
    MARMOT_RESULT_PROPOSAL            = 2,
    MARMOT_RESULT_UNPROCESSABLE       = 3,
    MARMOT_RESULT_OWN_MESSAGE         = 4,
} MarmotMessageResultType;
```

### Event Kind Constants

```c
#define MARMOT_KIND_KEY_PACKAGE   443  // MIP-00
#define MARMOT_KIND_WELCOME       444  // MIP-02
#define MARMOT_KIND_GROUP_MESSAGE 445  // MIP-03
#define MARMOT_EXTENSION_TYPE     0xF2EE  // MIP-01 Group Data Extension
```

---

## Error Handling

All functions return `MarmotError`. Zero indicates success; negative values indicate errors.

```c
MarmotError rc = marmot_create_group(m, ...);
if (rc != MARMOT_OK) {
    fprintf(stderr, "Error: %s\n", marmot_error_string(rc));
}
```

### Error Categories

| Range | Category | Examples |
|-------|----------|---------|
| 0 | Success | `MARMOT_OK` |
| -1 to -6 | General | `MARMOT_ERR_INVALID_ARG`, `MARMOT_ERR_MEMORY`, `MARMOT_ERR_NOT_IMPLEMENTED` |
| -10 to -13 | Encoding | `MARMOT_ERR_HEX`, `MARMOT_ERR_BASE64`, `MARMOT_ERR_TLS_CODEC` |
| -20 to -23 | Crypto | `MARMOT_ERR_KEYS`, `MARMOT_ERR_CRYPTO`, `MARMOT_ERR_NIP44`, `MARMOT_ERR_SIGNATURE` |
| -30 to -35 | Event | `MARMOT_ERR_EVENT`, `MARMOT_ERR_EVENT_BUILD`, `MARMOT_ERR_UNEXPECTED_EVENT` |
| -40 to -48 | Group | `MARMOT_ERR_GROUP_NOT_FOUND`, `MARMOT_ERR_OWN_COMMIT_PENDING`, `MARMOT_ERR_ADMIN_ONLY` |
| -49 to -52 | Key Package | `MARMOT_ERR_KEY_PACKAGE`, `MARMOT_ERR_KEY_PACKAGE_IDENTITY` |
| -60 to -66 | Message | `MARMOT_ERR_OWN_MESSAGE`, `MARMOT_ERR_WRONG_EPOCH`, `MARMOT_ERR_USE_AFTER_EVICTION` |
| -70 to -74 | Welcome | `MARMOT_ERR_WELCOME_INVALID`, `MARMOT_ERR_WELCOME_EXPIRED` |
| -80 to -84 | Extension | `MARMOT_ERR_EXTENSION_NOT_FOUND`, `MARMOT_ERR_EXTENSION_VERSION` |
| -90 to -96 | Serialization | `MARMOT_ERR_DESERIALIZATION`, `MARMOT_ERR_VALIDATION`, `MARMOT_ERR_MLS` |
| -100 to -102 | Storage | `MARMOT_ERR_STORAGE`, `MARMOT_ERR_STORAGE_NOT_FOUND` |
| -110 to -117 | MLS Protocol | `MARMOT_ERR_MLS_LIBRARY`, `MARMOT_ERR_MLS_CREATE_MESSAGE`, `MARMOT_ERR_MLS_MERGE_COMMIT` |
| -130 to -132 | Group ID Tag | `MARMOT_ERR_MISSING_GROUP_ID_TAG`, `MARMOT_ERR_INVALID_GROUP_ID_FORMAT` |
| -140 to -143 | Image | `MARMOT_ERR_INVALID_IMAGE_HASH_LEN`, `MARMOT_ERR_INVALID_IMAGE_UPLOAD_LEN` |
| -150 to -151 | Media | `MARMOT_ERR_MEDIA_DECRYPT`, `MARMOT_ERR_MEDIA_HASH_MISMATCH` |

---

## Lifecycle

```c
// Create with default configuration
Marmot *marmot_new(MarmotStorage *storage);

// Create with custom configuration
Marmot *marmot_new_with_config(MarmotStorage *storage, const MarmotConfig *config);

// Destroy (also calls storage->destroy)
void marmot_free(Marmot *m);
```

The `Marmot` instance takes ownership of the `MarmotStorage`. When `marmot_free()` is called, it also destroys the storage.

---

## MIP-00: Credentials & KeyPackages

```c
// Create a signed key package (caller provides secret key)
MarmotError marmot_create_key_package(
    Marmot *m,
    const uint8_t nostr_pubkey[32],       // Nostr public key (32 bytes x-only)
    const uint8_t nostr_sk[32],           // Nostr secret key (32 bytes) for signing
    const char **relay_urls,              // Array of relay URL strings
    size_t relay_count,
    MarmotKeyPackageResult *result        // Output: event JSON + ref
);

// Create an unsigned key package (for external signer architectures)
MarmotError marmot_create_key_package_unsigned(
    Marmot *m,
    const uint8_t nostr_pubkey[32],
    const char **relay_urls,
    size_t relay_count,
    MarmotKeyPackageResult *result
);

// Result struct
typedef struct {
    char    *event_json;                   // kind:443 event JSON (caller-owned)
    uint8_t  key_package_ref[32];          // KeyPackageRef identifier
} MarmotKeyPackageResult;

void marmot_key_package_result_free(MarmotKeyPackageResult *result);
```

---

## MIP-01: Group Construction

```c
// Create a new group with initial members
MarmotError marmot_create_group(
    Marmot *m,
    const uint8_t creator_pubkey[32],
    const char **key_package_event_jsons,  // JSON strings of kind:443 events
    size_t kp_count,
    const MarmotGroupConfig *config,       // Name, description, admins, relays
    MarmotCreateGroupResult *result        // Output
);

// Merge a pending self-commit after creating or adding members
MarmotError marmot_merge_pending_commit(Marmot *m, const MarmotGroupId *mls_group_id);

// Add members to an existing group
MarmotError marmot_add_members(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const char **key_package_event_jsons,
    size_t kp_count,
    char ***out_welcome_jsons,             // Output: welcome rumor JSON strings
    size_t *out_welcome_count,
    char **out_commit_json                 // Output: commit event JSON
);

// Remove members from a group
MarmotError marmot_remove_members(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const uint8_t (*member_pubkeys)[32],
    size_t count,
    char **out_commit_json
);

// Leave the group
MarmotError marmot_leave_group(Marmot *m, const MarmotGroupId *mls_group_id);

// Update group metadata (name, description, admins, relays)
MarmotError marmot_update_group_metadata(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const MarmotGroupConfig *config
);

// Group config for creation/update
typedef struct {
    char     *name;
    char     *description;
    uint8_t  (*admin_pubkeys)[32];
    size_t    admin_count;
    char    **relay_urls;
    size_t    relay_count;
} MarmotGroupConfig;

// Result struct
typedef struct {
    MarmotGroup *group;
    char       **welcome_rumor_jsons;      // NIP-59 gift-wrap each for recipients
    size_t       welcome_count;
    char        *evolution_event_json;     // kind:445 commit event to publish
} MarmotCreateGroupResult;

void marmot_create_group_result_free(MarmotCreateGroupResult *result);
```

---

## MIP-02: Welcome Events

```c
// Process a received kind:444 Welcome event
MarmotError marmot_process_welcome(
    Marmot *m,
    const uint8_t wrapper_event_id[32],    // Gift-wrap event ID (32 bytes)
    const char *rumor_event_json,          // Unwrapped kind:444 rumor JSON
    MarmotWelcome **out_welcome            // Output: stored welcome record
);

// Accept or decline a Welcome
MarmotError marmot_accept_welcome(Marmot *m, const MarmotWelcome *welcome);
MarmotError marmot_decline_welcome(Marmot *m, const MarmotWelcome *welcome);

// Get all pending (unprocessed) welcomes
MarmotError marmot_get_pending_welcomes(
    Marmot *m,
    const MarmotPagination *pagination,    // NULL for defaults
    MarmotWelcome ***out_welcomes,
    size_t *out_count
);
```

---

## MIP-03: Group Messages

```c
// Encrypt and package an inner event for group delivery
MarmotError marmot_create_message(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const char *inner_event_json,          // Unsigned event JSON to encrypt
    MarmotOutgoingMessage *result          // Output: kind:445 event
);

// Process a received kind:445 group message
MarmotError marmot_process_message(
    Marmot *m,
    const char *group_event_json,          // kind:445 rumor JSON (after NIP-59 unwrap)
    MarmotMessageResult *result            // Output
);

// Outgoing message result
typedef struct {
    char          *event_json;             // kind:445 event JSON (caller-owned)
    MarmotMessage *message;                // Stored message record
} MarmotOutgoingMessage;

void marmot_outgoing_message_free(MarmotOutgoingMessage *result);

// Processing result
typedef struct {
    MarmotMessageResultType type;

    struct {                               // Valid when type == APPLICATION_MESSAGE
        char *inner_event_json;            // Decrypted inner event JSON
        char *sender_pubkey_hex;           // Verified sender hex pubkey
    } app_msg;

    struct {                               // Valid when type == COMMIT
        MarmotGroup *updated_group;        // Updated group info (may be NULL)
    } commit;
} MarmotMessageResult;

void marmot_message_result_free(MarmotMessageResult *result);
```

---

## MIP-04: Encrypted Media

```c
// Encrypt a file for group sharing
MarmotError marmot_encrypt_media(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const uint8_t *file_data,
    size_t file_len,
    const char *mime_type,                 // e.g., "image/png"
    const char *filename,                  // e.g., "photo.png"
    MarmotEncryptedMedia *result
);

// Decrypt a received encrypted file
MarmotError marmot_decrypt_media(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const uint8_t *encrypted_data,
    size_t enc_len,
    const MarmotImetaInfo *imeta,
    uint8_t **plaintext_out,               // Caller frees with free()
    size_t *plaintext_len
);

// Encrypted media result
typedef struct {
    uint8_t        *encrypted_data;
    size_t          encrypted_len;
    uint8_t         nonce[12];             // ChaCha20-Poly1305 nonce
    uint8_t         file_hash[32];         // SHA-256 of plaintext
    size_t          original_size;         // Original file size
    MarmotImetaInfo imeta;                 // Metadata for group message tags
} MarmotEncryptedMedia;

// Media metadata (from NIP-94 imeta tag)
typedef struct {
    char    *mime_type;
    char    *filename;
    char    *url;
    size_t   original_size;
    uint8_t  file_hash[32];
    uint8_t  nonce[12];
    uint64_t epoch;                        // MLS epoch for key derivation
} MarmotImetaInfo;

void marmot_encrypted_media_clear(MarmotEncryptedMedia *result);
```

---

## Group Queries

```c
// Get a single group by MLS group ID
MarmotError marmot_get_group(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    MarmotGroup **out                      // NULL if not found (still returns OK)
);

// Get all groups
MarmotError marmot_get_all_groups(
    Marmot *m,
    MarmotGroup ***out_groups,
    size_t *out_count
);

// Get messages for a group
MarmotError marmot_get_messages(
    Marmot *m,
    const MarmotGroupId *mls_group_id,
    const MarmotPagination *pagination,    // NULL for defaults
    MarmotMessage ***out_msgs,
    size_t *out_count
);

// Pagination
typedef struct {
    size_t limit;                          // Default: 1000
    size_t offset;                         // Default: 0
    MarmotSortOrder sort_order;            // Default: CREATED_AT_FIRST
} MarmotPagination;

MarmotPagination marmot_pagination_default(void);
```

---

## Storage Interface

The `MarmotStorage` vtable defines 25 operations that any storage backend must implement:

```c
typedef struct MarmotStorage {
    // Groups
    int    (*save_group)(void *ctx, const MarmotGroup *group);
    MarmotGroup *(*find_group)(void *ctx, const MarmotGroupId *mls_group_id);
    MarmotGroup *(*find_group_by_nostr_id)(void *ctx, const char *nostr_group_id);
    MarmotGroup **(*all_groups)(void *ctx, size_t *count);
    int    (*update_group_relays)(void *ctx, const MarmotGroupId *id,
                                  const char * const *urls, size_t count);

    // Messages
    int    (*save_message)(void *ctx, const MarmotMessage *msg);
    MarmotMessage *(*find_message)(void *ctx, const char *id);
    MarmotMessage **(*messages_for_group)(void *ctx, const MarmotGroupId *id,
                                          const MarmotPagination *page, size_t *count);
    MarmotMessage *(*last_message)(void *ctx, const MarmotGroupId *id);
    bool   (*is_message_processed)(void *ctx, const char *event_id);

    // Welcomes
    int    (*save_welcome)(void *ctx, const MarmotWelcome *welcome);
    MarmotWelcome *(*find_welcome)(void *ctx, const char *id);
    MarmotWelcome **(*pending_welcomes)(void *ctx, size_t *count);
    bool   (*is_welcome_processed)(void *ctx, const char *event_id);

    // MLS key-value store (for internal MLS state)
    int    (*mls_store)(void *ctx, const char *label, const uint8_t *key,
                         size_t key_len, const uint8_t *value, size_t value_len);
    int    (*mls_load)(void *ctx, const char *label, const uint8_t *key,
                        size_t key_len, uint8_t **value, size_t *value_len);
    int    (*mls_delete)(void *ctx, const char *label, const uint8_t *key, size_t key_len);

    // Exporter secrets (per-group per-epoch)
    int    (*save_exporter_secret)(void *ctx, const MarmotGroupId *id,
                                    uint64_t epoch, const uint8_t secret[32]);
    int    (*get_exporter_secret)(void *ctx, const MarmotGroupId *id,
                                   uint64_t epoch, uint8_t secret_out[32]);

    // Snapshots (for commit race resolution)
    int    (*create_snapshot)(void *ctx, const MarmotGroupId *id, const char *name);
    int    (*rollback_snapshot)(void *ctx, const MarmotGroupId *id, const char *name);
    int    (*delete_snapshot)(void *ctx, const MarmotGroupId *id, const char *name);

    // Metadata
    bool   (*is_persistent)(void *ctx);

    // Lifecycle
    void   (*destroy)(void *ctx);
    void   *ctx;    // Opaque backend pointer
} MarmotStorage;
```

---

## Storage Backends

### In-Memory

```c
MarmotStorage *marmot_storage_memory_new(void);
```

No dependencies. All data lost on process exit. Ideal for testing.

### SQLite

```c
MarmotStorage *marmot_storage_sqlite_new(const char *db_path, const char *encryption_key);
```

Requires `SQLite3`. The `encryption_key` parameter enables SQLCipher encryption if SQLCipher is installed; pass `NULL` for unencrypted storage.

### nostrdb

```c
MarmotStorage *marmot_storage_nostrdb_new(struct ndb *ndb, void *lmdb_env);
```

Hybrid backend: Nostr events stored in nostrdb (LMDB), MLS state in a separate LMDB database. Shares the nostrdb instance with the gnostr application.

---

## Configuration

```c
typedef struct {
    uint64_t max_event_age_secs;      // Default: 3888000 (45 days)
    uint64_t max_future_skew_secs;    // Default: 300 (5 minutes)
    uint32_t out_of_order_tolerance;  // Default: 100
    uint32_t max_forward_distance;    // Default: 1000
    uint32_t epoch_snapshot_retention; // Default: 5
    uint64_t snapshot_ttl_seconds;    // Default: 604800 (1 week)
} MarmotConfig;

MarmotConfig marmot_config_default(void);
```

All defaults match MDK's `MdkConfig` exactly.

---

## GObject Wrapper (marmot-gobject)

### Types

| C (libmarmot) | GObject | GIR Namespace |
|---------------|---------|---------------|
| `Marmot` | `MarmotGobjectClient` | `Marmot.Client` |
| `MarmotGroup` | `MarmotGobjectGroup` | `Marmot.Group` |
| `MarmotMessage` | `MarmotGobjectMessage` | `Marmot.Message` |
| `MarmotWelcome` | `MarmotGobjectWelcome` | `Marmot.Welcome` |
| `MarmotStorage` | `MarmotGobjectStorage` (GInterface) | `Marmot.Storage` |
| `MarmotGroupState` | `MarmotGobjectGroupState` (GEnum) | `Marmot.GroupState` |
| `MarmotMessageState` | `MarmotGobjectMessageState` (GEnum) | `Marmot.MessageState` |
| `MarmotWelcomeState` | `MarmotGobjectWelcomeState` (GEnum) | `Marmot.WelcomeState` |

### Async API

All operations that touch storage or crypto use `GTask`:

```c
// Async create group
void marmot_gobject_client_create_group_async(
    MarmotGobjectClient *client,
    const char * const *member_pubkeys,
    gsize n_members,
    const char *name,
    const char *description,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer user_data
);
MarmotGobjectGroup *marmot_gobject_client_create_group_finish(
    MarmotGobjectClient *client,
    GAsyncResult *result,
    GError **error
);

// Async process welcome
void marmot_gobject_client_process_welcome_async(...);
MarmotGobjectWelcome *marmot_gobject_client_process_welcome_finish(...);

// Async send message
void marmot_gobject_client_send_message_async(...);
MarmotGobjectMessage *marmot_gobject_client_send_message_finish(...);
```

### Signals

| Signal | Handler Signature | Emitted When |
|--------|------------------|-------------|
| `group-joined` | `void (*)(MarmotGobjectClient*, MarmotGobjectGroup*)` | A Welcome is accepted |
| `message-received` | `void (*)(MarmotGobjectClient*, MarmotGobjectMessage*)` | A group message is decrypted |
| `welcome-received` | `void (*)(MarmotGobjectClient*, MarmotGobjectWelcome*)` | A Welcome event arrives |

### Storage Backends (GObject)

```c
// In-memory backend
MarmotGobjectStorageMemory *marmot_gobject_storage_memory_new(void);

// SQLite backend
MarmotGobjectStorageSqlite *marmot_gobject_storage_sqlite_new(
    const char *db_path, const char *encryption_key);

// Access the raw C storage pointer
MarmotStorage *marmot_gobject_storage_get_raw_storage(MarmotGobjectStorage *storage);
```

### Language Bindings

**Python** (via GObject Introspection):
```python
from gi.repository import Marmot

storage = Marmot.StorageMemory.new()
client = Marmot.Client.new(storage)
client.connect("message-received", on_message)
```

**Vala** (via VAPI):
```vala
var storage = new Marmot.StorageMemory();
var client = new Marmot.Client(storage);
client.message_received.connect((msg) => {
    print("Message from %s: %s\n", msg.sender_pubkey, msg.content);
});
```

**JavaScript** (via GJS):
```javascript
const { Marmot } = imports.gi;

let storage = new Marmot.StorageMemory();
let client = new Marmot.Client({ storage });
client.connect('message-received', (client, msg) => {
    log(`Message: ${msg.content}`);
});
```

---

## Thread Safety

- `Marmot` instances are NOT thread-safe. All calls to a single instance must be serialized.
- Different `Marmot` instances (with separate storage backends) can be used concurrently from different threads.
- `MarmotGobjectClient` serializes access internally via `GTask` — all async operations are safe to call from any thread.
- Storage backends:
  - **Memory**: Not thread-safe (use one instance per thread, or synchronize externally)
  - **SQLite**: Thread-safe via SQLite's internal locking (`SQLITE_THREADSAFE=1`)
  - **nostrdb**: Thread-safe via LMDB's MVCC architecture

---

## Related Documentation

- [ARCHITECTURE.md](../ARCHITECTURE.md) — Full nostrc architecture overview
- [libmarmot/README.md](../libmarmot/README.md) — Build instructions and overview
- [marmot-gobject/](../marmot-gobject/) — GObject wrapper source
- [Marmot Protocol Spec](https://github.com/marmot-org/marmot) — MIP-00 through MIP-05
- [RFC 9420](https://www.rfc-editor.org/rfc/rfc9420) — The Messaging Layer Security Protocol
- [MDK](https://github.com/marmot-org/mdk) — Rust reference implementation
