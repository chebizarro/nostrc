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
32-byte MLS group identifier. Used as the primary key for all group operations.

```c
typedef struct {
    uint8_t bytes[MARMOT_GROUP_ID_LEN];  // 32 bytes
} MarmotGroupId;

MarmotGroupId  marmot_group_id_from_bytes(const uint8_t bytes[32]);
MarmotGroupId  marmot_group_id_from_hex(const char *hex);
char          *marmot_group_id_to_hex(const MarmotGroupId *id);  // caller frees
bool           marmot_group_id_equal(const MarmotGroupId *a, const MarmotGroupId *b);
```

### `MarmotGroup`
Represents a Marmot group with its Nostr and MLS identifiers plus metadata.

```c
typedef struct {
    MarmotGroupId  mls_group_id;      // 32-byte MLS group ID
    char          *nostr_group_id;    // Hex-encoded Nostr group ID (h-tag)
    char          *name;              // Group name (nullable)
    char          *description;       // Group description (nullable)
    char         **admin_pubkeys;     // Array of hex pubkeys
    size_t         admin_count;
    char         **relay_urls;        // Group relay URLs
    size_t         relay_count;
    MarmotGroupState state;           // ACTIVE, INACTIVE, PENDING
    uint64_t       epoch;             // Current MLS epoch number
    int64_t        created_at;        // Unix timestamp
} MarmotGroup;
```

### `MarmotMessage`
Represents a decrypted group message.

```c
typedef struct {
    char            *id;              // Message ID (event ID)
    MarmotGroupId    group_id;        // Source group
    char            *sender_pubkey;   // Hex sender pubkey
    char            *content;         // Decrypted content (nullable)
    int              kind;            // Inner event kind
    int64_t          created_at;      // Unix timestamp
    MarmotMessageState state;         // CREATED, DELIVERED, DECRYPTED, FAILED
} MarmotMessage;
```

### `MarmotWelcome`
Represents a received Welcome event (group invitation).

```c
typedef struct {
    char            *id;              // Welcome event ID
    MarmotGroupId    group_id;        // Target group
    char            *sender_pubkey;   // Inviter pubkey
    char            *group_name;      // Preview name (nullable)
    char            *group_description; // Preview description (nullable)
    uint32_t         member_count;    // Estimated member count
    MarmotWelcomeState state;         // PENDING, ACCEPTED, DECLINED
    int64_t          created_at;
} MarmotWelcome;
```

### Enums

```c
typedef enum {
    MARMOT_GROUP_ACTIVE,
    MARMOT_GROUP_INACTIVE,
    MARMOT_GROUP_PENDING,
} MarmotGroupState;

typedef enum {
    MARMOT_MSG_CREATED,
    MARMOT_MSG_DELIVERED,
    MARMOT_MSG_DECRYPTED,
    MARMOT_MSG_FAILED,
} MarmotMessageState;

typedef enum {
    MARMOT_WELCOME_PENDING,
    MARMOT_WELCOME_ACCEPTED,
    MARMOT_WELCOME_DECLINED,
} MarmotWelcomeState;

typedef enum {
    MARMOT_MSG_APPLICATION,      // Decrypted application message
    MARMOT_MSG_COMMIT,           // Group state change (epoch advance)
    MARMOT_MSG_PROPOSAL,         // Group change proposal
    MARMOT_MSG_UNPROCESSABLE,    // Too old / wrong epoch
    MARMOT_MSG_OWN_MESSAGE,      // Our own message (skip)
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

All functions returning `int` use the convention: `0 = success`, negative = error.

```c
int rc = marmot_create_group(m, ...);
if (rc != 0) {
    fprintf(stderr, "Error: %s\n", marmot_error_string(rc));
}
```

### Error Categories

| Range | Category | Examples |
|-------|----------|---------|
| 0 | Success | `MARMOT_OK` |
| -1 to -9 | General | `MARMOT_ERR_INVALID_ARG`, `MARMOT_ERR_NOT_FOUND`, `MARMOT_ERR_NOT_IMPLEMENTED` |
| -10 to -19 | Storage | `MARMOT_ERR_STORAGE_READ`, `MARMOT_ERR_STORAGE_WRITE` |
| -20 to -29 | MLS | `MARMOT_ERR_MLS_LIBRARY`, `MARMOT_ERR_MLS_STATE` |
| -30 to -39 | Group | `MARMOT_ERR_GROUP_NOT_FOUND`, `MARMOT_ERR_NOT_MEMBER`, `MARMOT_ERR_NOT_ADMIN` |
| -40 to -49 | Welcome | `MARMOT_ERR_WELCOME_EXPIRED`, `MARMOT_ERR_WELCOME_DECRYPTION` |
| -50 to -59 | Message | `MARMOT_ERR_MESSAGE_DECRYPTION`, `MARMOT_ERR_MESSAGE_EPOCH` |
| -60 to -69 | Crypto | `MARMOT_ERR_CRYPTO_SIGN`, `MARMOT_ERR_CRYPTO_VERIFY` |
| -70 to -79 | TLS codec | `MARMOT_ERR_TLS_CODEC`, `MARMOT_ERR_TLS_TOO_SHORT` |
| -90 to -99 | Serialization | `MARMOT_ERR_DESERIALIZATION`, `MARMOT_ERR_VALIDATION` |
| -140 to -149 | Image/Media | `MARMOT_ERR_IMAGE_TOO_LARGE`, `MARMOT_ERR_MEDIA_DECRYPT` |

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
// Create a new key package for the given Nostr identity
int marmot_create_key_package(
    Marmot *m,
    const uint8_t nostr_pubkey[32],       // Nostr public key (32 bytes)
    const char * const *relay_urls,        // Array of relay URLs
    size_t relay_count,
    MarmotKeyPackageResult *result         // Output: event + ref
);

// Result struct
typedef struct {
    NostrEvent *event;                     // Unsigned kind:443 event
    uint8_t key_package_ref[32];           // KeyPackageRef identifier
} MarmotKeyPackageResult;

void marmot_key_package_result_clear(MarmotKeyPackageResult *result);
```

