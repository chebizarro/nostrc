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
#include "key_store_private.h"
#include "signet/store.h"
#include "signet/store_tokens.h"
#include "signet/audit_logger.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <sqlite3.h>

/* libnostr */
#include <nostr-keys.h>
#include <nostr/nip04.h>
#include <nostr/nip44/nip44.h>
#include <secure_buf.h>

/* libsodium for mlock */
#include <sodium.h>

/* ----------------------------- cache entry -------------------------------- */

typedef struct {
  uint8_t secret_key[32];  /* plaintext secret key (mlock'd) */
  char pubkey_hex[65];     /* derived pubkey for this agent */
  int64_t loaded_at;
  /* Process-unique id of this entry. Cache-only client bindings pin it, so
   * any replacement or removal of the entry invalidates them. */
  uint64_t generation;
  /* Cache-only mode: SHA-256 hex of the agent's pending one-time
   * connect_secret ("" = none or consumed). Persistent mode keeps the
   * secret in the store instead and leaves this empty. */
  char connect_secret_hash[65];
} SignetCacheEntry;

/* Cache-only NIP-46 client binding (see key_store_private.h). */
typedef struct {
  char *agent_id;
  uint64_t generation;      /* SignetCacheEntry.generation at pairing time */
  char secret_hash[65];     /* SHA-256 hex of the consumed pairing secret */
} SignetEphemeralBinding;

static void signet_ephemeral_binding_free(gpointer p) {
  SignetEphemeralBinding *b = p;
  if (!b) return;
  if (b->agent_id) {
    sodium_memzero(b->agent_id, strlen(b->agent_id));
    g_free(b->agent_id);
  }
  sodium_memzero(b, sizeof(*b));
  g_free(b);
}

static void signet_secret_hash_free(gpointer p) {
  if (!p) return;
  sodium_memzero(p, strlen((const char *)p));
  g_free(p);
}

static atomic_uint_fast64_t signet_cache_next_generation = 1;

/* Weak allocation seam used by the key-store unit test to prove that a
 * replacement key is fully prepared before durable identity state changes.
 * Production builds resolve this implementation directly. */
__attribute__((weak)) void *signet_key_store_cache_alloc(size_t size) {
  return sodium_malloc(size);
}

static void signet_secret_key_to_hex(const uint8_t *sk, char out_hex[65]) {
  for (int i = 0; i < 32; i++) {
    sprintf(out_hex + (i * 2), "%02x", sk[i]);
  }
  out_hex[64] = '\0';
}

static SignetCacheEntry *signet_cache_entry_new(const uint8_t *sk, int64_t loaded_at) {
  /* Allocate in locked memory via sodium_malloc */
  SignetCacheEntry *e =
      (SignetCacheEntry *)signet_key_store_cache_alloc(sizeof(SignetCacheEntry));
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
  e->generation = atomic_fetch_add(&signet_cache_next_generation, 1);
  return e;
}

static void signet_cache_entry_free(gpointer p) {
  if (!p) return;
  SignetCacheEntry *e = (SignetCacheEntry *)p;
  sodium_memzero(e, sizeof(*e));
  sodium_free(e);
}

/* ------------------------------ key store -------------------------------- */

struct SignetKeyStore {
  SignetAuditLogger *audit;
  SignetStore *store;

  /* Hot cache: agent_id (gchar*) → SignetCacheEntry* (mlock'd) */
  GHashTable *cache;
  /* Cache-only NIP-46 bindings: canonical client pubkey (gchar*) →
   * SignetEphemeralBinding*. Unused when a persistent store is open. */
  GHashTable *ephemeral_bindings;
  /* Spent hashes outlive bindings, which are dropped on revoke/rotate. */
  GHashTable *spent_secret_hashes;
  GMutex mu;
#ifdef SIGNET_ENABLE_TEST_HOOKS
  char *test_next_reissue_secret;
#endif
};

#ifdef SIGNET_ENABLE_TEST_HOOKS
void signet_key_store_test_next_reissue_secret(SignetKeyStore *ks, const char *secret) {
  if (!ks) return;
  g_mutex_lock(&ks->mu);
  if (ks->test_next_reissue_secret) {
    sodium_memzero(ks->test_next_reissue_secret, strlen(ks->test_next_reissue_secret));
    g_free(ks->test_next_reissue_secret);
  }
  ks->test_next_reissue_secret = secret ? g_strdup(secret) : NULL;
  g_mutex_unlock(&ks->mu);
}
#endif

/* SHA-256 hex of a connect secret, the same digest the persistent store
 * records as bound_secret_hash. */
static bool signet_connect_secret_hash(const char *secret, char out[65]) {
  out[0] = '\0';
  if (!secret || !secret[0]) return false;
  char *h = g_compute_checksum_for_string(G_CHECKSUM_SHA256, secret, -1);
  bool ok = h && strlen(h) == 64;
  if (ok) memcpy(out, h, 65);
  if (h) {
    sodium_memzero(h, strlen(h));
    g_free(h);
  }
  return ok;
}

/* True when a hot-cache entry other than @self already holds @hash as its
 * pending connect secret (the cache-only analogue of the store's UNIQUE
 * column). Call with ks->mu held. */
static bool signet_cache_secret_hash_in_use(SignetKeyStore *ks,
                                            const SignetCacheEntry *self,
                                            const char *hash) {
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init(&it, ks->cache);
  while (g_hash_table_iter_next(&it, NULL, &v)) {
    const SignetCacheEntry *e = v;
    if (e != self && e->connect_secret_hash[0] &&
        sodium_memcmp(e->connect_secret_hash, hash, 64) == 0)
      return true;
  }
  /* Every consumed binding secret is retained in this hash index even after
   * revoke/rotate drops the binding. Never resurrect an old bunker URI. */
  return g_hash_table_contains(ks->spent_secret_hashes, hash);
}

/* Cache-only: make @secret the pending one-time connect secret of @e (which
 * may not be in the cache yet), replacing any earlier one. Fails when the
 * digest cannot be computed or another agent holds the same pending secret.
 * Call with ks->mu held. */
