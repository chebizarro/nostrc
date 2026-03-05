/* SPDX-License-Identifier: MIT
 *
 * key_store.c - Key custody with SQLCipher + mlock'd hot key cache.
 *
 * Architecture:
 *   SQLCipher DB (cold) ←→ GHashTable in mlock'd pages (hot)
 *
 * At startup, all agent keys are loaded from SQLCipher into the hot cache.
 * The signing hot path reads only from the cache (pointer dereference).
 * Provisioning/revocation update both SQLCipher and the cache atomically.
 */

#include "signet/key_store.h"
#include "signet/store.h"
#include "signet/audit_logger.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>

/* libnostr */
#include <nostr-keys.h>
#include <secure_buf.h>

/* libsodium for mlock */
#include <sodium.h>

/* ----------------------------- cache entry -------------------------------- */

typedef struct {
  uint8_t secret_key[32];  /* plaintext secret key (mlock'd) */
  int64_t loaded_at;
} SignetCacheEntry;

static SignetCacheEntry *signet_cache_entry_new(const uint8_t *sk, int64_t loaded_at) {
  /* Allocate in locked memory via sodium_malloc */
  SignetCacheEntry *e = (SignetCacheEntry *)sodium_malloc(sizeof(SignetCacheEntry));
  if (!e) return NULL;
  memcpy(e->secret_key, sk, 32);
  e->loaded_at = loaded_at;
  return e;
}

static void signet_cache_entry_free(gpointer p) {
  if (!p) return;
  SignetCacheEntry *e = (SignetCacheEntry *)p;
  sodium_memzero(e->secret_key, 32);
  sodium_free(e);
}

/* ------------------------------ key store -------------------------------- */

struct SignetKeyStore {
  SignetAuditLogger *audit;
  SignetStore *store;

  /* Hot cache: agent_id (gchar*) → SignetCacheEntry* (mlock'd) */
  GHashTable *cache;
  GMutex mu;
};

SignetKeyStore *signet_key_store_new(SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg) {
  if (sodium_init() < 0) return NULL;

  SignetKeyStore *ks = (SignetKeyStore *)calloc(1, sizeof(*ks));
  if (!ks) return NULL;

  g_mutex_init(&ks->mu);
  ks->audit = audit;

  ks->cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                     g_free, signet_cache_entry_free);
  if (!ks->cache) {
    g_mutex_clear(&ks->mu);
    free(ks);
    return NULL;
  }

  /* Open SQLCipher store if config provides db_path + master_key. */
  if (cfg && cfg->db_path && cfg->master_key && cfg->db_path[0] && cfg->master_key[0]) {
    SignetStoreConfig sc = {
      .db_path = cfg->db_path,
      .master_key = cfg->master_key,
    };
    ks->store = signet_store_open(&sc);

    /* Load all agent keys into the hot cache. */
    if (ks->store) {
      char **ids = NULL;
      size_t count = 0;
      if (signet_store_list_agents(ks->store, &ids, &count) == 0) {
        int64_t now = (int64_t)time(NULL);
        for (size_t i = 0; i < count; i++) {
          SignetAgentRecord rec;
          memset(&rec, 0, sizeof(rec));
          if (signet_store_get_agent(ks->store, ids[i], &rec) == 0) {
            SignetCacheEntry *entry = signet_cache_entry_new(rec.secret_key, now);
            if (entry) {
              g_hash_table_replace(ks->cache, g_strdup(ids[i]), entry);
            }
            signet_agent_record_clear(&rec);
          }
        }
        signet_store_free_agent_ids(ids, count);
      }
    }
  }

  return ks;
}

void signet_key_store_free(SignetKeyStore *ks) {
  if (!ks) return;

  g_mutex_lock(&ks->mu);

  if (ks->cache) {
    g_hash_table_destroy(ks->cache);
    ks->cache = NULL;
  }

  if (ks->store) {
    signet_store_close(ks->store);
    ks->store = NULL;
  }

  g_mutex_unlock(&ks->mu);
  g_mutex_clear(&ks->mu);
  free(ks);
}

