/* SPDX-License-Identifier: MIT
 *
 * key_store.h - Key custody interface for Signet.
 *
 * Provides signing keys from an mlock'd in-process cache backed by
 * SQLCipher persistence. Keys are loaded into the hot cache at startup
 * and accessed via pointer dereference on the signing hot path.
 */

#ifndef SIGNET_KEY_STORE_H
#define SIGNET_KEY_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * SignetKeyStore:
 * Opaque hot key-cache and persistent key-store handle.
 *
 * Since: 1.0
 */
typedef struct SignetKeyStore SignetKeyStore;

struct SignetAuditLogger;
struct SignetStore;

/**
 * SignetLoadedKey:
 * @secret_key: 32-byte secret key (heap, should be in locked memory).
 * @secret_key_len: always 32 on success.
 * @loaded_at: unix seconds.
 * @expires_at: unix seconds; 0 means "no expiry".
 *
 * A decrypted custody key copied from the hot key cache.
 *
 * Ownership: clear instances with the corresponding *_clear() function to release heap data and wipe secrets where applicable.
 *
 * Since: 1.0
 */
typedef struct {
  uint8_t *secret_key;     /* 32-byte secret key (heap, should be in locked memory) */
  size_t secret_key_len;   /* always 32 on success */
  int64_t loaded_at;       /* unix seconds */
  int64_t expires_at;      /* unix seconds; 0 means "no expiry" */
} SignetLoadedKey;

/**
 * SignetKeyStoreConfig:
 * @db_path: SQLCipher database path.
 * @master_key: master key for SQLCipher + envelope encryption.
 *
 * Configuration for opening a #SignetKeyStore.
 *
 * Since: 1.0
 */
typedef struct {
  const char *db_path;     /* SQLCipher database path */
  const char *master_key;  /* master key for SQLCipher + envelope encryption */
} SignetKeyStoreConfig;

/* Create key store backed by SQLCipher.
 * Opens (or creates) the database and loads all agent keys into the hot cache.
 * Returns NULL on failure. */
/**
 * signet_key_store_new:
 * @audit: (not nullable): audit
 * @cfg: (nullable): configuration to use
 *
 * Create key store backed by SQLCipher. Opens (or creates) the database and loads all agent keys into the hot cache. Returns NULL on failure.
 *
 * Returns: (transfer full) (nullable): a newly allocated object, or %NULL on failure
 *
 * Since: 1.0
 */
SignetKeyStore *signet_key_store_new(struct SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg);

/* Free key store, wiping all cached keys. Safe on NULL. */
/**
 * signet_key_store_free:
 * @ks: (nullable): a #SignetKeyStore
 *
 * Free key store, wiping all cached keys. Safe on NULL.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Since: 1.0
 */
void signet_key_store_free(SignetKeyStore *ks);

/* Load the custody key for an agent from the hot cache.
 * Returns true on success (key found), false if not found. */
/**
 * signet_key_store_load_agent_key:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 * @out_key: (out) (not nullable): return location for key
 *
 * Load the custody key for an agent from the hot cache. Returns true on success (key found), false if not found.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_key_store_load_agent_key(SignetKeyStore *ks,
                                     const char *agent_id,
                                     SignetLoadedKey *out_key);

/* Provision a new agent key. Generates a new keypair, stores in SQLCipher,
 * and adds to the hot cache.
 * out_pubkey_hex must be at least 65 bytes.
 * If bunker_pubkey_hex and relay_urls are provided, a bunker:// URI is built and
 * returned via *out_bunker_uri (caller frees with g_free). out_bunker_uri may be
 * NULL if not needed. Returns 0 on success, -1 on error. */
/**
 * signet_key_store_provision_agent:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 * @bunker_pubkey_hex: (not nullable): bunker pubkey hex
 * @relay_urls: (nullable): relay urls
 * @n_relay_urls: number of elements
 * @out_pubkey_hex: (out) (not nullable): return location for pubkey hex
 * @out_pubkey_hex_sz: (out): return location for pubkey hex sz
 * @out_bunker_uri: (out) (transfer full) (nullable): return location for bunker uri
 *
 * Provision a new agent key. Generates a new keypair, stores in SQLCipher, and adds to the hot cache. out_pubkey_hex must be at least 65 bytes. If bunker_pubkey_hex and relay_urls are provided, a bunker:// URI is built and returned via *out_bunker_uri (caller frees with g_free). out_bunker_uri may be NULL if not needed. Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_key_store_provision_agent(SignetKeyStore *ks,
                                     const char *agent_id,
                                     const char *bunker_pubkey_hex,
                                     const char *const *relay_urls,
                                     size_t n_relay_urls,
                                     char *out_pubkey_hex,
                                     size_t out_pubkey_hex_sz,
                                     char **out_bunker_uri);

/* Revoke an agent. Removes from hot cache and SQLCipher.
 * Returns 0 on success, 1 if not found, -1 on error. */
/**
 * signet_key_store_revoke_agent:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 *
 * Revoke an agent. Removes from hot cache and SQLCipher. Returns 0 on success, 1 if not found, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_key_store_revoke_agent(SignetKeyStore *ks, const char *agent_id);

/* Rotate an agent's keypair. Generates a new keypair, replaces old key in
 * both SQLCipher and hot cache.
 * out_pubkey_hex must be at least 65 bytes.
 * Returns 0 on success, 1 if agent not found, -1 on error. */
