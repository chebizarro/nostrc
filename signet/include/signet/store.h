/* SPDX-License-Identifier: MIT
 *
 * store.h - SQLCipher-backed persistent store for Signet.
 *
 * Provides AES-256-encrypted storage for agent secret keys and metadata.
 * Secret keys are envelope-encrypted at rest using crypto_secretbox_easy
 * with a per-record random nonce and a data-encryption key derived from the
 * master key (SIGNET_DB_KEY env var) via keyed BLAKE2b (crypto_generichash)
 * with domain separation. SIGNET_DB_KEY may be supplied as 64/128-char hex, a
 * base64-encoded 32/64-byte key, or a raw ASCII passphrase (>= 32 bytes).
 *
 * Schema:
 *   agents(agent_id TEXT PRIMARY KEY,
 *          encrypted_nsec BLOB NOT NULL,
 *          nonce BLOB NOT NULL,
 *          algo TEXT NOT NULL DEFAULT 'xsalsa20poly1305',
 *          connect_secret TEXT,
 *          created_at INTEGER NOT NULL,
 *          last_used INTEGER NOT NULL DEFAULT 0)
 */

#ifndef SIGNET_STORE_H
#define SIGNET_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * SignetStore:
 * Opaque handle for the SQLCipher-backed Signet persistent store.
 *
 * Since: 1.0
 */
typedef struct SignetStore SignetStore;

/**
 * SignetStoreConfig:
 * @db_path: path to SQLCipher database file.
 * @master_key: master key (hex or base64; min 32 bytes entropy).
 * @read_only: open read-only for introspection; skips schema creation/migration
 *   and auto-migration, sets PRAGMA query_only, and never takes write locks, so
 *   it can coexist with a live daemon that already holds the DB open.
 *
 * Configuration used to open a Signet persistent store.
 *
 * Since: 1.0
 */
typedef struct {
  const char *db_path;     /* path to SQLCipher database file */
  const char *master_key;  /* master key (hex or base64; min 32 bytes entropy) */
  bool read_only;          /* open read-only (introspection); no schema writes */
} SignetStoreConfig;

/* Agent record returned from the store. Caller must free with signet_agent_record_clear(). */
/**
 * SignetAgentRecord:
 * @agent_id: (transfer full): agent identifier.
 * @secret_key: (array length=secret_key_len) (transfer full): decrypted 32-byte secret key in locked heap memory.
 * @secret_key_len: length of @secret_key in bytes; 32 on success.
 * @connect_secret: (transfer full) (nullable): one-time connect secret, or %NULL after consumption.
 * @created_at: creation time as Unix seconds.
 * @last_used: last-use time as Unix seconds, or 0 if never used.
 *
 * Decrypted agent record returned from persistent storage.
 * Clear with signet_agent_record_clear() to wipe the secret key.
 *
 * Since: 1.0
 */
typedef struct {
  char *agent_id;
  uint8_t *secret_key;       /* decrypted 32-byte secret key (heap, mlock'd) */
  size_t secret_key_len;     /* always 32 on success */
  char *connect_secret;      /* one-time auth secret (heap, may be NULL after use) */
  int64_t created_at;
  int64_t last_used;
} SignetAgentRecord;

/**
 * SignetAgentMeta:
 * @agent_id: (transfer full): agent identifier.
 * @pubkey: (transfer full) (nullable): 64-hex public key, or %NULL for legacy
 *   rows created before the pubkey column existed.
 * @provenance: (transfer full) (nullable): "provisioned", "adopted", or
 *   "rotated".
 * @created_at: creation time as Unix seconds.
 * @last_used: last-use time as Unix seconds, or 0 if never used.
 *
 * Non-secret agent metadata. Unlike #SignetAgentRecord this is read WITHOUT
 * decrypting the custody key, so it renders even when the secret cannot be
 * decrypted (e.g. wrong/rotated DEK or a corrupt blob). Clear with
 * signet_agent_meta_clear().
 *
 * Since: 1.1
 */
typedef struct {
  char *agent_id;
  char *pubkey;
  char *provenance;
  int64_t created_at;
  int64_t last_used;
} SignetAgentMeta;

/* Open (or create) the store. Returns NULL on failure. */
/**
 * signet_store_open:
 * @cfg: (nullable): configuration to use
 *
 * Open (or create) the store. Returns NULL on failure.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetStore *signet_store_open(const SignetStoreConfig *cfg);

/* Close and free the store. Safe on NULL. */
/**
 * signet_store_close:
 * @store: (nullable): a #SignetStore
 *
 * Closes and frees a store, wiping process-owned secret material.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Since: 1.0
 */