bool signet_key_store_load_agent_key(SignetKeyStore *ks,
                                     const char *agent_id,
                                     SignetLoadedKey *out_key) {
  if (!ks || !agent_id || !out_key) return false;
  memset(out_key, 0, sizeof(*out_key));

  g_mutex_lock(&ks->mu);

  SignetCacheEntry *entry = (SignetCacheEntry *)g_hash_table_lookup(ks->cache, agent_id);
  if (!entry) {
    g_mutex_unlock(&ks->mu);
    return false;
  }

  /* Copy the secret key into a fresh locked allocation. */
  uint8_t *sk_copy = (uint8_t *)sodium_malloc(32);
  if (!sk_copy) {
    g_mutex_unlock(&ks->mu);
    return false;
  }
  memcpy(sk_copy, entry->secret_key, 32);

  out_key->secret_key = sk_copy;
  out_key->secret_key_len = 32;
  out_key->loaded_at = entry->loaded_at;
  out_key->expires_at = 0;

  /* Touch the last_used in the backing store (best-effort). */
  if (ks->store) {
    signet_store_touch_agent(ks->store, agent_id, (int64_t)time(NULL));
  }

  g_mutex_unlock(&ks->mu);
  return true;
}

int signet_key_store_provision_agent(SignetKeyStore *ks,
                                     const char *agent_id,
                                     char *out_pubkey_hex,
                                     size_t out_pubkey_hex_sz) {
  if (!ks || !agent_id || !out_pubkey_hex || out_pubkey_hex_sz < 65) return -1;

  /* Generate a new keypair using libnostr. */
  char *sk_hex = nostr_key_generate_private();
  if (!sk_hex) return -1;

  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) {
    secure_wipe(sk_hex, strlen(sk_hex));
    free(sk_hex);
    return -1;
  }

  /* Convert sk hex to raw bytes. */
  uint8_t sk_raw[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    sscanf(sk_hex + i * 2, "%2x", &byte);
    sk_raw[i] = (uint8_t)byte;
  }

  g_mutex_lock(&ks->mu);

  int rc = -1;
  int64_t now = (int64_t)time(NULL);

  /* Store in SQLCipher. */
  if (ks->store) {
    rc = signet_store_put_agent(ks->store, agent_id, sk_raw, 32, now);
  }

  if (rc == 0 || !ks->store) {
    /* Add to hot cache. */
    SignetCacheEntry *entry = signet_cache_entry_new(sk_raw, now);
    if (entry) {
      g_hash_table_replace(ks->cache, g_strdup(agent_id), entry);
      rc = 0;
    } else {
      rc = -1;
    }
  }

  g_mutex_unlock(&ks->mu);

  if (rc == 0) {
    /* Copy pubkey to output. */
    size_t pk_len = strlen(pk_hex);
    if (pk_len < out_pubkey_hex_sz) {
      memcpy(out_pubkey_hex, pk_hex, pk_len + 1);
    } else {
      rc = -1;
    }
  }

  sodium_memzero(sk_raw, 32);
  secure_wipe(sk_hex, strlen(sk_hex));
  free(sk_hex);
  free(pk_hex);

  return rc;
}

int signet_key_store_revoke_agent(SignetKeyStore *ks, const char *agent_id) {
  if (!ks || !agent_id) return -1;

  g_mutex_lock(&ks->mu);

  /* Remove from hot cache (wipes the key via destroy func). */
  gboolean found = g_hash_table_remove(ks->cache, agent_id);

  /* Remove from backing store. */
  int store_rc = 0;
  if (ks->store) {
    store_rc = signet_store_delete_agent(ks->store, agent_id);
  }

  g_mutex_unlock(&ks->mu);

  if (!found && store_rc == 1) return 1; /* not found anywhere */
  if (store_rc < 0) return -1;
  return 0;
}

uint32_t signet_key_store_cache_count(const SignetKeyStore *ks) {
  if (!ks || !ks->cache) return 0;
  /* Note: not locking for this read-only atomic-ish operation. */
  return (uint32_t)g_hash_table_size(ks->cache);
}

bool signet_key_store_is_open(const SignetKeyStore *ks) {
  if (!ks) return false;
  if (ks->store) return signet_store_is_open(ks->store);
  return true; /* cache-only mode */
}

void signet_loaded_key_clear(SignetLoadedKey *k) {
  if (!k) return;
  if (k->secret_key) {
    sodium_free(k->secret_key);
    k->secret_key = NULL;
  }
  k->secret_key_len = 0;
  k->loaded_at = 0;
  k->expires_at = 0;
}