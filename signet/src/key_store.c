/* SPDX-License-Identifier: MIT
 *
 * key_store.c - Key custody (Vault-backed).
 */

#include "signet/key_store.h"
#include "signet/audit_logger.h"
#include "signet/vault_client.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "glib.h"
#include "json-glib/json-glib.h"

typedef struct {
  nostr_secure_buf secret_key;
  int64_t loaded_at;
  int64_t expires_at;
} SignetCachedKey;

struct SignetKeyStore {
  SignetVaultClient *vault;
  SignetAuditLogger *audit;

  uint32_t cache_ttl_seconds;

  char *kv_mount;
  char *key_prefix;
  char *secret_key_field;

  GHashTable *cache; /* identity -> SignetCachedKey* (optional; only if ttl > 0) */
  GMutex mu;
};

static void signet_cached_key_free(SignetCachedKey *ck) {
  if (!ck) return;
  secure_free(&ck->secret_key);
  ck->loaded_at = 0;
  ck->expires_at = 0;
  g_free(ck);
}

static const char *signet_nonempty_or_default(const char *s, const char *def) {
  if (s && s[0] != '\0') return s;
  return def;
}

static char *signet_join_path2(const char *a, const char *b) {
  if (!a) a = "";
  if (!b) b = "";

  /* Strip extra slashes at boundaries */
  while (*a == '/') a++;
  while (*b == '/') b++;

  size_t al = strlen(a);
  size_t bl = strlen(b);

  while (al > 0 && a[al - 1] == '/') al--;

  if (al == 0 && bl == 0) return NULL;
  if (al == 0) return g_strdup(b);
  if (bl == 0) return g_strndup(a, al);

  char *out = g_malloc(al + 1 + bl + 1);
  if (!out) return NULL;

  memcpy(out, a, al);
  out[al] = '/';
  memcpy(out + al + 1, b, bl);
  out[al + 1 + bl] = '\0';
  return out;
}

static void signet_audit_key_access(SignetKeyStore *ks,
                                    const char *identity,
                                    const char *vault_mount,
                                    const char *vault_path,
                                    const char *result,
                                    long http_status,
                                    const char *error_code) {
  if (!ks || !ks->audit) return;

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  if (identity && identity[0] != '\0') {
    json_builder_set_member_name(b, "identity");
    json_builder_add_string_value(b, identity);
  }
  if (vault_mount && vault_mount[0] != '\0') {
    json_builder_set_member_name(b, "vault_mount");
    json_builder_add_string_value(b, vault_mount);
  }
  if (vault_path && vault_path[0] != '\0') {
    json_builder_set_member_name(b, "vault_path");
    json_builder_add_string_value(b, vault_path);
  }
  if (result && result[0] != '\0') {
    json_builder_set_member_name(b, "result");
    json_builder_add_string_value(b, result);
  }
  if (http_status != 0) {
    json_builder_set_member_name(b, "http_status");
    json_builder_add_int_value(b, (gint64)http_status);
  }
  if (error_code && error_code[0] != '\0') {
    json_builder_set_member_name(b, "error_code");
    json_builder_add_string_value(b, error_code);
  }

  json_builder_end_object(b);

  JsonNode *root = json_builder_get_root(b);
  if (!root) return;

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);

  gchar *payload = json_generator_to_data(gen, NULL);
  json_node_unref(root);

  if (!payload) return;

  (void)signet_audit_log_json(ks->audit, SIGNET_AUDIT_EVENT_KEY_ACCESS, payload);
  g_free(payload);
}

static int signet_hexval(unsigned char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
  return -1;
}

