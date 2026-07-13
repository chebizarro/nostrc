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
 * SignetAdoptResult:
 * Result codes for signet_key_store_adopt_agent().
 */
typedef enum {
  SIGNET_ADOPT_OK = 0,
  SIGNET_ADOPT_ERR_INVALID_SECRET = -1,
  SIGNET_ADOPT_ERR_PUBKEY_MISMATCH = -2,
  SIGNET_ADOPT_ERR_AGENT_EXISTS = -3,
  SIGNET_ADOPT_ERR_PUBKEY_EXISTS = -4,
  SIGNET_ADOPT_ERR_INTERNAL = -5,
} SignetAdoptResult;

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

/**
 * signet_key_store_adopt_agent:
 * Register an EXISTING canonical identity (BYO-key) instead of minting a new
 * keypair. Derives the pubkey from @secret_key; if @expected_pubkey_hex is
 * non-NULL it must match exactly. Fails if @agent_id already exists or the
 * pubkey is already bound to another agent. On success stores the key in the
 * same encrypted path as provisioned agents (provenance=adopted), returns the
 * pubkey via @out_pubkey_hex (>=65 bytes) and, when @bunker_pubkey_hex/relays
 * are given, a bunker:// URI via *@out_bunker_uri (caller frees with g_free).
 * @connect_secret_in may be NULL to generate a random one. The caller must
 * zeroize @secret_key after the call.
 *
 * Returns: SIGNET_ADOPT_OK on success, or a SignetAdoptResult error code.
 */
SignetAdoptResult signet_key_store_adopt_agent(SignetKeyStore *ks,
                                               const char *agent_id,
                                               const uint8_t secret_key[32],
                                               const char *expected_pubkey_hex,
                                               const char *connect_secret_in,
                                               const char *bunker_pubkey_hex,
                                               const char *const *relay_urls,
                                               size_t n_relay_urls,
                                               char out_pubkey_hex[65],
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

/* Mint and persist a fresh one-time connect_secret for an EXISTING agent
 * (management agent/reissue-connect). The previous secret — consumed or not —
 * becomes invalid. Requires a backing store (fails in cache-only mode).
 * When @expected_user_pubkey is non-NULL, the reissue proceeds only if the
 * agent's CURRENT identity pubkey equals it (case-insensitive), checked
 * atomically with the mutation under the key-store mutex — self-service
 * callers pass the authenticated sender so a concurrent rotate cannot let a
 * superseded key mint a secret for the new identity (returns 2 on mismatch).
 * Provisioner callers pass NULL.
 * On success:
 *   - *out_connect_secret receives the fresh secret (caller wipes + g_free);
 *   - out_pubkey_hex (>= 65 bytes) receives the agent identity pubkey;
 *   - when @bunker_pubkey_hex/relays are given and out_bunker_uri is non-NULL,
 *     *out_bunker_uri receives a bunker:// URI embedding the fresh secret
 *     (caller wipes + g_free).
 * Returns 0 on success, 1 if the agent does not exist, 2 on identity
 * mismatch, -1 on error. */
/**
 * signet_key_store_reissue_connect_secret:
 * @ks: (not nullable): a #SignetKeyStore
 * @agent_id: (not nullable): agent identifier
 * @expected_user_pubkey: (nullable): require the agent's current identity pubkey to equal this 64-hex value
 * @bunker_pubkey_hex: (nullable): bunker pubkey for bunker:// URI
 * @relay_urls: (nullable) (array length=n_relay_urls): relay URLs for the URI
 * @n_relay_urls: number of elements
 * @out_pubkey_hex: (out) (not nullable): return location for the agent pubkey hex (>= 65 bytes)
 * @out_connect_secret: (out) (transfer full) (not nullable): return location for the fresh secret
 * @out_bunker_uri: (out) (transfer full) (nullable): return location for a bunker:// URI
 *
 * Mint and persist a fresh one-time connect secret for an existing agent, invalidating any prior secret. Returns 0 on success, 1 if the agent does not exist, -1 on error.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.1
 */
int signet_key_store_reissue_connect_secret(SignetKeyStore *ks,
                                            const char *agent_id,
                                            const char *expected_user_pubkey,
                                            const char *bunker_pubkey_hex,
                                            const char *const *relay_urls,
                                            size_t n_relay_urls,
                                            char out_pubkey_hex[65],
                                            char **out_connect_secret,
                                            char **out_bunker_uri);

/* Backfill agents.pubkey for legacy rows created before the pubkey column
 * existed (pre-v3.1). For each row with a NULL/empty pubkey, decrypts the
 * custody key, derives the x-only pubkey, and persists it so
 * signet_store_pubkey_in_use() collision checks (agent/adopt-existing) cover
 * the whole fleet. Rows whose secret cannot be decrypted — and rows whose
 * derived pubkey is already bound to another agent (duplicate legacy custody
 * keys) — are counted in *out_failed and left untouched. Idempotent —
 * populated rows are never rewritten. No-op (returns 0 with zero counts) in
 * cache-only mode.
 * Returns 0 on success (even with per-row failures), -1 on store error. */
/**
 * signet_key_store_backfill_pubkeys:
 * @ks: (not nullable): a #SignetKeyStore
 * @out_updated: (out) (nullable): return location for the number of rows backfilled
 * @out_failed: (out) (nullable): return location for the number of rows that could not be backfilled
 *
 * Backfills the persisted pubkey for legacy agent rows. Idempotent.
 *
 * Thread safety: callers may share the object when the implementation serializes access internally; avoid mutating the same output storage concurrently.
 *
 * Returns: operation-specific status or value as documented by the function
 *
 * Since: 1.1
 */
int signet_key_store_backfill_pubkeys(SignetKeyStore *ks,
                                      size_t *out_updated,
                                      size_t *out_failed);

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
