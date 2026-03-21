/* SPDX-License-Identifier: MIT
 *
 * store.h - SQLCipher-backed persistent store for Signet.
 *
 * Provides AES-256-encrypted storage for agent secret keys and metadata.
 * Secret keys are envelope-encrypted at rest using crypto_secretbox_easy
 * with a per-agent nonce and a data-encryption key derived via HKDF from
 * the master key (SIGNET_DB_KEY env var).
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

typedef struct SignetStore SignetStore;

typedef struct {
  const char *db_path;     /* path to SQLCipher database file */
  const char *master_key;  /* master key (hex or base64; min 32 bytes entropy) */
} SignetStoreConfig;

/* Agent record returned from the store. Caller must free with signet_agent_record_clear(). */
typedef struct {
  char *agent_id;
  uint8_t *secret_key;       /* decrypted 32-byte secret key (heap, mlock'd) */
  size_t secret_key_len;     /* always 32 on success */
  char *connect_secret;      /* one-time auth secret (heap, may be NULL after use) */
  int64_t created_at;
  int64_t last_used;
} SignetAgentRecord;

/* Open (or create) the store. Returns NULL on failure. */
SignetStore *signet_store_open(const SignetStoreConfig *cfg);

/* Close and free the store. Safe on NULL. */
void signet_store_close(SignetStore *store);

/* Check if the store is open and usable. */
bool signet_store_is_open(const SignetStore *store);

/* Store a new agent key. secret_key must be 32 bytes.
 * Returns 0 on success, -1 on error. */
int signet_store_put_agent(SignetStore *store,
                           const char *agent_id,
                           const uint8_t *secret_key,
                           size_t secret_key_len,
                           const char *connect_secret,
                           int64_t now);

/* Retrieve and decrypt an agent key.
 * Returns 0 on success, 1 if not found, -1 on error. */
int signet_store_get_agent(SignetStore *store,
                           const char *agent_id,
                           SignetAgentRecord *out_record);

/* Delete an agent from the store.
 * Returns 0 on success, 1 if not found, -1 on error. */
int signet_store_delete_agent(SignetStore *store, const char *agent_id);

/* List all agent IDs. Caller frees with signet_store_free_agent_ids().
 * Returns 0 on success, -1 on error. */
int signet_store_list_agents(SignetStore *store,
                             char ***out_ids,
                             size_t *out_count);

/* Update the last_used timestamp for an agent.
 * Returns 0 on success, -1 on error. */
int signet_store_touch_agent(SignetStore *store,
                             const char *agent_id,
                             int64_t now);

/* Consume (clear) the connect_secret for an agent after successful connect.
 * Returns 0 on success, 1 if not found or already consumed, -1 on error. */
int signet_store_consume_connect_secret(SignetStore *store,
                                        const char *agent_id);

/* Resolve an agent by connect_secret and consume that secret atomically.
 * On success, returns 0 and sets *out_agent_id to a newly allocated string
 * owned by the caller (g_free). Returns 1 if not found, -1 on error. */
int signet_store_consume_connect_secret_value(SignetStore *store,
                                              const char *connect_secret,
                                              char **out_agent_id);

/* Free an agent record (wipes secret key). Safe on NULL. */
void signet_agent_record_clear(SignetAgentRecord *rec);

/* Free an agent ID list. Safe on NULL. */
void signet_store_free_agent_ids(char **ids, size_t count);

/* Get the underlying sqlite3 handle for use by sub-stores.
 * Returns NULL if store is not open. The handle is owned by the store. */
struct sqlite3 *signet_store_get_db(SignetStore *store);

/* Get the data-encryption key for envelope encryption by sub-stores.
 * Returns NULL if store is not open. The pointer is mlock'd and owned by the store.
 * Caller MUST NOT free or modify. */
const uint8_t *signet_store_get_dek(const SignetStore *store);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_STORE_H */