/**
 * signet_key_store_rotate_agent:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 * @out_pubkey_hex: (out) (not nullable): return location for pubkey hex
 * @out_pubkey_hex_sz: (out): return location for pubkey hex sz
 *
 * Rotate an agent's keypair. Generates a new keypair, replaces old key in both SQLCipher and hot cache. out_pubkey_hex must be at least 65 bytes. Returns 0 on success, 1 if agent not found, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_key_store_rotate_agent(SignetKeyStore *ks,
                                   const char *agent_id,
                                   char *out_pubkey_hex,
                                   size_t out_pubkey_hex_sz);

/* Validate and consume a connect_secret for an agent.
 * Returns 0 if secret matches and was consumed, 1 if no secret required,
 * -1 on mismatch or error. */
/**
 * signet_key_store_validate_connect_secret:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 * @provided_secret: (nullable): provided secret
 *
 * Validate and consume a connect_secret for an agent. Returns 0 if secret matches and was consumed, 1 if no secret required, -1 on mismatch or error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_key_store_validate_connect_secret(SignetKeyStore *ks,
                                              const char *agent_id,
                                              const char *provided_secret);

/* Resolve and consume a connect_secret, returning the bound agent_id.
 * Returns 0 on success, 1 if no such secret exists, -1 on error.
 * Caller owns *out_agent_id (g_free). */
/**
 * signet_key_store_consume_connect_secret:
 * @ks: (not nullable): a #SignetKeyStore
 * @provided_secret: (nullable): provided secret
 * @now: current Unix time in seconds
 * @out_agent_id: (out) (transfer full) (not nullable): return location for agent id
 *
 * Resolve and consume a connect_secret, returning the bound agent_id. Returns 0 on success, 1 if no such secret exists, -1 on error. Caller owns *out_agent_id (g_free).
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_key_store_consume_connect_secret(SignetKeyStore *ks,
                                            const char *provided_secret,
                                            int64_t now,
                                            char **out_agent_id);

/* Copy the derived pubkey for an agent into out_pubkey_hex.
 * out_pubkey_hex must be at least 65 bytes. Returns true on success. */
/**
 * signet_key_store_get_agent_pubkey:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 * @out_pubkey_hex: (out) (not nullable): return location for pubkey hex
 * @out_pubkey_hex_sz: (out): return location for pubkey hex sz
 *
 * Copy the derived pubkey for an agent into out_pubkey_hex. out_pubkey_hex must be at least 65 bytes. Returns true on success.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: %true if the condition is met, otherwise %false
 *
 * Since: 1.0
 */
bool signet_key_store_get_agent_pubkey(SignetKeyStore *ks,
                                       const char *agent_id,
                                       char *out_pubkey_hex,
                                       size_t out_pubkey_hex_sz);

/* List all agent IDs from the hot cache. Caller frees with g_strfreev().
 * Returns 0 on success, -1 on error. */
/**
 * signet_key_store_list_agents:
 * @ks: (not nullable): a #SignetKeyStore
 * @out_ids: (out) (transfer full) (not nullable) (array): return location for ids
 * @out_count: (out) (not nullable): return location for the number of elements
 *
 * List all agent IDs from the hot cache. Caller frees with g_strfreev(). Returns 0 on success, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
int signet_key_store_list_agents(SignetKeyStore *ks,
                                  char ***out_ids,
                                  size_t *out_count);

/* Get the number of keys in the hot cache. */
/**
 * signet_key_store_cache_count:
 * @ks: (not nullable): a #SignetKeyStore
 *
 * Get the number of keys in the hot cache.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.0
 */
uint32_t signet_key_store_cache_count(const SignetKeyStore *ks);

/* Check if the backing store is open. */
/**
 * signet_key_store_is_open:
 * @ks: (not nullable): a #SignetKeyStore
 *
 * Check if the backing store is open.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: %true if the backing store is open, otherwise %false
 *
 * Since: 1.0
 */
bool signet_key_store_is_open(const SignetKeyStore *ks);

/* Get the underlying SignetStore handle (for sub-stores like deny list).
 * Returns NULL if key store is not open. Caller does NOT own the pointer. */
/**
 * signet_key_store_get_store:
 * @ks: (not nullable): a #SignetKeyStore
 *
 * Get the underlying SignetStore handle (for sub-stores like deny list). Returns NULL if key store is not open. Caller does NOT own the pointer.
 *
 * Returns: (transfer none) (nullable): a borrowed pointer owned by the callee
 *
 * Since: 1.0
 */
struct SignetStore *signet_key_store_get_store(SignetKeyStore *ks);

/* Wipe and free a loaded key. Safe on NULL/empty. */
/**
 * signet_loaded_key_clear:
 * @k: (nullable): k
 *
 * Wipe and free a loaded key. Safe on NULL/empty.
 *
 * Since: 1.0
 */
void signet_loaded_key_clear(SignetLoadedKey *k);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_KEY_STORE_H */