static bool signet_hex_decode_32(const char *hex, uint8_t out32[32]) {
  if (!hex || !out32) return false;

  if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;

  size_t n = strlen(hex);
  if (n != 64) return false;

  for (size_t i = 0; i < 32; i++) {
    int hi = signet_hexval((unsigned char)hex[i * 2]);
    int lo = signet_hexval((unsigned char)hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out32[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static const char *signet_json_get_string_member0(JsonObject *o, const char *name) {
  if (!o || !name) return NULL;
  if (!json_object_has_member(o, name)) return NULL;
  return json_object_get_string_member(o, name);
}

static const char *signet_vault_kv2_extract_field(const char *json_body,
                                                  const char *field_name) {
  if (!json_body || json_body[0] == '\0' || !field_name || field_name[0] == '\0') return NULL;

  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, json_body, -1, NULL)) return NULL;

  JsonNode *root = json_parser_get_root(p);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) return NULL;

  JsonObject *root_o = json_node_get_object(root);
  if (!root_o) return NULL;

  if (!json_object_has_member(root_o, "data")) return NULL;
  JsonObject *data_o = json_object_get_object_member(root_o, "data");
  if (!data_o) return NULL;

  if (!json_object_has_member(data_o, "data")) return NULL;
  JsonObject *data_data_o = json_object_get_object_member(data_o, "data");
  if (!data_data_o) return NULL;

  return signet_json_get_string_member0(data_data_o, field_name);
}

SignetKeyStore *signet_key_store_new(SignetVaultClient *vault,
                                     SignetAuditLogger *audit,
                                     const SignetKeyStoreConfig *cfg) {
  if (!vault) return NULL;

  const char *mount = signet_nonempty_or_default(cfg ? cfg->vault_kv_mount : NULL, "secret");
  const char *prefix = signet_nonempty_or_default(cfg ? cfg->key_prefix : NULL, "signet/keys");
  const char *field = signet_nonempty_or_default(cfg ? cfg->secret_key_field : NULL, "secp256k1_sk_hex");
  uint32_t ttl = cfg ? cfg->cache_ttl_seconds : 0;

  SignetKeyStore *ks = (SignetKeyStore *)calloc(1, sizeof(*ks));
  if (!ks) return NULL;

  g_mutex_init(&ks->mu);

  ks->vault = vault;
  ks->audit = audit;
  ks->cache_ttl_seconds = ttl;

  ks->kv_mount = g_strdup(mount);
  ks->key_prefix = g_strdup(prefix);
  ks->secret_key_field = g_strdup(field);

  if (!ks->kv_mount || !ks->key_prefix || !ks->secret_key_field) {
    signet_key_store_free(ks);
    return NULL;
  }

  if (ks->cache_ttl_seconds > 0) {
    ks->cache = g_hash_table_new_full(g_str_hash,
                                      g_str_equal,
                                      g_free,
                                      (GDestroyNotify)signet_cached_key_free);
    if (!ks->cache) {
      signet_key_store_free(ks);
      return NULL;
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
  g_mutex_unlock(&ks->mu);

  g_free(ks->kv_mount);
  g_free(ks->key_prefix);
  g_free(ks->secret_key_field);

  g_mutex_clear(&ks->mu);
  free(ks);
}

bool signet_key_store_load_identity_key(SignetKeyStore *ks,
                                        const char *identity,
                                        SignetLoadedKey *out_key) {
  if (!ks || !identity || identity[0] == '\0' || !out_key) return false;
  memset(out_key, 0, sizeof(*out_key));

  int64_t now = (int64_t)time(NULL);

  /* Cache fast-path (if enabled). */
  if (ks->cache && ks->cache_ttl_seconds > 0) {
    g_mutex_lock(&ks->mu);

    SignetCachedKey *ck = (SignetCachedKey *)g_hash_table_lookup(ks->cache, identity);
    if (ck) {
      if (ck->expires_at > 0 && now >= ck->expires_at) {
        (void)g_hash_table_remove(ks->cache, identity);
        ck = NULL;
      }
    }

    if (ck && ck->secret_key.ptr && ck->secret_key.len == 32) {
      out_key->secret_key = secure_alloc(ck->secret_key.len);
      if (out_key->secret_key.ptr && out_key->secret_key.len == ck->secret_key.len) {
        memcpy(out_key->secret_key.ptr, ck->secret_key.ptr, ck->secret_key.len);
        out_key->loaded_at = now;
        out_key->expires_at = now + (int64_t)ks->cache_ttl_seconds;

        g_mutex_unlock(&ks->mu);

        char *vault_path = signet_join_path2(ks->key_prefix, identity);
        signet_audit_key_access(ks, identity, ks->kv_mount, vault_path, "ok(cache_hit)", 0, NULL);
        g_free(vault_path);

        return true;
      }

      g_mutex_unlock(&ks->mu);
      secure_free(&out_key->secret_key);
      memset(out_key, 0, sizeof(*out_key));
      signet_audit_key_access(ks, identity, ks->kv_mount, NULL, "error", 0, "cache_copy_failed");
      return false;
    }

    g_mutex_unlock(&ks->mu);
  }

  char *vault_path = signet_join_path2(ks->key_prefix, identity);
  if (!vault_path) {
    signet_audit_key_access(ks, identity, ks->kv_mount, NULL, "error", 0, "path_build_failed");
    return false;
  }

  SignetVaultResponse resp;
  memset(&resp, 0, sizeof(resp));

  bool ok = signet_vault_kv2_read(ks->vault, ks->kv_mount, vault_path, &resp);
  if (!ok) {
    signet_audit_key_access(ks, identity, ks->kv_mount, vault_path, "error", resp.http_status, "vault_read_failed");
    signet_vault_response_clear(&resp);
    g_free(vault_path);
    return false;
  }

  const char *sk_hex = signet_vault_kv2_extract_field(resp.body, ks->secret_key_field);
  if (!sk_hex || sk_hex[0] == '\0') {
    signet_audit_key_access(ks, identity, ks->kv_mount, vault_path, "error", resp.http_status, "missing_key_field");
    signet_vault_response_clear(&resp);
    g_free(vault_path);
    return false;
  }

  uint8_t sk32[32];
  if (!signet_hex_decode_32(sk_hex, sk32)) {
    signet_audit_key_access(ks, identity, ks->kv_mount, vault_path, "error", resp.http_status, "invalid_key_hex");
    secure_wipe(sk32, sizeof(sk32));
    signet_vault_response_clear(&resp);
    g_free(vault_path);
    return false;
  }

  out_key->secret_key = secure_alloc(32);
  if (!out_key->secret_key.ptr || out_key->secret_key.len != 32) {
    signet_audit_key_access(ks, identity, ks->kv_mount, vault_path, "error", resp.http_status, "secure_alloc_failed");
    secure_wipe(sk32, sizeof(sk32));
    signet_vault_response_clear(&resp);
    g_free(vault_path);
    memset(out_key, 0, sizeof(*out_key));
    return false;
  }

  memcpy(out_key->secret_key.ptr, sk32, 32);
  secure_wipe(sk32, sizeof(sk32));

  out_key->loaded_at = now;
  out_key->expires_at = (ks->cache_ttl_seconds > 0) ? (now + (int64_t)ks->cache_ttl_seconds) : 0;

  /* Cache population (if enabled). */
  if (ks->cache && ks->cache_ttl_seconds > 0) {
    SignetCachedKey *ck = g_new0(SignetCachedKey, 1);
    if (ck) {
      ck->secret_key = secure_alloc(32);
      if (ck->secret_key.ptr && ck->secret_key.len == 32) {
        memcpy(ck->secret_key.ptr, out_key->secret_key.ptr, 32);
        ck->loaded_at = now;
        ck->expires_at = out_key->expires_at;

        g_mutex_lock(&ks->mu);
        g_hash_table_replace(ks->cache, g_strdup(identity), ck);
        g_mutex_unlock(&ks->mu);
      } else {
        signet_cached_key_free(ck);
      }
    }
  }

  signet_audit_key_access(ks, identity, ks->kv_mount, vault_path, "ok", resp.http_status, NULL);

  signet_vault_response_clear(&resp);
  g_free(vault_path);
  return true;
}

void signet_loaded_key_clear(SignetLoadedKey *k) {
  if (!k) return;
  secure_free(&k->secret_key);
  k->loaded_at = 0;
  k->expires_at = 0;
}