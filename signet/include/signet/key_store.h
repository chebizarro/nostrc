/* SPDX-License-Identifier: MIT
 *
 * key_store.h - Key custody interface for Signet.
 *
 * Signet's key store provides signing keys from an mlock'd in-process cache
 * backed by SQLCipher persistence. Keys are loaded into the hot cache at
 * startup and accessed via pointer dereference on the signing hot path.
 *
 * This header defines the interface. The SQLCipher store and hot key cache
 * modules (store.h, key-cache.h) will be implemented in subsequent tasks.
 * For now, this is a stub that always returns "not found".
 */

#ifndef SIGNET_KEY_STORE_H
#define SIGNET_KEY_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <secure_buf.h>

typedef struct SignetKeyStore SignetKeyStore;

struct SignetAuditLogger;

typedef struct {
  nostr_secure_buf secret_key; /* secret material; must be freed via signet_loaded_key_clear() */
  int64_t loaded_at;           /* unix seconds */
  int64_t expires_at;          /* unix seconds; 0 means "no expiry"/unknown */
} SignetLoadedKey;

typedef struct {
  /* Reserved for SQLCipher store configuration (db_path, etc).
   * Will be populated when the store module is implemented. */
  const char *placeholder;
} SignetKeyStoreConfig;

/* Create key store. Returns NULL on OOM/invalid config. */
SignetKeyStore *signet_key_store_new(struct SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg);

/* Free key store. Safe on NULL. */
void signet_key_store_free(SignetKeyStore *ks);

/* Load the custody key for an agent.
 *
 * agent_id: unique agent identifier.
 * out_key: returned secure buffer and metadata.
 *
 * Returns true on success.
 * NOTE: Currently a stub that always returns false (no backend).
 */
bool signet_key_store_load_agent_key(SignetKeyStore *ks,
                                     const char *agent_id,
                                     SignetLoadedKey *out_key);

/* Wipe and free a loaded key. Safe on NULL/empty. */
void signet_loaded_key_clear(SignetLoadedKey *k);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_KEY_STORE_H */