void signet_store_close(SignetStore *store);

/* Check if the store is open and usable. */
/**
 * signet_store_is_open:
 * @store: (nullable): a #SignetStore
 *
 * Checks whether a store handle is open and usable.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: %true if the store is open, otherwise %false
 *
 * Since: 1.0
 */
bool signet_store_is_open(const SignetStore *store);

/* True if the underlying database is SQLCipher-encrypted at rest (i.e. the
 * build linked SQLCipher and PRAGMA key took effect). False means the DB is
 * plain SQLite and only per-record envelope encryption is protecting secrets. */
bool signet_store_is_encrypted(const SignetStore *store);

/* True if the file at db_path is a legacy plaintext SQLite database (detected by
 * the on-disk "SQLite format 3\0" magic header). SQLCipher databases and
 * empty/absent files return false. */
bool signet_store_file_is_plaintext_sqlite(const char *db_path);

/* True if this build's sqlite3 is actually SQLCipher (so encryption/migration
 * are possible). */
bool signet_store_sqlcipher_available(void);

/* Migrate a legacy plaintext SQLite database at db_path to a SQLCipher-encrypted
 * database keyed by master_key. Uses SQLCipher's ATTACH + sqlcipher_export; on
 * success the original plaintext file is renamed to "<db_path>.plaintext-backup"
 * and db_path becomes the new encrypted database.
 *
 * Returns 0 on success, 1 if db_path is not a plaintext SQLite file (nothing to
 * migrate), or -1 on error (SQLCipher unavailable, or migration failure with the
 * original left intact). signet_store_open() performs this automatically for
 * legacy databases unless SIGNET_MIGRATE_PLAINTEXT_DB=false. */
int signet_store_migrate_plaintext_to_sqlcipher(const char *db_path,
                                                const char *master_key);

/* Store a new agent key. secret_key must be 32 bytes.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_put_agent:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @secret_key: (not nullable): secret key
 * @secret_key_len: length of @secret_key in bytes
 * @connect_secret: (not nullable): connect secret
 * @now: current Unix time in seconds
 *
 * Stores a new agent custody key and optional one-time connect secret.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_put_agent(SignetStore *store,
                           const char *agent_id,
                           const uint8_t *secret_key,
                           size_t secret_key_len,
                           const char *connect_secret,
                           int64_t now);

/* Like signet_store_put_agent but also records the agent's x-only pubkey hex
 * (for adopt/collision checks) and provenance ("provisioned"|"adopted"|"rotated";
 * NULL defaults to "provisioned"). Returns 0 on success, -1 on error. */
int signet_store_put_agent_ex(SignetStore *store,
                              const char *agent_id,
                              const uint8_t *secret_key,
                              size_t secret_key_len,
                              const char *connect_secret,
                              const char *pubkey_hex,
                              const char *provenance,
                              int64_t now);

/* Report whether pubkey_hex is already bound to some agent (optionally excluding
 * exclude_agent_id). Only detects agents whose pubkey column is populated.
 * Returns 0 on success (sets *out_in_use), -1 on error. */
int signet_store_pubkey_in_use(SignetStore *store,
                               const char *pubkey_hex,
                               const char *exclude_agent_id,
                               bool *out_in_use);

/* Retrieve and decrypt an agent key.
 * Returns 0 on success, 1 if not found, -1 on error. */
/**
 * signet_store_get_agent:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @out_record: (out) (not nullable): return location for record
 *
 * Looks up and decrypts an agent custody key.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_get_agent(SignetStore *store,
                           const char *agent_id,
                           SignetAgentRecord *out_record);

/**
 * signet_store_get_agent_meta:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @out_meta: (out) (not nullable): return location for the metadata
 *
 * Reads non-secret agent metadata (agent_id, pubkey, provenance, timestamps)
 * WITHOUT decrypting the custody key. Use this for listing/introspection so a
 * row still renders when its secret cannot be decrypted.
 *
 * Returns: 0 on success, 1 if not found, -1 on error.
 *
 * Since: 1.1
 */
int signet_store_get_agent_meta(SignetStore *store,
                                const char *agent_id,
                                SignetAgentMeta *out_meta);

/**
 * signet_agent_meta_clear:
 * @meta: (nullable): metadata to clear
 *
 * Frees the strings owned by a #SignetAgentMeta and zeroes the struct.
 *
 * Since: 1.1
 */