static bool signet_cache_set_pending_secret(SignetKeyStore *ks, SignetCacheEntry *e,
                                            const char *secret) {
  char hash[65];
  bool ok = signet_connect_secret_hash(secret, hash) &&
            !(e->connect_secret_hash[0] &&
              sodium_memcmp(e->connect_secret_hash, hash, 64) == 0) &&
            !signet_cache_secret_hash_in_use(ks, e, hash);
  if (ok) memcpy(e->connect_secret_hash, hash, 65);
  sodium_memzero(hash, sizeof(hash));
  return ok;
}

/* Cache-only: drop every client binding of @agent_id. Called whenever the
 * agent's hot-cache entry is removed or replaced, so stale bindings do not
 * linger until their client is next looked up (lookup also re-checks the
 * pinned generation). A no-op with a persistent store, whose bindings live in
 * agent_clients. Call with ks->mu held. */
static void signet_ephemeral_bindings_drop_agent(SignetKeyStore *ks, const char *agent_id) {
  if (!ks->ephemeral_bindings || !agent_id) return;
  GHashTableIter it;
  gpointer v;
  g_hash_table_iter_init(&it, ks->ephemeral_bindings);
  while (g_hash_table_iter_next(&it, NULL, &v)) {
    const SignetEphemeralBinding *b = v;
    if (g_strcmp0(b->agent_id, agent_id) == 0) g_hash_table_iter_remove(&it);
  }
}

