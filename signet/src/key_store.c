/* SPDX-License-Identifier: MIT
 *
 * key_store.c - Key custody stub (pending SQLCipher + hot cache implementation).
 *
 * This module is a minimal stub following the Vault removal. It provides the
 * key_store interface but always returns "not found" for key lookups.
 * The real implementation will be provided by:
 *   - store.c (SQLCipher persistence)
 *   - key-cache.c (mlock'd GHashTable hot cache)
 */

#include "signet/key_store.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>

struct SignetKeyStore {
  SignetAuditLogger *audit;
};

SignetKeyStore *signet_key_store_new(SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg) {
  (void)cfg;

  SignetKeyStore *ks = calloc(1, sizeof(*ks));
  if (!ks) return NULL;

  ks->audit = audit;
  return ks;
}

void signet_key_store_free(SignetKeyStore *ks) {
  if (!ks) return;
  free(ks);
}

bool signet_key_store_load_agent_key(SignetKeyStore *ks,
                                     const char *agent_id,
                                     SignetLoadedKey *out_key) {
  (void)ks;
  (void)agent_id;

  if (out_key) {
    memset(out_key, 0, sizeof(*out_key));
  }

  /* Stub: no backend available yet. SQLCipher store + hot cache
   * will provide the real implementation. */
  return false;
}

void signet_loaded_key_clear(SignetLoadedKey *k) {
  if (!k) return;
  secure_free(&k->secret_key);
  k->loaded_at = 0;
  k->expires_at = 0;
}