void signet_agent_meta_clear(SignetAgentMeta *meta);

/* Delete an agent from the store.
 * Returns 0 on success, 1 if not found, -1 on error. */
/**
 * signet_store_delete_agent:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 *
 * Deletes an agent record from persistent storage.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_delete_agent(SignetStore *store, const char *agent_id);

/**
 * signet_store_free_agent_ids:
 * @store: (nullable): a #SignetStore
 * @out_ids: (out) (transfer full) (not nullable) (array): return location for ids
 * @out_count: (out) (not nullable): return location for the number of elements
 *
 * Frees an agent identifier vector returned by signet_store_list_agents().
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: Caller frees with result
 *
 * Since: 1.0
 */
/* List all agent IDs. Caller frees with signet_store_free_agent_ids().
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_list_agents:
 * @store: (not nullable): a #SignetStore
 * @out_ids: (out) (transfer full) (not nullable): return location for a newly allocated string vector
 * @out_count: (out) (not nullable): number of elements
 *
 * List all agent IDs. Caller frees with signet_store_free_agent_ids(). Returns 0 on success, -1 on error.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_list_agents(SignetStore *store,
                             char ***out_ids,
                             size_t *out_count);

/* Update the last_used timestamp for an agent.
 * Returns 0 on success, -1 on error. */
/**
 * signet_store_touch_agent:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 * @now: current Unix time in seconds
 *
 * Updates an agent record last-used timestamp.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_touch_agent(SignetStore *store,
                             const char *agent_id,
                             int64_t now);

/* Consume (clear) the connect_secret for an agent after successful connect.
 * Returns 0 on success, 1 if not found or already consumed, -1 on error. */
/**
 * signet_store_consume_connect_secret:
 * @store: (nullable): a #SignetStore
 * @agent_id: (not nullable): agent identifier
 *
 * Clears an agent connect secret after successful connection.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_consume_connect_secret(SignetStore *store,
                                        const char *agent_id);

/* Resolve an agent by connect_secret and consume that secret atomically.
 * If the connect_secret was previously bound to a bootstrap token handoff,
 * the matching bootstrap token must also still be active and will be marked
 * used in the same transaction.
 * On success, returns 0 and sets *out_agent_id to a newly allocated string
 * owned by the caller (g_free). Returns 1 if not found/expired, -1 on error. */
/**
 * signet_store_consume_connect_secret_value:
 * @store: (nullable): a #SignetStore
 * @connect_secret: (not nullable): connect secret
 * @now: current Unix time in seconds
 * @out_agent_id: (out) (transfer full) (not nullable): return location for agent id
 *
 * Resolves an agent by one-time connect secret and consumes it atomically.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_store_consume_connect_secret_value(SignetStore *store,
                                              const char *connect_secret,
                                              int64_t now,
                                              char **out_agent_id);

/* Free an agent record (wipes secret key). Safe on NULL. */
/**
 * signet_agent_record_clear:
 * @rec: (nullable): rec
 *
 * Clears an agent record and wipes its decrypted key.
 *
 * Since: 1.0
 */
void signet_agent_record_clear(SignetAgentRecord *rec);

/* Free an agent ID list. Safe on NULL. */
/**
 * signet_store_free_agent_ids:
 * @ids: (not nullable) (array): ids
 * @count: count
 *
 * Frees an agent identifier vector returned by signet_store_list_agents().
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Since: 1.0
 */
void signet_store_free_agent_ids(char **ids, size_t count);

/* Get the underlying sqlite3 handle for use by sub-stores.
 * Returns NULL if store is not open. The handle is owned by the store. */
/**
 * signet_store_get_db:
 * @store: (not nullable): a #SignetStore
 *
 * Get the underlying sqlite3 handle for use by sub-stores. Returns NULL if store is not open. The handle is owned by the store.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
struct sqlite3 *signet_store_get_db(SignetStore *store);

/* Get the data-encryption key for envelope encryption by sub-stores.
 * Returns NULL if store is not open. The pointer is mlock'd and owned by the store.
 * Caller MUST NOT free or modify. */
/**
 * signet_store_get_dek:
 * @store: (not nullable): a #SignetStore
 *
 * Get the data-encryption key for envelope encryption by sub-stores. Returns NULL if store is not open. The pointer is mlock'd and owned by the store. Caller MUST NOT free or modify.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
const uint8_t *signet_store_get_dek(const SignetStore *store);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_H */
