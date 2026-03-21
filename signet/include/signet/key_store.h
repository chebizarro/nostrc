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

typedef struct SignetKeyStore SignetKeyStore;

struct SignetAuditLogger;
struct SignetStore;

typedef struct {
  uint8_t *secret_key;     /* 32-byte secret key (heap, should be in locked memory) */
  size_t secret_key_len;   /* always 32 on success */
  int64_t loaded_at;       /* unix seconds */
  int64_t expires_at;      /* unix seconds; 0 means "no expiry" */
} SignetLoadedKey;

typedef struct {
  const char *db_path;     /* SQLCipher database path */
  const char *master_key;  /* master key for SQLCipher + envelope encryption */
} SignetKeyStoreConfig;

/* Create key store backed by SQLCipher.
 * Opens (or creates) the database and loads all agent keys into the hot cache.
 * Returns NULL on failure. */
SignetKeyStore *signet_key_store_new(struct SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg);

/* Free key store, wiping all cached keys. Safe on NULL. */
void signet_key_store_free(SignetKeyStore *ks);

/* Load the custody key for an agent from the hot cache.
 * Returns true on success (key found), false if not found. */
bool signet_key_store_load_agent_key(SignetKeyStore *ks,
                                     const char *agent_id,
                                     SignetLoadedKey *out_key);

/* Provision a new agent key. Generates a new keypair, stores in SQLCipher,
 * and adds to the hot cache.
 * out_pubkey_hex must be at least 65 bytes.
 * If bunker_pubkey_hex and relay_urls are provided, a bunker:// URI is built and
 * returned via *out_bunker_uri (caller frees with g_free). out_bunker_uri may be
 * NULL if not needed. Returns 0 on success, -1 on error. */
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
int signet_key_store_revoke_agent(SignetKeyStore *ks, const char *agent_id);

/* Rotate an agent's keypair. Generates a new keypair, replaces old key in
 * both SQLCipher and hot cache.
 * out_pubkey_hex must be at least 65 bytes.
 * Returns 0 on success, 1 if agent not found, -1 on error. */
int signet_key_store_rotate_agent(SignetKeyStore *ks,
                                   const char *agent_id,
                                   char *out_pubkey_hex,
                                   size_t out_pubkey_hex_sz);

/* Validate and consume a connect_secret for an agent.
 * Returns 0 if secret matches and was consumed, 1 if no secret required,
 * -1 on mismatch or error. */
int signet_key_store_validate_connect_secret(SignetKeyStore *ks,
                                              const char *agent_id,
                                              const char *provided_secret);

/* Resolve and consume a connect_secret, returning the bound agent_id.
 * Returns 0 on success, 1 if no such secret exists, -1 on error.
 * Caller owns *out_agent_id (g_free). */
int signet_key_store_consume_connect_secret(SignetKeyStore *ks,
                                            const char *provided_secret,
                                            int64_t now,
                                            char **out_agent_id);

/* Copy the derived pubkey for an agent into out_pubkey_hex.
 * out_pubkey_hex must be at least 65 bytes. Returns true on success. */
bool signet_key_store_get_agent_pubkey(SignetKeyStore *ks,
                                       const char *agent_id,
                                       char *out_pubkey_hex,
                                       size_t out_pubkey_hex_sz);

/* List all agent IDs from the hot cache. Caller frees with g_strfreev().
 * Returns 0 on success, -1 on error. */
int signet_key_store_list_agents(SignetKeyStore *ks,
                                  char ***out_ids,
                                  size_t *out_count);

/* Get the number of keys in the hot cache. */
uint32_t signet_key_store_cache_count(const SignetKeyStore *ks);

/* Check if the backing store is open. */
bool signet_key_store_is_open(const SignetKeyStore *ks);

/* Get the underlying SignetStore handle (for sub-stores like deny list).
 * Returns NULL if key store is not open. Caller does NOT own the pointer. */
struct SignetStore *signet_key_store_get_store(SignetKeyStore *ks);

/* Wipe and free a loaded key. Safe on NULL/empty. */
void signet_loaded_key_clear(SignetLoadedKey *k);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_KEY_STORE_H */