---

## MIP-01: Group Construction

```c
// Create a new group with initial members
int marmot_create_group(
    Marmot *m,
    const uint8_t creator_pubkey[32],
    NostrEvent **key_package_events,       // Array of kind:443 events
    size_t member_count,
    const MarmotGroupConfig *config,       // Name, description, admins, relays
    MarmotGroupResult *result              // Output
);

// Merge a pending self-commit after publishing
int marmot_merge_pending_commit(Marmot *m, const MarmotGroupId *group_id);

// Leave the group (publishes remove proposal + commit)
int marmot_leave_group(Marmot *m, const MarmotGroupId *group_id);

// Result struct
typedef struct {
    MarmotGroup group;
    NostrEvent **welcome_rumors;           // NIP-59 gift-wrap each for recipients
    size_t welcome_count;
    NostrEvent *evolution_event;           // kind:445 commit event to publish
} MarmotGroupResult;

void marmot_group_result_clear(MarmotGroupResult *result);
```

---

## MIP-02: Welcome Events

```c
// Process a received kind:444 Welcome event
int marmot_process_welcome(
    Marmot *m,
    const char *event_id_hex,              // Welcome event ID
    NostrEvent *welcome_rumor,             // Unwrapped NIP-59 rumor
    MarmotWelcomePreview *preview          // Output: group info preview
);

// Accept or decline a Welcome
int marmot_accept_welcome(Marmot *m, const MarmotWelcomePreview *preview);
int marmot_decline_welcome(Marmot *m, const char *welcome_id);

// Get all pending (unprocessed) welcomes
int marmot_get_pending_welcomes(Marmot *m, MarmotWelcome ***welcomes, size_t *count);
```

---

## MIP-03: Group Messages

```c
// Encrypt and package an inner event for group delivery
int marmot_create_message(
    Marmot *m,
    const MarmotGroupId *group_id,
    NostrEvent *inner_event,               // The actual content event (unsigned)
    MarmotOutgoingMessage *result          // Output: kind:445 event
);

// Process a received kind:445 group message
int marmot_process_message(
    Marmot *m,
    NostrEvent *group_event,               // Received kind:445 event
    MarmotMessageResult *result            // Output
);

// Result struct
typedef struct {
    MarmotMessageResultType type;
    NostrEvent *inner_event;               // Decrypted inner event (if APPLICATION)
    char *sender_pubkey;                   // Verified sender (if APPLICATION)
} MarmotMessageResult;

void marmot_message_result_clear(MarmotMessageResult *result);
```

---

## MIP-04: Encrypted Media

```c
// Encrypt a file for group sharing
int marmot_encrypt_media(
    Marmot *m,
    const MarmotGroupId *group_id,
    const uint8_t *file_data,
    size_t file_len,
    const char *mime_type,                 // e.g., "image/png"
    const char *filename,                  // e.g., "photo.png"
    MarmotEncryptedMedia *result
);

// Decrypt a received encrypted file
int marmot_decrypt_media(
    Marmot *m,
    const MarmotGroupId *group_id,
    const uint8_t *encrypted_data,
    size_t enc_len,
    const MarmotImetaInfo *imeta,          // Metadata from group message
    uint8_t **out_data,                    // Caller frees with free()
    size_t *out_len
);

// Result struct
typedef struct {
    uint8_t *encrypted_data;
    size_t   encrypted_len;
    uint8_t  nonce[24];                    // ChaCha20-Poly1305 nonce
    uint8_t  file_hash[32];               // SHA-256 of plaintext
    MarmotImetaInfo imeta;                // Metadata for group message tags
} MarmotEncryptedMedia;

void marmot_encrypted_media_clear(MarmotEncryptedMedia *result);
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
    size_t   epoch_snapshot_retention; // Default: 5
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
