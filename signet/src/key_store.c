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
#include "signet/store_tokens.h"
#include "signet/audit_logger.h"

#include <stdio.h>
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
  char pubkey_hex[65];     /* derived pubkey for this agent */
  int64_t loaded_at;
} SignetCacheEntry;

static void signet_secret_key_to_hex(const uint8_t *sk, char out_hex[65]) {
  for (int i = 0; i < 32; i++) {
    sprintf(out_hex + (i * 2), "%02x", sk[i]);
  }
  out_hex[64] = '\0';
}

static SignetCacheEntry *signet_cache_entry_new(const uint8_t *sk, int64_t loaded_at) {
  /* Allocate in locked memory via sodium_malloc */
  SignetCacheEntry *e = (SignetCacheEntry *)sodium_malloc(sizeof(SignetCacheEntry));
  if (!e) return NULL;
  memset(e, 0, sizeof(*e));
  memcpy(e->secret_key, sk, 32);

  char sk_hex[65];
  signet_secret_key_to_hex(sk, sk_hex);
  char *pub_hex = nostr_key_get_public(sk_hex);
  secure_wipe(sk_hex, sizeof(sk_hex));
  if (!pub_hex || strlen(pub_hex) != 64) {
    if (pub_hex) free(pub_hex);
    sodium_memzero(e->secret_key, 32);
    sodium_free(e);
    return NULL;
  }
  memcpy(e->pubkey_hex, pub_hex, 65);
  free(pub_hex);

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
                                     const char *bunker_pubkey_hex,
                                     const char *const *relay_urls,
                                     size_t n_relay_urls,
                                     char *out_pubkey_hex,
                                     size_t out_pubkey_hex_sz,
                                     char **out_bunker_uri) {
  if (!ks || !agent_id || !out_pubkey_hex || out_pubkey_hex_sz < 65) return -1;
  if (out_bunker_uri) *out_bunker_uri = NULL;

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

  /* Generate a random connect_secret (32 bytes hex = 64 chars). */
  uint8_t secret_raw[32];
  randombytes_buf(secret_raw, sizeof(secret_raw));
  char connect_secret[65];
  for (int i = 0; i < 32; i++) {
    sprintf(connect_secret + i * 2, "%02x", secret_raw[i]);
  }
  connect_secret[64] = '\0';
  sodium_memzero(secret_raw, sizeof(secret_raw));

  g_mutex_lock(&ks->mu);

  int rc = -1;
  int64_t now = (int64_t)time(NULL);

  /* Store in SQLCipher (with connect_secret). */
  if (ks->store) {
    rc = signet_store_put_agent(ks->store, agent_id, sk_raw, 32, connect_secret, now);
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

  /* Build bunker:// URI if requested and provision succeeded.
   * Format: bunker://<bunker_pubkey>?relay=<url1>&relay=<url2>&secret=<connect_secret> */
  if (rc == 0 && out_bunker_uri && bunker_pubkey_hex && bunker_pubkey_hex[0]) {
    GString *uri = g_string_new("bunker://");
    g_string_append(uri, bunker_pubkey_hex);
    g_string_append_c(uri, '?');
    for (size_t i = 0; i < n_relay_urls; i++) {
      if (i > 0) g_string_append_c(uri, '&');
      /* URL-encode the relay URL */
      char *escaped = g_uri_escape_string(relay_urls[i], NULL, FALSE);
      g_string_append(uri, "relay=");
      g_string_append(uri, escaped ? escaped : relay_urls[i]);
      g_free(escaped);
    }
    if (n_relay_urls > 0) g_string_append_c(uri, '&');
    g_string_append(uri, "secret=");
    g_string_append(uri, connect_secret);
    *out_bunker_uri = g_string_free(uri, FALSE);
  }

  sodium_memzero(sk_raw, 32);
  sodium_memzero(connect_secret, sizeof(connect_secret));
  secure_wipe(sk_hex, strlen(sk_hex));
  free(sk_hex);
  free(pk_hex);

  return rc;
}

int signet_key_store_validate_connect_secret(SignetKeyStore *ks,
                                              const char *agent_id,
                                              const char *provided_secret) {
  if (!ks || !agent_id) return -1;

  g_mutex_lock(&ks->mu);

  if (!ks->store) {
    /* No backing store — no secret to validate. */
    g_mutex_unlock(&ks->mu);
    return 1; /* no secret required */
  }

  SignetAgentRecord rec;
  memset(&rec, 0, sizeof(rec));
  int rc = signet_store_get_agent(ks->store, agent_id, &rec);
  if (rc != 0) {
    g_mutex_unlock(&ks->mu);
    return -1; /* agent not found or error */
  }

  int result;
  if (!rec.connect_secret || !rec.connect_secret[0]) {
    /* No connect_secret set — already consumed or never had one. */
    result = 1; /* no secret required */
  } else if (!provided_secret || !provided_secret[0]) {
    /* Secret required but not provided. */
    result = -1;
  } else if (g_strcmp0(rec.connect_secret, provided_secret) == 0) {
    /* Match! Consume the secret so it can't be reused. */
    signet_store_consume_connect_secret(ks->store, agent_id);
    result = 0;
  } else {
    /* Mismatch. */
    result = -1;
  }

  signet_agent_record_clear(&rec);
  g_mutex_unlock(&ks->mu);
  return result;
}

int signet_key_store_consume_connect_secret(SignetKeyStore *ks,
                                            const char *provided_secret,
                                            int64_t now,
                                            char **out_agent_id) {
  if (out_agent_id) *out_agent_id = NULL;
  if (!ks || !provided_secret || !provided_secret[0] || !out_agent_id) return -1;

  g_mutex_lock(&ks->mu);
  if (!ks->store) {
    g_mutex_unlock(&ks->mu);
    return -1;
  }

  int rc = signet_store_consume_connect_secret_value(ks->store, provided_secret, now, out_agent_id);
  g_mutex_unlock(&ks->mu);
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

int signet_key_store_rotate_agent(SignetKeyStore *ks,
                                   const char *agent_id,
                                   char *out_pubkey_hex,
                                   size_t out_pubkey_hex_sz) {
  if (!ks || !agent_id || !out_pubkey_hex || out_pubkey_hex_sz < 65) return -1;

  g_mutex_lock(&ks->mu);

  /* Check that the agent exists in the cache. */
  if (!g_hash_table_contains(ks->cache, agent_id)) {
    g_mutex_unlock(&ks->mu);
    return 1; /* not found */
  }

  /* Generate a new keypair. */
  char *sk_hex = nostr_key_generate_private();
  if (!sk_hex) {
    g_mutex_unlock(&ks->mu);
    return -1;
  }

  char *pk_hex = nostr_key_get_public(sk_hex);
  if (!pk_hex) {
    secure_wipe(sk_hex, strlen(sk_hex));
    free(sk_hex);
    g_mutex_unlock(&ks->mu);
    return -1;
  }

  /* Convert sk hex to raw bytes. */
  uint8_t sk_raw[32];
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    sscanf(sk_hex + i * 2, "%2x", &byte);
    sk_raw[i] = (uint8_t)byte;
  }

  int rc = -1;
  int64_t now = (int64_t)time(NULL);

  /* Replace in SQLCipher (connect_secret = NULL for rotated keys). */
  if (ks->store) {
    rc = signet_store_put_agent(ks->store, agent_id, sk_raw, 32, NULL, now);
  }

  if (rc == 0 || !ks->store) {
    /* Replace in hot cache. */
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

int signet_key_store_list_agents(SignetKeyStore *ks,
                                  char ***out_ids,
                                  size_t *out_count) {
  if (!ks || !out_ids || !out_count) return -1;
  *out_ids = NULL;
  *out_count = 0;

  g_mutex_lock(&ks->mu);

  guint n = g_hash_table_size(ks->cache);
  char **ids = (char **)g_new0(char *, n + 1);
  if (!ids && n > 0) {
    g_mutex_unlock(&ks->mu);
    return -1;
  }

  GHashTableIter iter;
  gpointer key, value;
  size_t i = 0;
  g_hash_table_iter_init(&iter, ks->cache);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    ids[i++] = g_strdup((const char *)key);
  }

  g_mutex_unlock(&ks->mu);

  *out_ids = ids;
  *out_count = i;
  return 0;
}

bool signet_key_store_get_agent_pubkey(SignetKeyStore *ks,
                                       const char *agent_id,
                                       char *out_pubkey_hex,
                                       size_t out_pubkey_hex_sz) {
  if (!ks || !agent_id || !out_pubkey_hex || out_pubkey_hex_sz < 65) return false;

  g_mutex_lock(&ks->mu);
  SignetCacheEntry *entry = (SignetCacheEntry *)g_hash_table_lookup(ks->cache, agent_id);
  if (!entry) {
    g_mutex_unlock(&ks->mu);
    return false;
  }

  memcpy(out_pubkey_hex, entry->pubkey_hex, 65);
  g_mutex_unlock(&ks->mu);
  return true;
}

uint32_t signet_key_store_cache_count(const SignetKeyStore *ks) {
  if (!ks || !ks->cache) return 0;

  g_mutex_lock((GMutex *)&ks->mu);
  uint32_t n = (uint32_t)g_hash_table_size(ks->cache);
  g_mutex_unlock((GMutex *)&ks->mu);

  return n;
}

bool signet_key_store_is_open(const SignetKeyStore *ks) {
  if (!ks) return false;
  if (ks->store) return signet_store_is_open(ks->store);
  return true; /* cache-only mode */
}

SignetStore *signet_key_store_get_store(SignetKeyStore *ks) {
  if (!ks) return NULL;
  return ks->store;
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
