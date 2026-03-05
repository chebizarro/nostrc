/* SPDX-License-Identifier: MIT
 *
 * key_store.h - Key custody interface for Signet.
 *
 * Signet's key store loads signing keys from Vault (KV v2) and returns them in
 * locked/zeroized secure buffers (libnostr secure_buf).
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
struct SignetVaultClient;

typedef struct {
  nostr_secure_buf secret_key; /* secret material; must be freed via signet_loaded_key_clear() */
  int64_t loaded_at;           /* unix seconds */
  int64_t expires_at;          /* unix seconds; 0 means "no expiry"/unknown */
} SignetLoadedKey;

typedef struct {
  /* If 0, disable caching entirely (always read from Vault). */
  uint32_t cache_ttl_seconds;

  /* Vault KV v2 mount name, e.g. "secret".
   * Used to build: GET /v1/{mount}/data/{path}
   * If NULL/empty, defaults to "secret".
   */
  const char *vault_kv_mount;

  /* Key prefix within the KV mount, e.g. "signet/keys".
   * The resolved secret path becomes: {key_prefix}/{identity}
   * If NULL/empty, defaults to "signet/keys".
   */
  const char *key_prefix;

  /* JSON field name inside Vault KV v2 data.data.* containing the secret key hex.
   * Default: "secp256k1_sk_hex"
   */
  const char *secret_key_field;
} SignetKeyStoreConfig;

/* Create key store. Returns NULL on OOM/invalid config. */
SignetKeyStore *signet_key_store_new(struct SignetVaultClient *vault,
                                     struct SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg);

/* Free key store. Safe on NULL. */
void signet_key_store_free(SignetKeyStore *ks);

/* Load the custody key for an identity.
 *
 * identity: logical Signet identity (used for path resolution).
 * out_key: returned secure buffer and metadata.
 *
 * Returns true on success.
 */
bool signet_key_store_load_identity_key(SignetKeyStore *ks,
                                        const char *identity,
                                        SignetLoadedKey *out_key);

/* Wipe and free a loaded key. Safe on NULL/empty. */
void signet_loaded_key_clear(SignetLoadedKey *k);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_KEY_STORE_H */