SignetKeyStore *signet_key_store_new(SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg) {
  if (sodium_init() < 0) return NULL;

  SignetKeyStore *ks = (SignetKeyStore *)calloc(1, sizeof(*ks));
  if (!ks) return NULL;

  g_mutex_init(&ks->mu);
  ks->audit = audit;

  ks->cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                     g_free, signet_cache_entry_free);
  ks->ephemeral_bindings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                 signet_ephemeral_binding_free);
  ks->spent_secret_hashes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                    signet_secret_hash_free, NULL);
  if (!ks->cache || !ks->ephemeral_bindings || !ks->spent_secret_hashes) {
    if (ks->cache) g_hash_table_destroy(ks->cache);
    if (ks->ephemeral_bindings) g_hash_table_destroy(ks->ephemeral_bindings);
    if (ks->spent_secret_hashes) g_hash_table_destroy(ks->spent_secret_hashes);
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

    if (!ks->store) {
      signet_key_store_free(ks);
      return NULL;
    }

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
  if (ks->ephemeral_bindings) {
    g_hash_table_destroy(ks->ephemeral_bindings);
    ks->ephemeral_bindings = NULL;
  }
  if (ks->spent_secret_hashes) {
    g_hash_table_destroy(ks->spent_secret_hashes);
    ks->spent_secret_hashes = NULL;
  }
#ifdef SIGNET_ENABLE_TEST_HOOKS
  if (ks->test_next_reissue_secret) {
    sodium_memzero(ks->test_next_reissue_secret, strlen(ks->test_next_reissue_secret));
    g_free(ks->test_next_reissue_secret);
  }
#endif

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
  /* Persisted keys can become fenced at any time on another connection. A
   * raw copy would outlive that transfer, so no persisted key may escape. */
  if (!entry || ks->store) {
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

int signet_key_store_with_signing_key(SignetKeyStore *ks,
                                      const char *agent_id,
                                      const char *client,
                                      SignetKeyStoreCustodyFn fn,
                                      void *user_data) {
  if (!ks || !agent_id || !fn) return -1;
  g_mutex_lock(&ks->mu);
  int rc = -1;
  if (ks->store) {
    rc = signet_store_writer_sign(ks->store, agent_id, client, fn, user_data);
  } else {
    /* Cache-only stores have no persistent fence. */
    SignetCacheEntry *entry = g_hash_table_lookup(ks->cache, agent_id);
    if (entry) rc = fn(entry->secret_key, user_data);
  }
  g_mutex_unlock(&ks->mu);
  return rc;
}

int signet_key_store_writer_acquire(SignetKeyStore *ks, const char *agent_id,
                                    const char *owner, int64_t *out_epoch) {
  if (!ks || !ks->store) return -1;
  g_mutex_lock(&ks->mu);
  int rc = signet_store_writer_acquire(ks->store, agent_id, owner, out_epoch);
  g_mutex_unlock(&ks->mu);
  return rc;
}

int signet_key_store_writer_revoke(SignetKeyStore *ks, const char *agent_id,
                                   int64_t *out_epoch) {
  if (!ks || !ks->store) return -1;
  g_mutex_lock(&ks->mu);
  int rc = signet_store_writer_revoke(ks->store, agent_id, out_epoch);
  g_mutex_unlock(&ks->mu);
  return rc;
}

typedef struct {
  const char *method;
  const char *peer;
  const char *input;
  char *result;
  size_t result_len;
} SignetCryptoWork;

static bool signet_key_store_decode_peer(const char *hex, uint8_t out[32]) {
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    int hi = g_ascii_xdigit_value(hex[i * 2]);
    int lo = g_ascii_xdigit_value(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static int signet_key_store_crypto_callback(const uint8_t key[32], void *data) {
  SignetCryptoWork *work = data;
  const char *m = work->method;
  if (strcmp(m, "nip04_encrypt") == 0 || strcmp(m, "nip04_decrypt") == 0) {
    char sk_hex[65];
    signet_secret_key_to_hex(key, sk_hex);
    char *err = NULL;
    int rc = strcmp(m, "nip04_encrypt") == 0
        ? nostr_nip04_encrypt(work->input, work->peer, sk_hex,
                             &work->result, &err)
        : nostr_nip04_decrypt(work->input, work->peer, sk_hex,
                             &work->result, &err);
    sodium_memzero(sk_hex, sizeof(sk_hex));
    free(err);
    if (work->result) work->result_len = strlen(work->result);
    return rc == 0 && work->result ? 0 : -1;
  }

  uint8_t peer[32];
  if (!signet_key_store_decode_peer(work->peer, peer)) return -1;
  int rc = -1;
  if (strcmp(m, "nip44_encrypt") == 0 ||
      strcmp(m, "nip44_encrypt_b64") == 0) {
    const uint8_t *plain = (const uint8_t *)work->input;
    size_t plain_len = strlen(work->input);
    guchar *decoded = NULL;
    if (strcmp(m, "nip44_encrypt_b64") == 0) {
      size_t encoded_len = plain_len;
      if (!encoded_len || encoded_len % 4) goto done;
      size_t pad = work->input[encoded_len - 1] == '='
          ? (work->input[encoded_len - 2] == '=' ? 2 : 1) : 0;
      for (size_t i = 0; i < encoded_len - pad; i++) {
        char c = work->input[i];
        if (!g_ascii_isalnum(c) && c != '+' && c != '/') goto done;
      }
      gsize decoded_len = 0;
      decoded = g_base64_decode(work->input, &decoded_len);
      if (!decoded || decoded_len != (encoded_len / 4) * 3 - pad) {
        if (decoded) { sodium_memzero(decoded, decoded_len); g_free(decoded); }
        goto done;
      }
      plain = decoded;
      plain_len = decoded_len;
    }
    rc = nostr_nip44_encrypt_v2(key, peer, plain, plain_len,
                                &work->result);
    if (decoded) { sodium_memzero(decoded, plain_len); g_free(decoded); }
  } else if (strcmp(m, "nip44_decrypt") == 0 ||
             strcmp(m, "nip44_decrypt_b64") == 0) {
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    rc = nostr_nip44_decrypt_v2(key, peer, work->input, &raw, &raw_len);
    if (rc == 0 && raw) {
      bool binary_safe = strcmp(m, "nip44_decrypt_b64") == 0;
      if (!binary_safe &&
          (memchr(raw, '\0', raw_len) != NULL ||
           !g_utf8_validate((const char *)raw, raw_len, NULL))) {
        /* Text NIP-44 cannot represent binary plaintext without truncation or
         * invalid JSON. The caller must use nip44_decrypt_b64 instead. */
        rc = -2;
      } else {
        work->result = binary_safe
            ? g_base64_encode(raw, raw_len)
            : g_strndup((const char *)raw, raw_len);
        if (work->result && !binary_safe) work->result_len = raw_len;
      }
    }
    if (raw) { sodium_memzero(raw, raw_len); free(raw); }
  }
done:
  sodium_memzero(peer, sizeof(peer));
  if (rc == 0 && work->result && work->result_len == 0)
    work->result_len = strlen(work->result);
  return rc == -2 ? -2 : (rc == 0 && work->result ? 0 : -1);
}

static int signet_key_store_crypt_in_custody(SignetKeyStore *ks,
                                             const char *agent_id,
                                             const char *client,
                                             const char *method,
                                             const char *peer_pubkey,
                                             const char *input,
                                             char **out_result) {
  if (out_result) *out_result = NULL;
  if (!ks || !agent_id || !method || !peer_pubkey || !input || !out_result)
    return -1;
  SignetCryptoWork work = {
      .method = method, .peer = peer_pubkey, .input = input
  };
  int rc = signet_key_store_with_signing_key(ks, agent_id, client,
                                             signet_key_store_crypto_callback,
                                             &work);
  /* The callback may have completed before the custody transaction fails
   * to commit. No ciphertext or plaintext may escape in that case. */
  if (rc != 0) {
    if (work.result) {
      sodium_memzero(work.result, work.result_len);
      g_free(work.result);
    }
    return rc == -2 ? -2 : -1;
  }
  *out_result = work.result;
  return 0;
}

int signet_key_store_crypt_legacy(SignetKeyStore *ks, const char *agent_id,
                                  const char *method, const char *peer_pubkey,
                                  const char *input, char **out_result) {
  return signet_key_store_crypt_in_custody(ks, agent_id, NULL,
                                           method, peer_pubkey, input,
                                           out_result);
}

int signet_key_store_crypt_nip44(SignetKeyStore *ks, const char *agent_id,
                                 const char *client,
                                 const char *method, const char *peer_pubkey,
                                 const char *input, char **out_result) {
  if (!client || !method ||
      (strcmp(method, "nip44_encrypt") != 0 &&
       strcmp(method, "nip44_decrypt") != 0 &&
       strcmp(method, "nip44_encrypt_b64") != 0 &&
       strcmp(method, "nip44_decrypt_b64") != 0)) {
    if (out_result) *out_result = NULL;
    return -1;
  }
  return signet_key_store_crypt_in_custody(ks, agent_id, client,
                                           method, peer_pubkey, input,
                                           out_result);
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

  /* Store in SQLCipher (with connect_secret + pubkey + provenance). */
  if (ks->store) {
    rc = signet_store_put_agent_ex(ks->store, agent_id, sk_raw, 32, connect_secret,
                                   pk_hex, "provisioned", now);
    /* A uniqueness conflict on a freshly generated random key/secret is a
     * store-integrity problem, not a caller error — normalize to failure. */
    if (rc == 1) rc = -1;
  }

  if (rc == 0 || !ks->store) {
    /* Add to hot cache. */
    SignetCacheEntry *entry = signet_cache_entry_new(sk_raw, now);
    /* Cache-only: the hot cache is the only place the pending pairing
     * secret can live; keep its digest so the bunker URI handed out below
     * can actually pair. */
    if (entry && !ks->store && !signet_cache_set_pending_secret(ks, entry, connect_secret)) {
      signet_cache_entry_free(entry);
      entry = NULL;
    }
    if (entry) {
      signet_ephemeral_bindings_drop_agent(ks, agent_id);
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

SignetAdoptResult signet_key_store_adopt_agent(SignetKeyStore *ks,
                                               const char *agent_id,
                                               const uint8_t secret_key[32],
                                               const char *expected_pubkey_hex,
                                               const char *connect_secret_in,
                                               const char *bunker_pubkey_hex,
                                               const char *const *relay_urls,
                                               size_t n_relay_urls,
                                               char out_pubkey_hex[65],
                                               char **out_bunker_uri) {
  if (!ks || !agent_id || !secret_key || !out_pubkey_hex)
    return SIGNET_ADOPT_ERR_INTERNAL;
  if (out_bunker_uri) *out_bunker_uri = NULL;

  /* Derive the pubkey from the supplied secret. */
  char sk_hex[65];
  for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", secret_key[i]);
  sk_hex[64] = '\0';
  char *pk_hex = nostr_key_get_public(sk_hex);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  if (!pk_hex || strlen(pk_hex) != 64) {
    free(pk_hex);
    return SIGNET_ADOPT_ERR_INVALID_SECRET;
  }

  /* Require the caller-declared pubkey to match exactly. */
  if (expected_pubkey_hex && expected_pubkey_hex[0] &&
      g_ascii_strcasecmp(pk_hex, expected_pubkey_hex) != 0) {
    free(pk_hex);
    return SIGNET_ADOPT_ERR_PUBKEY_MISMATCH;
  }

  /* connect_secret: use the supplied one (any length), else a random 32-byte
   * hex string. Heap-allocated so a caller-supplied value is never silently
   * truncated. */
  char *connect_secret = NULL;
  if (connect_secret_in && connect_secret_in[0]) {
    connect_secret = g_strdup(connect_secret_in);
  } else {
    uint8_t secret_raw[32];
    randombytes_buf(secret_raw, sizeof(secret_raw));
    connect_secret = g_malloc(65);
    for (int i = 0; i < 32; i++) sprintf(connect_secret + i * 2, "%02x", secret_raw[i]);
    connect_secret[64] = '\0';
    sodium_memzero(secret_raw, sizeof(secret_raw));
  }

  SignetAdoptResult result = SIGNET_ADOPT_ERR_INTERNAL;
  int64_t now = (int64_t)time(NULL);
  int rc = -1;
  bool exists = false;

  g_mutex_lock(&ks->mu);

  /* Reject if agent_id already exists (cache or store). Fail closed on a
   * store/decrypt error rather than risk overwriting an existing row. */
  exists = g_hash_table_contains(ks->cache, agent_id);
  if (!exists && ks->store) {
    SignetAgentRecord rec;
    memset(&rec, 0, sizeof(rec));
    int grc = signet_store_get_agent(ks->store, agent_id, &rec);
    if (grc == 0) {
      exists = true;
      signet_agent_record_clear(&rec);
    } else if (grc < 0) {
      result = SIGNET_ADOPT_ERR_INTERNAL;
      goto done;
    }
  }
  if (exists) { result = SIGNET_ADOPT_ERR_AGENT_EXISTS; goto done; }

  /* Reject if the pubkey is already bound to another agent. Fail closed on a
   * store error rather than storing a possibly-colliding key. */
  if (ks->store) {
    bool in_use = false;
    if (signet_store_pubkey_in_use(ks->store, pk_hex, agent_id, &in_use) != 0) {
      result = SIGNET_ADOPT_ERR_INTERNAL;
      goto done;
    }
    if (in_use) { result = SIGNET_ADOPT_ERR_PUBKEY_EXISTS; goto done; }
  }

  /* Store the externally supplied key in the same encrypted path as provisioned
   * agents, tagged provenance=adopted. */
  if (ks->store) {
    rc = signet_store_put_agent_ex(ks->store, agent_id, secret_key, 32,
                                   connect_secret, pk_hex, "adopted", now);
    if (rc == 1) {
      /* DB-level uniqueness fired under the same mutex as the pre-checks, so
       * this is either a pubkey bound to another agent that the (pre-backfill)
       * column check missed, or a caller-supplied connect_secret already in
       * use. Distinguish so the provisioner gets an accurate failure code. */
      bool pk_in_use = false;
      if (signet_store_pubkey_in_use(ks->store, pk_hex, agent_id, &pk_in_use) == 0 &&
          pk_in_use) {
        result = SIGNET_ADOPT_ERR_PUBKEY_EXISTS;
      } else {
        result = SIGNET_ADOPT_ERR_INTERNAL; /* connect_secret collision or other */
      }
      goto done;
    }
  }
  if (rc == 0 || !ks->store) {
    SignetCacheEntry *entry = signet_cache_entry_new(secret_key, now);
    /* Cache-only: keep the pending pairing secret's digest on the entry,
     * unique across agents like the store's connect_secret column. */
    if (entry && !ks->store && !signet_cache_set_pending_secret(ks, entry, connect_secret)) {
      signet_cache_entry_free(entry);
      entry = NULL;
    }
    if (entry) {
      signet_ephemeral_bindings_drop_agent(ks, agent_id);
      g_hash_table_replace(ks->cache, g_strdup(agent_id), entry);
      rc = 0;
    } else {
      rc = -1;
    }
  }
  if (rc != 0) { result = SIGNET_ADOPT_ERR_INTERNAL; goto done; }

  g_strlcpy(out_pubkey_hex, pk_hex, 65);
  result = SIGNET_ADOPT_OK;

  if (out_bunker_uri && bunker_pubkey_hex && bunker_pubkey_hex[0]) {
    GString *uri = g_string_new("bunker://");
    g_string_append(uri, bunker_pubkey_hex);
    g_string_append_c(uri, '?');
    for (size_t i = 0; i < n_relay_urls; i++) {
      if (i > 0) g_string_append_c(uri, '&');
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

done:
  g_mutex_unlock(&ks->mu);
  if (connect_secret) {
    sodium_memzero(connect_secret, strlen(connect_secret));
    g_free(connect_secret);
  }
  free(pk_hex);
  return result;
}

SignetAdoptResult signet_key_store_restore_agent(SignetKeyStore *ks,
                                                 const char *agent_id,
                                                 const uint8_t secret_key[32],
                                                 const char *expected_pubkey_hex,
                                                 char out_pubkey_hex[65]) {
  if (!ks || !agent_id || !agent_id[0] || !secret_key || !out_pubkey_hex)
    return SIGNET_ADOPT_ERR_INTERNAL;

  char sk_hex[65];
  for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", secret_key[i]);
  sk_hex[64] = '\0';
  char *pk_hex = nostr_key_get_public(sk_hex);
  sodium_memzero(sk_hex, sizeof(sk_hex));
  if (!pk_hex || strlen(pk_hex) != 64) {
    free(pk_hex);
    return SIGNET_ADOPT_ERR_INVALID_SECRET;
  }
  if (expected_pubkey_hex && expected_pubkey_hex[0] &&
      g_ascii_strcasecmp(pk_hex, expected_pubkey_hex) != 0) {
    free(pk_hex);
    return SIGNET_ADOPT_ERR_PUBKEY_MISMATCH;
  }

  SignetAdoptResult result = SIGNET_ADOPT_ERR_INTERNAL;
  SignetCacheEntry *replacement = NULL;
  g_mutex_lock(&ks->mu);

  bool exists = g_hash_table_contains(ks->cache, agent_id);
  if (!exists && ks->store) {
    SignetAgentRecord rec;
    memset(&rec, 0, sizeof(rec));
    int grc = signet_store_get_agent(ks->store, agent_id, &rec);
    if (grc == 0) {
      exists = true;
      signet_agent_record_clear(&rec);
    } else if (grc < 0) {
      goto done;
    }
  }
  if (!exists) {
    result = SIGNET_ADOPT_ERR_AGENT_NOT_FOUND;
    goto done;
  }

  /* Prepare the fallible mlock-backed cache entry before changing SQL state.
   * Once the durable update succeeds, the cache swap itself is infallible. */
  replacement = signet_cache_entry_new(secret_key, (int64_t)time(NULL));
  if (!replacement) goto done;

  if (ks->store) {
    int rc = signet_store_restore_agent_key(
        ks->store, agent_id, secret_key, pk_hex, "restored");
    if (rc == 1) {
      bool in_use = false;
      if (signet_store_pubkey_in_use(ks->store, pk_hex, agent_id, &in_use) == 0 &&
          in_use)
        result = SIGNET_ADOPT_ERR_PUBKEY_EXISTS;
      else
        result = SIGNET_ADOPT_ERR_AGENT_NOT_FOUND;
      goto done;
    }
    if (rc != 0) goto done;
  }

  /* Cache-only: like the store row, a restore keeps the agent's pending
   * connect secret (if any); reissue-connect mints one otherwise. */
  if (!ks->store) {
    const SignetCacheEntry *prior = g_hash_table_lookup(ks->cache, agent_id);
    if (prior)
      memcpy(replacement->connect_secret_hash, prior->connect_secret_hash,
             sizeof(replacement->connect_secret_hash));
  }
  signet_ephemeral_bindings_drop_agent(ks, agent_id);
  g_hash_table_replace(ks->cache, g_strdup(agent_id), replacement);
  replacement = NULL;
  g_strlcpy(out_pubkey_hex, pk_hex, 65);
  result = SIGNET_ADOPT_OK;

done:
  if (replacement) signet_cache_entry_free(replacement);
  g_mutex_unlock(&ks->mu);
  free(pk_hex);
  return result;
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

/* ------------------- cache-only NIP-46 client bindings ------------------- */

/* Lowercase a 64-hex pubkey. Returns false if malformed. */
static bool signet_ks_canonical_pubkey(const char *pubkey_hex, char out[65]) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return false;
  for (int i = 0; i < 64; i++) {
    if (!g_ascii_isxdigit(pubkey_hex[i])) return false;
    out[i] = (char)g_ascii_tolower(pubkey_hex[i]);
  }
  out[64] = '\0';
  return true;
}

typedef struct {
  SignetStore *store;
  const char *agent_id;
  const char *client;
  const char *secret_hash;
  SignetKeyStoreCustodyFn fn;
  void *data;
} SignetBoundCustody;

static int signet_bound_custody_callback(const uint8_t key[32], void *data) {
  SignetBoundCustody *work = data;
  char *current_agent = NULL, *current_hash = NULL;
  int rc = signet_store_lookup_client_binding(work->store, work->client,
                                               (int64_t)time(NULL),
                                               &current_agent, &current_hash);
  bool hash_matches = (!current_hash && !work->secret_hash) ||
      (current_hash && work->secret_hash &&
       strlen(current_hash) == 64 && strlen(work->secret_hash) == 64 &&
       sodium_memcmp(current_hash, work->secret_hash, 64) == 0);
  bool valid = rc == 0 && g_strcmp0(current_agent, work->agent_id) == 0 &&
               hash_matches;
  g_free(current_agent);
  if (current_hash) { sodium_memzero(current_hash, strlen(current_hash)); g_free(current_hash); }
  return valid ? work->fn(key, work->data) : -3;
}

int signet_key_store_with_bound_session(SignetKeyStore *ks, const char *agent_id,
                                        const char *client_pubkey,
                                        const char *bound_secret_hash,
                                        uint64_t generation,
                                        int (*fn)(void *), void *data) {
  if (!ks || !agent_id || !client_pubkey || !fn) return -1;
  char client[65];
  if (!signet_ks_canonical_pubkey(client_pubkey, client)) return -3;
  g_mutex_lock(&ks->mu);
  int rc = -3;
  if (ks->store) {
    /* FIDO may start its own store transaction, so hold the connection mutex
     * (not a nested writer transaction) across validation and callback. */
    sqlite3 *db = signet_store_get_db(ks->store);
    if (db) {
      sqlite3_mutex_enter(sqlite3_db_mutex(db));
      char *current_agent = NULL, *current_hash = NULL;
      int lookup = signet_store_lookup_client_binding(ks->store, client,
          (int64_t)time(NULL), &current_agent, &current_hash);
      bool hash_matches = (!current_hash && !bound_secret_hash) ||
          (current_hash && bound_secret_hash && strlen(current_hash) == 64 &&
           strlen(bound_secret_hash) == 64 &&
           sodium_memcmp(current_hash, bound_secret_hash, 64) == 0);
      if (lookup == 0 && g_strcmp0(current_agent, agent_id) == 0 && hash_matches)
        rc = fn(data);
      g_free(current_agent);
      if (current_hash) { sodium_memzero(current_hash, strlen(current_hash)); g_free(current_hash); }
      sqlite3_mutex_leave(sqlite3_db_mutex(db));
    }
  } else {
    const SignetEphemeralBinding *b = g_hash_table_lookup(ks->ephemeral_bindings, client);
    const SignetCacheEntry *e = g_hash_table_lookup(ks->cache, agent_id);
    if (b && e && generation != 0 && b->generation == generation &&
        e->generation == generation && g_strcmp0(b->agent_id, agent_id) == 0)
      rc = fn(data);
  }
  g_mutex_unlock(&ks->mu);
  return rc;
}

static int signet_key_store_with_bound_key_impl(SignetKeyStore *ks, const char *agent_id,
                                    const char *client_pubkey,
                                    const char *bound_secret_hash,
                                    uint64_t generation,
                                    bool legacy_only,
                                    SignetKeyStoreCustodyFn fn, void *user_data) {
  if (!ks || !agent_id || !client_pubkey || !fn) return -1;
  char client[65];
  if (!signet_ks_canonical_pubkey(client_pubkey, client)) return -3;
  /* Lock order: ks->mu -> SQLite connection mutex/transaction. Never call
   * back into the key store from fn. Entry/binding pointers never escape. */
  g_mutex_lock(&ks->mu);
  int rc = -3;
  if (ks->store) {
    SignetBoundCustody work = {ks->store, agent_id, client,
                               bound_secret_hash, fn, user_data};
    rc = signet_store_writer_sign(ks->store, agent_id,
                                  legacy_only ? NULL : client,
                                  signet_bound_custody_callback, &work);
  } else {
    const SignetEphemeralBinding *b = g_hash_table_lookup(ks->ephemeral_bindings, client);
    const SignetCacheEntry *e = g_hash_table_lookup(ks->cache, agent_id);
    if (b && e && generation != 0 && b->generation == generation &&
        e->generation == generation && g_strcmp0(b->agent_id, agent_id) == 0)
      rc = fn(e->secret_key, user_data);
  }
  g_mutex_unlock(&ks->mu);
  return rc;
}

int signet_key_store_with_bound_key(SignetKeyStore *ks, const char *agent_id,
                                    const char *client_pubkey,
                                    const char *bound_secret_hash,
                                    uint64_t generation,
                                    SignetKeyStoreCustodyFn fn, void *user_data) {
  return signet_key_store_with_bound_key_impl(ks, agent_id, client_pubkey,
      bound_secret_hash, generation, false, fn, user_data);
}

int signet_key_store_crypt_bound(SignetKeyStore *ks, const char *agent_id,
                                 const char *client_pubkey,
                                 const char *bound_secret_hash,
                                 uint64_t generation, const char *method,
                                 const char *peer_pubkey, const char *input,
                                 char **out_result) {
  if (out_result) *out_result = NULL;
  if (!method || !peer_pubkey || !input || !out_result) return -1;
  SignetCryptoWork work = {.method = method, .peer = peer_pubkey, .input = input};
  bool legacy_only = g_str_has_prefix(method, "nip04_");
  int rc = signet_key_store_with_bound_key_impl(ks, agent_id, client_pubkey,
      bound_secret_hash, generation, legacy_only,
      signet_key_store_crypto_callback, &work);
  if (rc != 0) {
    if (work.result) { sodium_memzero(work.result, work.result_len); g_free(work.result); }
    return rc;
  }
  *out_result = work.result;
  return 0;
}

typedef struct {
  SignetKeyStore *ks;
  const char *agent_id;
  char *out;
} SignetBoundPubkeyWork;

static int signet_bound_pubkey_callback(void *data) {
  SignetBoundPubkeyWork *w = data;
  if (w->ks->store) {
    SignetAgentMeta meta = {0};
    int rc = signet_store_get_agent_meta(w->ks->store, w->agent_id, &meta);
    if (rc == 0 && meta.pubkey && strlen(meta.pubkey) == 64)
      memcpy(w->out, meta.pubkey, 65);
    signet_agent_meta_clear(&meta);
    return w->out[0] ? 0 : -1;
  }
  const SignetCacheEntry *e = g_hash_table_lookup(w->ks->cache, w->agent_id);
  if (!e) return -1;
  memcpy(w->out, e->pubkey_hex, 65);
  return 0;
}

int signet_key_store_get_bound_pubkey(SignetKeyStore *ks, const char *agent_id,
                                      const char *client_pubkey,
                                      const char *bound_secret_hash,
                                      uint64_t generation, char out[65]) {
  if (!out) return -1;
  out[0] = '\0';
  SignetBoundPubkeyWork work = {ks, agent_id, out};
  return signet_key_store_with_bound_session(ks, agent_id, client_pubkey,
      bound_secret_hash, generation, signet_bound_pubkey_callback, &work);
}

int signet_key_store_ephemeral_pair_client(SignetKeyStore *ks,
                                           const char *connect_secret,
                                           const char *client_pubkey_hex,
                                           char **out_agent_id) {
  if (out_agent_id) *out_agent_id = NULL;
  if (!ks || !connect_secret || !connect_secret[0] || !out_agent_id) return -1;

  char client[65];
  char hash[65];
  if (!signet_ks_canonical_pubkey(client_pubkey_hex, client)) return -1;
  if (!signet_connect_secret_hash(connect_secret, hash)) return -1;

  g_mutex_lock(&ks->mu);
  if (ks->store) {
    g_mutex_unlock(&ks->mu);
    sodium_memzero(hash, sizeof(hash));
    return -1;
  }

  /* Find the one agent holding this pending secret. Adopt/provision keep the
   * digests unique; compare every entry without an early exit. */
  const char *match_id = NULL;
  SignetCacheEntry *match = NULL;
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, ks->cache);
  while (g_hash_table_iter_next(&it, &k, &v)) {
    SignetCacheEntry *e = v;
    if (e->connect_secret_hash[0] &&
        sodium_memcmp(e->connect_secret_hash, hash, 64) == 0) {
      match_id = k;
      match = e;
    }
  }
  if (!match) {
    g_mutex_unlock(&ks->mu);
    sodium_memzero(hash, sizeof(hash));
    return 1;
  }

  SignetEphemeralBinding *b = g_new0(SignetEphemeralBinding, 1);
  b->agent_id = g_strdup(match_id);
  b->generation = match->generation;
  memcpy(b->secret_hash, hash, 65);
  char *agent_copy = g_strdup(match_id);

  /* Consume and bind together: the secret is single-use, and a re-pair of an
   * already bound client key replaces its earlier binding. */
  sodium_memzero(match->connect_secret_hash, sizeof(match->connect_secret_hash));
  g_hash_table_add(ks->spent_secret_hashes, g_strdup(hash));
  g_hash_table_replace(ks->ephemeral_bindings, g_strdup(client), b);
  g_mutex_unlock(&ks->mu);
  sodium_memzero(hash, sizeof(hash));

  *out_agent_id = agent_copy;
  return 0;
}

unsigned int signet_key_store_ephemeral_binding_count(SignetKeyStore *ks) {
  if (!ks) return 0;
  g_mutex_lock(&ks->mu);
  unsigned int n = ks->ephemeral_bindings ? g_hash_table_size(ks->ephemeral_bindings) : 0;
  g_mutex_unlock(&ks->mu);
  return n;
}

int signet_key_store_ephemeral_lookup_client(SignetKeyStore *ks,
                                             const char *client_pubkey_hex,
                                             char **out_agent_id,
                                             char **out_bound_secret_hash,
                                             uint64_t *out_generation) {
  if (out_agent_id) *out_agent_id = NULL;
  if (out_bound_secret_hash) *out_bound_secret_hash = NULL;
  if (out_generation) *out_generation = 0;
  if (!ks || !out_agent_id) return -1;

  char client[65];
  if (!signet_ks_canonical_pubkey(client_pubkey_hex, client)) return 1;

  g_mutex_lock(&ks->mu);
  if (ks->store) {
    g_mutex_unlock(&ks->mu);
    return -1;
  }

  SignetEphemeralBinding *b = g_hash_table_lookup(ks->ephemeral_bindings, client);
  if (!b) {
    g_mutex_unlock(&ks->mu);
    return 1;
  }

  /* The binding conveys authority only toward the exact identity it was made
   * against: a revoked agent has no entry, and rotate-key or a re-adopt or
   * re-provision under the same agent_id installs a new entry. */
  const SignetCacheEntry *e = g_hash_table_lookup(ks->cache, b->agent_id);
  if (!e || e->generation != b->generation) {
    g_hash_table_remove(ks->ephemeral_bindings, client);
    g_mutex_unlock(&ks->mu);
    return 1;
  }

  *out_agent_id = g_strdup(b->agent_id);
  if (out_bound_secret_hash) *out_bound_secret_hash = g_strdup(b->secret_hash);
  if (out_generation) *out_generation = b->generation;
  g_mutex_unlock(&ks->mu);
  return 0;
}

int signet_key_store_reissue_connect_secret(SignetKeyStore *ks,
                                            const char *agent_id,
                                            const char *expected_user_pubkey,
                                            const char *bunker_pubkey_hex,
                                            const char *const *relay_urls,
                                            size_t n_relay_urls,
                                            char out_pubkey_hex[65],
                                            char **out_connect_secret,
                                            char **out_bunker_uri) {
  if (out_connect_secret) *out_connect_secret = NULL;
  if (out_bunker_uri) *out_bunker_uri = NULL;
  if (!ks || !agent_id || !agent_id[0] || !out_pubkey_hex || !out_connect_secret)
    return -1;

  /* Generate a fresh random connect_secret (32 bytes hex = 64 chars). */
  uint8_t secret_raw[32];
  randombytes_buf(secret_raw, sizeof(secret_raw));
  char connect_secret[65];
  for (int i = 0; i < 32; i++) sprintf(connect_secret + i * 2, "%02x", secret_raw[i]);
  connect_secret[64] = '\0';
  sodium_memzero(secret_raw, sizeof(secret_raw));

  int rc = -1;
  char *pk_hex = NULL;

  g_mutex_lock(&ks->mu);
#ifdef SIGNET_ENABLE_TEST_HOOKS
  if (ks->test_next_reissue_secret) {
    g_strlcpy(connect_secret, ks->test_next_reissue_secret, sizeof(connect_secret));
    sodium_memzero(ks->test_next_reissue_secret, strlen(ks->test_next_reissue_secret));
    g_free(ks->test_next_reissue_secret);
    ks->test_next_reissue_secret = NULL;
  }
#endif

  /* The agent must exist. With a store, load the record so we can derive the
   * identity pubkey even for legacy rows without a populated pubkey column;
   * a cache-only key store holds agents only in the hot cache. */
  SignetCacheEntry *cached = NULL;
  if (ks->store) {
    SignetAgentRecord rec;
    memset(&rec, 0, sizeof(rec));
    int grc = signet_store_get_agent(ks->store, agent_id, &rec);
    if (grc != 0) {
      g_mutex_unlock(&ks->mu);
      sodium_memzero(connect_secret, sizeof(connect_secret));
      return (grc > 0) ? 1 : -1;
    }

    if (rec.secret_key && rec.secret_key_len == 32) {
      char sk_hex[65];
      for (int i = 0; i < 32; i++) sprintf(sk_hex + i * 2, "%02x", rec.secret_key[i]);
      sk_hex[64] = '\0';
      pk_hex = nostr_key_get_public(sk_hex);
      sodium_memzero(sk_hex, sizeof(sk_hex));
    }
    signet_agent_record_clear(&rec);
  } else {
    cached = g_hash_table_lookup(ks->cache, agent_id);
    if (!cached) {
      g_mutex_unlock(&ks->mu);
      sodium_memzero(connect_secret, sizeof(connect_secret));
      return 1;
    }
    pk_hex = strdup(cached->pubkey_hex);
  }

  if (!pk_hex || strlen(pk_hex) != 64) {
    g_mutex_unlock(&ks->mu);
    sodium_memzero(connect_secret, sizeof(connect_secret));
    free(pk_hex);
    return -1;
  }

  /* Identity binding, checked ATOMICALLY with the mutation (same mutex hold):
   * when the caller pins an expected identity (self-service authorization),
   * a concurrent rotate between the caller's auth check and this call must
   * not let the OLD key mint a secret for the NEW identity. */
  if (expected_user_pubkey && expected_user_pubkey[0] &&
      g_ascii_strcasecmp(pk_hex, expected_user_pubkey) != 0) {
    g_mutex_unlock(&ks->mu);
    sodium_memzero(connect_secret, sizeof(connect_secret));
    free(pk_hex);
    return 2; /* identity mismatch */
  }

  if (ks->store) {
    int64_t now = (int64_t)time(NULL);
    rc = signet_store_reissue_connect_secret(ks->store, agent_id, connect_secret, now);
  } else {
    /* Cache-only: replace the pending secret's digest in place. Existing
     * bindings stay (as with the store), only the earlier secret dies. */
    rc = signet_cache_set_pending_secret(ks, cached, connect_secret) ? 0 : -1;
  }

  g_mutex_unlock(&ks->mu);

  if (rc != 0) {
    sodium_memzero(connect_secret, sizeof(connect_secret));
    free(pk_hex);
    return (rc > 0) ? 1 : -1;
  }

  memcpy(out_pubkey_hex, pk_hex, 65);
  *out_connect_secret = g_strdup(connect_secret);

  /* Build bunker:// URI embedding the fresh secret, if requested.
   * Format: bunker://<bunker_pubkey>?relay=<url1>&relay=<url2>&secret=<connect_secret> */
  if (out_bunker_uri && bunker_pubkey_hex && bunker_pubkey_hex[0]) {
    GString *uri = g_string_new("bunker://");
    g_string_append(uri, bunker_pubkey_hex);
    g_string_append_c(uri, '?');
    for (size_t i = 0; i < n_relay_urls; i++) {
      if (i > 0) g_string_append_c(uri, '&');
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

  sodium_memzero(connect_secret, sizeof(connect_secret));
  free(pk_hex);
  return 0;
}

int signet_key_store_evict_agent(SignetKeyStore *ks, const char *agent_id) {
  if (!ks || !agent_id) return -1;

  g_mutex_lock(&ks->mu);
  signet_ephemeral_bindings_drop_agent(ks, agent_id);
  gboolean found = g_hash_table_remove(ks->cache, agent_id);
  g_mutex_unlock(&ks->mu);
  return found ? 0 : 1;
}

int signet_key_store_revoke_agent(SignetKeyStore *ks, const char *agent_id) {
  if (!ks || !agent_id) return -1;

  g_mutex_lock(&ks->mu);

  /* Delete durable state first so a store failure never leaves a live row with
   * its corresponding hot key already wiped. */
  int store_rc = 0;
  if (ks->store) {
    store_rc = signet_store_delete_agent(ks->store, agent_id);
    if (store_rc < 0) {
      g_mutex_unlock(&ks->mu);
      return -1;
    }
  }

  signet_ephemeral_bindings_drop_agent(ks, agent_id);
  gboolean found = g_hash_table_remove(ks->cache, agent_id);
  g_mutex_unlock(&ks->mu);

  if (!found && store_rc == 1) return 1; /* not found anywhere */
  return 0;
}

int signet_key_store_rotate_agent(SignetKeyStore *ks,
                                   const char *agent_id,
                                   char *out_pubkey_hex,
                                   size_t out_pubkey_hex_sz) {
  if (!ks || !agent_id || !out_pubkey_hex || out_pubkey_hex_sz < 65) return -1;

  g_mutex_lock(&ks->mu);

  /* The agent must exist. Check the hot cache first, then fall back to the
   * backing store so agents that were provisioned but not yet loaded into the
   * cache can still be rotated (previously they returned not_found). */
  if (!g_hash_table_contains(ks->cache, agent_id)) {
    int exists_in_store = 0;
    if (ks->store) {
      SignetAgentRecord rec;
      memset(&rec, 0, sizeof(rec));
      if (signet_store_get_agent(ks->store, agent_id, &rec) == 0) {
        exists_in_store = 1;
        signet_agent_record_clear(&rec);
      }
    }
    if (!exists_in_store) {
      g_mutex_unlock(&ks->mu);
      return 1; /* not found in cache or store */
    }
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

  /* Replace in SQLCipher (connect_secret = NULL for rotated keys). Record the
   * new pubkey + provenance so rotated agents stay visible to pubkey-collision
   * checks (adopt-existing) and their new identity is tracked. */
  if (ks->store) {
    rc = signet_store_put_agent_ex(ks->store, agent_id, sk_raw, 32, NULL,
                                   pk_hex, "rotated", now);
    /* A uniqueness conflict on a freshly generated random key is a
     * store-integrity problem — report rotate_failed, not not_found. */
    if (rc == 1) rc = -1;
  }

  if (rc == 0 || !ks->store) {
    /* Replace in hot cache. Like the store row (connect_secret = NULL), the
     * new entry has no pending secret: re-pairing takes reissue-connect. */
    SignetCacheEntry *entry = signet_cache_entry_new(sk_raw, now);
    if (entry) {
      signet_ephemeral_bindings_drop_agent(ks, agent_id);
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

int signet_key_store_backfill_pubkeys(SignetKeyStore *ks,
                                      size_t *out_updated,
                                      size_t *out_failed) {
  if (out_updated) *out_updated = 0;
  if (out_failed) *out_failed = 0;
  if (!ks) return -1;

  g_mutex_lock(&ks->mu);

  if (!ks->store) {
    /* Cache-only mode: nothing persisted, nothing to backfill. */
    g_mutex_unlock(&ks->mu);
    return 0;
  }

  char **ids = NULL;
  size_t count = 0;
  if (signet_store_list_agents_missing_pubkey(ks->store, &ids, &count) != 0) {
    g_mutex_unlock(&ks->mu);
    return -1;
  }

  size_t updated = 0, failed = 0;
  for (size_t i = 0; i < count; i++) {
    /* Decrypt the custody key and derive the identity pubkey. A row whose
     * secret cannot be decrypted (wrong/rotated DEK, corrupt blob) is counted
     * as failed and left untouched — `signetctl list-agents --verify` surfaces
     * those. */
    SignetAgentRecord rec;
    memset(&rec, 0, sizeof(rec));
    if (signet_store_get_agent(ks->store, ids[i], &rec) != 0) {
      failed++;
      continue;
    }

    char *pk_hex = NULL;
    if (rec.secret_key && rec.secret_key_len == 32) {
      char sk_hex[65];
      for (int b = 0; b < 32; b++) sprintf(sk_hex + b * 2, "%02x", rec.secret_key[b]);
      sk_hex[64] = '\0';
      pk_hex = nostr_key_get_public(sk_hex);
      sodium_memzero(sk_hex, sizeof(sk_hex));
    }
    signet_agent_record_clear(&rec);

    if (!pk_hex || strlen(pk_hex) != 64) {
      /* Could not derive a pubkey from the decrypted secret. */
      failed++;
      free(pk_hex);
      continue;
    }

    int src = signet_store_set_agent_pubkey(ks->store, ids[i], pk_hex);
    free(pk_hex);
    if (src == 0) {
      updated++;
    } else if (src == 1) {
      /* Row vanished or was populated concurrently — benign, not a failure. */
    } else if (src == 2) {
      /* This custody key is already bound to another agent (duplicate legacy
       * rows). Count per-row and keep going — one duplicated key must not
       * abort the backfill for the rest of the fleet. */
      failed++;
    } else {
      /* Store write error: abort the pass rather than mislabeling a database
       * problem as a per-row derivation failure. */
      g_mutex_unlock(&ks->mu);
      signet_store_free_agent_ids(ids, count);
      if (out_updated) *out_updated = updated;
      if (out_failed) *out_failed = failed;
      return -1;
    }
  }

  g_mutex_unlock(&ks->mu);

  signet_store_free_agent_ids(ids, count);
  if (out_updated) *out_updated = updated;
  if (out_failed) *out_failed = failed;
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
