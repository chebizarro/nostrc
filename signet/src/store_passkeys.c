/* SPDX-License-Identifier: MIT */
/*
 * store_passkeys.c - Dedicated passkey credential vault for Signet.
 */

#include "signet/store_passkeys.h"
#include "signet/store.h"
#include "store_passkeys_schema.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/sha.h>
#include <sqlite3.h>
#include <sodium.h>

#define SIGNET_PASSKEY_NONCE_LEN crypto_secretbox_NONCEBYTES
#define SIGNET_PASSKEY_MAC_LEN crypto_secretbox_MACBYTES
#define SIGNET_PASSKEY_MAGIC_0 'S'
#define SIGNET_PASSKEY_MAGIC_1 'P'
#define SIGNET_PASSKEY_MAGIC_2 'K'
#define SIGNET_PASSKEY_MAGIC_3 'P'
#define SIGNET_PASSKEY_EXPORT_MAGIC_0 'S'
#define SIGNET_PASSKEY_EXPORT_MAGIC_1 'P'
#define SIGNET_PASSKEY_EXPORT_MAGIC_2 'K'
#define SIGNET_PASSKEY_EXPORT_MAGIC_3 'E'

#define PASSKEY_SELECT_COLUMNS \
  "credential_id, agent_id, rp_id, rp_id_hash, user_handle, sign_count, " \
  "aaguid, discoverable, payload, nonce, created_at, updated_at"

typedef struct {
  const uint8_t *p;
  size_t len;
  size_t off;
} Reader;

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  int err;
} Writer;

static char *dup_bytes_as_string(const char *s, size_t n) {
  char *out = (char *)malloc(n + 1);
  if (!out) return NULL;
  if (n > 0) memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

static char *dup_string(const char *s) {
  return s ? dup_bytes_as_string(s, strlen(s)) : NULL;
}

static uint8_t *dup_blob(const uint8_t *p, size_t n) {
  if (!p || n == 0) return NULL;
  uint8_t *out = (uint8_t *)malloc(n);
  if (!out) return NULL;
  memcpy(out, p, n);
  return out;
}

static uint8_t *dup_secret_blob(const uint8_t *p, size_t n) {
  if (!p || n == 0) return NULL;
  uint8_t *out = (uint8_t *)sodium_malloc(n);
  if (!out) return NULL;
  memcpy(out, p, n);
  return out;
}

static int size_to_int(size_t n, int *out) {
  if (!out || n > (size_t)INT_MAX) return -1;
  *out = (int)n;
  return 0;
}

static bool valid_psk(const uint8_t *psk, size_t psk_len) {
  return psk && psk_len == SIGNET_PASSKEY_PSK_LEN;
}

static bool valid_create(const SignetPasskeyCreate *c,
                         const uint8_t *psk,
                         size_t psk_len) {
  return c &&
         c->credential_id && c->credential_id_len > 0 &&
         c->agent_id && c->agent_id[0] &&
         c->rp_id && c->rp_id[0] &&
         c->user_handle && c->user_handle_len > 0 &&
         c->aaguid &&
         c->backend_id && c->backend_id[0] &&
         c->cose_alg == SIGNET_PASSKEY_COSE_ALG_ES256 &&
         c->key_blob && c->key_blob_len > 0 &&
         c->cose_public_key && c->cose_public_key_len > 0 &&
         valid_psk(psk, psk_len);
}

static void write_init(Writer *w) {
  memset(w, 0, sizeof(*w));
}

static int write_reserve(Writer *w, size_t extra) {
  if (w->err) return -1;
  if (extra > SIZE_MAX - w->len) {
    w->err = 1;
    return -1;
  }
  size_t need = w->len + extra;
  if (need <= w->cap) return 0;
  size_t cap = w->cap ? w->cap * 2 : 128;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) {
      cap = need;
      break;
    }
    cap *= 2;
  }
  uint8_t *data = (uint8_t *)realloc(w->data, cap);
  if (!data) {
    w->err = 1;
    return -1;
  }
  w->data = data;
  w->cap = cap;
  return 0;
}

static void write_bytes(Writer *w, const uint8_t *p, size_t n) {
  if (n == 0) return;
  if (!p || write_reserve(w, n) != 0) {
    w->err = 1;
    return;
  }
  memcpy(w->data + w->len, p, n);
  w->len += n;
}

static void write_u8(Writer *w, uint8_t v) {
  write_bytes(w, &v, 1);
}

static void write_u16(Writer *w, uint16_t v) {
  write_u8(w, (uint8_t)(v >> 8));
  write_u8(w, (uint8_t)v);
}

static void write_u32(Writer *w, uint32_t v) {
  write_u8(w, (uint8_t)(v >> 24));
  write_u8(w, (uint8_t)(v >> 16));
  write_u8(w, (uint8_t)(v >> 8));
  write_u8(w, (uint8_t)v);
}

static void write_u64(Writer *w, uint64_t v) {
  write_u32(w, (uint32_t)(v >> 32));
  write_u32(w, (uint32_t)v);
}

static int write_len32(Writer *w, size_t n) {
  if (n > UINT32_MAX) {
    w->err = 1;
    return -1;
  }
  write_u32(w, (uint32_t)n);
  return 0;
}

static int pack_payload(const SignetPasskeyCreate *c,
                        uint8_t **out,
                        size_t *out_len) {
  if (!c || !out || !out_len) return -1;
  *out = NULL;
  *out_len = 0;

  size_t backend_len = strlen(c->backend_id);
  size_t rp_len = strlen(c->rp_id);
  size_t user_name_len = c->user_name ? strlen(c->user_name) : 0;
  size_t display_len = c->user_display_name ? strlen(c->user_display_name) : 0;

  if (backend_len > UINT16_MAX) return -1;

  Writer w;
  write_init(&w);
  write_u8(&w, SIGNET_PASSKEY_MAGIC_0);
  write_u8(&w, SIGNET_PASSKEY_MAGIC_1);
  write_u8(&w, SIGNET_PASSKEY_MAGIC_2);
  write_u8(&w, SIGNET_PASSKEY_MAGIC_3);
  write_u16(&w, SIGNET_PASSKEY_PAYLOAD_VERSION);
  write_u32(&w, (uint32_t)c->cose_alg);
  write_u16(&w, (uint16_t)backend_len);
  if (write_len32(&w, c->key_blob_len) != 0 ||
      write_len32(&w, c->cose_public_key_len) != 0 ||
      write_len32(&w, rp_len) != 0 ||
      write_len32(&w, c->user_handle_len) != 0 ||
      write_len32(&w, user_name_len) != 0 ||
      write_len32(&w, display_len) != 0) {
    free(w.data);
    return -1;
  }

  write_bytes(&w, (const uint8_t *)c->backend_id, backend_len);
  write_bytes(&w, c->key_blob, c->key_blob_len);
  write_bytes(&w, c->cose_public_key, c->cose_public_key_len);
  write_bytes(&w, (const uint8_t *)c->rp_id, rp_len);
  write_bytes(&w, c->user_handle, c->user_handle_len);
  if (user_name_len > 0) write_bytes(&w, (const uint8_t *)c->user_name, user_name_len);
  if (display_len > 0) write_bytes(&w, (const uint8_t *)c->user_display_name, display_len);

  if (w.err) {
    if (w.data) sodium_memzero(w.data, w.len);
    free(w.data);
    return -1;
  }

  *out = w.data;
  *out_len = w.len;
  return 0;
}

static int read_bytes(Reader *r, const uint8_t **out, size_t n) {
  if (!r || !out || n > r->len - r->off) return -1;
  *out = r->p + r->off;
  r->off += n;
  return 0;
}

static int read_u8(Reader *r, uint8_t *out) {
  const uint8_t *p = NULL;
  if (read_bytes(r, &p, 1) != 0) return -1;
  *out = p[0];
  return 0;
}

static int read_u16(Reader *r, uint16_t *out) {
  uint8_t a = 0;
  uint8_t b = 0;
  if (read_u8(r, &a) != 0 || read_u8(r, &b) != 0) return -1;
  *out = (uint16_t)(((uint16_t)a << 8) | b);
  return 0;
}

static int read_u32(Reader *r, uint32_t *out) {
  uint8_t a = 0;
  uint8_t b = 0;
  uint8_t c = 0;
  uint8_t d = 0;
  if (read_u8(r, &a) != 0 || read_u8(r, &b) != 0 ||
      read_u8(r, &c) != 0 || read_u8(r, &d) != 0) {
    return -1;
  }
  *out = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | d;
  return 0;
}

static int read_u64(Reader *r, uint64_t *out) {
  uint32_t hi = 0;
  uint32_t lo = 0;
  if (read_u32(r, &hi) != 0 || read_u32(r, &lo) != 0) return -1;
  *out = ((uint64_t)hi << 32) | lo;
  return 0;
}

static int pack_export_container(const SignetPasskeyCredential *rec,
                                 const uint8_t *payload,
                                 size_t payload_len,
                                 const uint8_t *nonce,
                                 size_t nonce_len,
                                 uint8_t **out,
                                 size_t *out_len) {
  if (!rec || !payload || payload_len == 0 ||
      !nonce || nonce_len != SIGNET_PASSKEY_NONCE_LEN || !out || !out_len) {
    return -1;
  }
  *out = NULL;
  *out_len = 0;

  size_t rp_len = rec->rp_id ? strlen(rec->rp_id) : 0;
  if (!rec->credential_id || rec->credential_id_len == 0 ||
      rp_len == 0 || !rec->user_handle || rec->user_handle_len == 0 ||
      rec->cose_alg != SIGNET_PASSKEY_COSE_ALG_ES256) {
    return -1;
  }

  Writer w;
  write_init(&w);
  write_u8(&w, SIGNET_PASSKEY_EXPORT_MAGIC_0);
  write_u8(&w, SIGNET_PASSKEY_EXPORT_MAGIC_1);
  write_u8(&w, SIGNET_PASSKEY_EXPORT_MAGIC_2);
  write_u8(&w, SIGNET_PASSKEY_EXPORT_MAGIC_3);
  write_u16(&w, SIGNET_PASSKEY_EXPORT_FORMAT_VERSION);
  write_u32(&w, (uint32_t)rec->cose_alg);
  write_u8(&w, rec->discoverable ? 1 : 0);
  write_u64(&w, (uint64_t)(rec->created_at > 0 ? rec->created_at : 0));
  write_bytes(&w, rec->aaguid, SIGNET_PASSKEY_AAGUID_LEN);
  write_bytes(&w, rec->rp_id_hash, SIGNET_PASSKEY_RP_ID_HASH_LEN);
  if (write_len32(&w, rec->credential_id_len) != 0 ||
      write_len32(&w, rp_len) != 0 ||
      write_len32(&w, rec->user_handle_len) != 0 ||
      write_len32(&w, nonce_len) != 0 ||
      write_len32(&w, payload_len) != 0) {
    free(w.data);
    return -1;
  }
  write_bytes(&w, rec->credential_id, rec->credential_id_len);
  write_bytes(&w, (const uint8_t *)rec->rp_id, rp_len);
  write_bytes(&w, rec->user_handle, rec->user_handle_len);
  write_bytes(&w, nonce, nonce_len);
  write_bytes(&w, payload, payload_len);

  if (w.err) {
    free(w.data);
    return -1;
  }
  *out = w.data;
  *out_len = w.len;
  return 0;
}

typedef struct {
  uint8_t *credential_id;
  size_t credential_id_len;
  char *rp_id;
  uint8_t rp_id_hash[SIGNET_PASSKEY_RP_ID_HASH_LEN];
  uint8_t *user_handle;
  size_t user_handle_len;
  uint8_t aaguid[SIGNET_PASSKEY_AAGUID_LEN];
  bool discoverable;
  int cose_alg;
  int64_t created_at;
  uint8_t *nonce;
  size_t nonce_len;
  uint8_t *payload;
  size_t payload_len;
} ParsedExport;

static void parsed_export_clear(ParsedExport *pe) {
  if (!pe) return;
  free(pe->credential_id);
  free(pe->rp_id);
  free(pe->user_handle);
  free(pe->nonce);
  free(pe->payload);
  memset(pe, 0, sizeof(*pe));
}

static int parse_export_container(const uint8_t *container,
                                  size_t container_len,
                                  ParsedExport *out) {
  if (!container || container_len == 0 || !out) return -1;
  memset(out, 0, sizeof(*out));

  Reader r = { container, container_len, 0 };
  uint8_t magic[4];
  for (size_t i = 0; i < sizeof(magic); i++) {
    if (read_u8(&r, &magic[i]) != 0) return -1;
  }
  if (magic[0] != SIGNET_PASSKEY_EXPORT_MAGIC_0 ||
      magic[1] != SIGNET_PASSKEY_EXPORT_MAGIC_1 ||
      magic[2] != SIGNET_PASSKEY_EXPORT_MAGIC_2 ||
      magic[3] != SIGNET_PASSKEY_EXPORT_MAGIC_3) {
    return -1;
  }

  uint16_t version = 0;
  uint32_t alg_u32 = 0;
  uint8_t discoverable = 0;
  uint64_t created_at = 0;
  uint32_t cred_len = 0, rp_len = 0, uh_len = 0, nonce_len = 0, payload_len = 0;
  const uint8_t *cred = NULL, *rp = NULL, *uh = NULL, *nonce = NULL, *payload = NULL;

  if (read_u16(&r, &version) != 0 ||
      read_u32(&r, &alg_u32) != 0 ||
      read_u8(&r, &discoverable) != 0 ||
      read_u64(&r, &created_at) != 0) {
    return -1;
  }
  const uint8_t *aaguid = NULL;
  const uint8_t *rp_hash = NULL;
  if (read_bytes(&r, &aaguid, SIGNET_PASSKEY_AAGUID_LEN) != 0 ||
      read_bytes(&r, &rp_hash, SIGNET_PASSKEY_RP_ID_HASH_LEN) != 0 ||
      read_u32(&r, &cred_len) != 0 ||
      read_u32(&r, &rp_len) != 0 ||
      read_u32(&r, &uh_len) != 0 ||
      read_u32(&r, &nonce_len) != 0 ||
      read_u32(&r, &payload_len) != 0 ||
      read_bytes(&r, &cred, cred_len) != 0 ||
      read_bytes(&r, &rp, rp_len) != 0 ||
      read_bytes(&r, &uh, uh_len) != 0 ||
      read_bytes(&r, &nonce, nonce_len) != 0 ||
      read_bytes(&r, &payload, payload_len) != 0 ||
      r.off != r.len) {
    return -1;
  }

  if (version != SIGNET_PASSKEY_EXPORT_FORMAT_VERSION ||
      (int32_t)alg_u32 != SIGNET_PASSKEY_COSE_ALG_ES256 ||
      cred_len == 0 || rp_len == 0 || uh_len == 0 ||
      nonce_len != SIGNET_PASSKEY_NONCE_LEN ||
      payload_len < SIGNET_PASSKEY_MAC_LEN) {
    return -1;
  }

  uint8_t computed_hash[SIGNET_PASSKEY_RP_ID_HASH_LEN];
  SHA256(rp, rp_len, computed_hash);
  if (memcmp(computed_hash, rp_hash, SIGNET_PASSKEY_RP_ID_HASH_LEN) != 0) {
    sodium_memzero(computed_hash, sizeof(computed_hash));
    return -1;
  }
  sodium_memzero(computed_hash, sizeof(computed_hash));

  out->credential_id = dup_blob(cred, cred_len);
  out->credential_id_len = cred_len;
  out->rp_id = dup_bytes_as_string((const char *)rp, rp_len);
  out->user_handle = dup_blob(uh, uh_len);
  out->user_handle_len = uh_len;
  memcpy(out->aaguid, aaguid, SIGNET_PASSKEY_AAGUID_LEN);
  memcpy(out->rp_id_hash, rp_hash, SIGNET_PASSKEY_RP_ID_HASH_LEN);
  out->discoverable = discoverable ? true : false;
  out->cose_alg = (int)(int32_t)alg_u32;
  out->created_at = (int64_t)created_at;
  out->nonce = dup_blob(nonce, nonce_len);
  out->nonce_len = nonce_len;
  out->payload = dup_blob(payload, payload_len);
  out->payload_len = payload_len;

  if (!out->credential_id || !out->rp_id || !out->user_handle ||
      !out->nonce || !out->payload) {
    parsed_export_clear(out);
    return -1;
  }
  return 0;
}

static int parse_payload(const uint8_t *payload,
                         size_t payload_len,
                         SignetPasskeyCredential *out) {
  if (!payload || payload_len == 0 || !out) return -1;

  Reader r = { payload, payload_len, 0 };
  uint8_t magic[4];
  for (size_t i = 0; i < sizeof(magic); i++) {
    if (read_u8(&r, &magic[i]) != 0) return -1;
  }
  if (magic[0] != SIGNET_PASSKEY_MAGIC_0 || magic[1] != SIGNET_PASSKEY_MAGIC_1 ||
      magic[2] != SIGNET_PASSKEY_MAGIC_2 || magic[3] != SIGNET_PASSKEY_MAGIC_3) {
    return -1;
  }

  uint16_t version = 0;
  uint32_t alg_u32 = 0;
  uint16_t backend_len = 0;
  uint32_t key_len = 0;
  uint32_t cose_len = 0;
  uint32_t rp_len = 0;
  uint32_t user_handle_len = 0;
  uint32_t user_name_len = 0;
  uint32_t display_len = 0;

  if (read_u16(&r, &version) != 0 ||
      read_u32(&r, &alg_u32) != 0 ||
      read_u16(&r, &backend_len) != 0 ||
      read_u32(&r, &key_len) != 0 ||
      read_u32(&r, &cose_len) != 0 ||
      read_u32(&r, &rp_len) != 0 ||
      read_u32(&r, &user_handle_len) != 0 ||
      read_u32(&r, &user_name_len) != 0 ||
      read_u32(&r, &display_len) != 0) {
    return -1;
  }

  if (version != SIGNET_PASSKEY_PAYLOAD_VERSION ||
      (int32_t)alg_u32 != SIGNET_PASSKEY_COSE_ALG_ES256 ||
      backend_len == 0 || key_len == 0 || cose_len == 0 ||
      rp_len == 0 || user_handle_len == 0) {
    return -1;
  }

  const uint8_t *backend = NULL;
  const uint8_t *key = NULL;
  const uint8_t *cose = NULL;
  const uint8_t *rp = NULL;
  const uint8_t *user_handle = NULL;
  const uint8_t *user_name = NULL;
  const uint8_t *display = NULL;

  if (read_bytes(&r, &backend, backend_len) != 0 ||
      read_bytes(&r, &key, key_len) != 0 ||
      read_bytes(&r, &cose, cose_len) != 0 ||
      read_bytes(&r, &rp, rp_len) != 0 ||
      read_bytes(&r, &user_handle, user_handle_len) != 0 ||
      read_bytes(&r, &user_name, user_name_len) != 0 ||
      read_bytes(&r, &display, display_len) != 0 ||
      r.off != r.len) {
    return -1;
  }

  out->payload_version = version;
  out->cose_alg = (int)(int32_t)alg_u32;
  out->backend_id = dup_bytes_as_string((const char *)backend, backend_len);
  out->key_blob = dup_secret_blob(key, key_len);
  out->key_blob_len = key_len;
  out->cose_public_key = dup_blob(cose, cose_len);
  out->cose_public_key_len = cose_len;
  out->rp_id = dup_bytes_as_string((const char *)rp, rp_len);
  out->user_handle = dup_blob(user_handle, user_handle_len);
  out->user_handle_len = user_handle_len;
  if (user_name_len > 0) out->user_name = dup_bytes_as_string((const char *)user_name, user_name_len);
  if (display_len > 0) out->user_display_name = dup_bytes_as_string((const char *)display, display_len);

  if (!out->backend_id || !out->key_blob || !out->cose_public_key ||
      !out->rp_id || !out->user_handle ||
      (user_name_len > 0 && !out->user_name) ||
      (display_len > 0 && !out->user_display_name)) {
    return -1;
  }

  return 0;
}

static int decrypt_payload(const uint8_t *ct,
                           size_t ct_len,
                           const uint8_t *nonce,
                           size_t nonce_len,
                           const uint8_t *psk,
                           size_t psk_len,
                           SignetPasskeyCredential *out) {
  if (!ct || ct_len < SIGNET_PASSKEY_MAC_LEN ||
      !nonce || nonce_len != SIGNET_PASSKEY_NONCE_LEN ||
      !valid_psk(psk, psk_len) || !out) {
    return -1;
  }

  size_t pt_len = ct_len - SIGNET_PASSKEY_MAC_LEN;
  uint8_t *pt = (uint8_t *)sodium_malloc(pt_len);
  if (!pt) return -1;

  int rc = crypto_secretbox_open_easy(pt, ct, ct_len, nonce, psk);
  if (rc == 0) rc = parse_payload(pt, pt_len, out);
  sodium_free(pt);
  return rc == 0 ? 0 : -1;
}

static int bind_blob_or_error(sqlite3_stmt *stmt,
                              int idx,
                              const uint8_t *p,
                              size_t n) {
  int len = 0;
  if (!p || size_to_int(n, &len) != 0) return -1;
  return sqlite3_bind_blob(stmt, idx, p, len, SQLITE_TRANSIENT) == SQLITE_OK ? 0 : -1;
}

static int load_record(sqlite3_stmt *stmt,
                       const uint8_t *psk,
                       size_t psk_len,
                       SignetPasskeyCredential *out) {
  if (!stmt || !out) return -1;
  memset(out, 0, sizeof(*out));

  const uint8_t *credential_id = (const uint8_t *)sqlite3_column_blob(stmt, 0);
  int credential_id_len = sqlite3_column_bytes(stmt, 0);
  const char *agent_id = (const char *)sqlite3_column_text(stmt, 1);
  const char *rp_id = (const char *)sqlite3_column_text(stmt, 2);
  const uint8_t *rp_hash = (const uint8_t *)sqlite3_column_blob(stmt, 3);
  int rp_hash_len = sqlite3_column_bytes(stmt, 3);
  const uint8_t *user_handle = (const uint8_t *)sqlite3_column_blob(stmt, 4);
  int user_handle_len = sqlite3_column_bytes(stmt, 4);
  int sign_count = sqlite3_column_int(stmt, 5);
  const uint8_t *aaguid = (const uint8_t *)sqlite3_column_blob(stmt, 6);
  int aaguid_len = sqlite3_column_bytes(stmt, 6);
  int discoverable = sqlite3_column_int(stmt, 7);
  const uint8_t *ct = (const uint8_t *)sqlite3_column_blob(stmt, 8);
  int ct_len = sqlite3_column_bytes(stmt, 8);
  const uint8_t *nonce = (const uint8_t *)sqlite3_column_blob(stmt, 9);
  int nonce_len = sqlite3_column_bytes(stmt, 9);

  if (!credential_id || credential_id_len <= 0 || !agent_id || !rp_id ||
      !rp_hash || rp_hash_len != SIGNET_PASSKEY_RP_ID_HASH_LEN ||
      !user_handle || user_handle_len <= 0 || sign_count != 0 ||
      !aaguid || aaguid_len != SIGNET_PASSKEY_AAGUID_LEN) {
    return -1;
  }

  if (decrypt_payload(ct, (size_t)ct_len, nonce, (size_t)nonce_len,
                      psk, psk_len, out) != 0) {
    signet_passkey_credential_clear(out);
    return -1;
  }

  /* Authenticated payload metadata must match the plaintext lookup fields. */
  if (strcmp(out->rp_id, rp_id) != 0 ||
      out->user_handle_len != (size_t)user_handle_len ||
      memcmp(out->user_handle, user_handle, (size_t)user_handle_len) != 0) {
    signet_passkey_credential_clear(out);
    return -1;
  }

  out->credential_id = dup_blob(credential_id, (size_t)credential_id_len);
  out->credential_id_len = (size_t)credential_id_len;
  out->agent_id = dup_string(agent_id);
  memcpy(out->rp_id_hash, rp_hash, SIGNET_PASSKEY_RP_ID_HASH_LEN);
  out->sign_count = 0;
  memcpy(out->aaguid, aaguid, SIGNET_PASSKEY_AAGUID_LEN);
  out->discoverable = discoverable ? true : false;
  out->created_at = sqlite3_column_int64(stmt, 10);
  out->updated_at = sqlite3_column_int64(stmt, 11);

  if (!out->credential_id || !out->agent_id) {
    signet_passkey_credential_clear(out);
    return -1;
  }

  return 0;
}

int signet_store_passkey_create(SignetStore *store,
                                const SignetPasskeyCreate *credential,
                                const uint8_t *fleet_psk,
                                size_t fleet_psk_len) {
  sqlite3 *db = signet_store_get_db(store);
  if (!db || !valid_create(credential, fleet_psk, fleet_psk_len)) return -1;

  uint8_t rp_hash[SIGNET_PASSKEY_RP_ID_HASH_LEN];
  SHA256((const unsigned char *)credential->rp_id, strlen(credential->rp_id), rp_hash);

  uint8_t *plaintext = NULL;
  size_t plaintext_len = 0;
  if (pack_payload(credential, &plaintext, &plaintext_len) != 0) return -1;

  uint8_t nonce[SIGNET_PASSKEY_NONCE_LEN];
  randombytes_buf(nonce, sizeof(nonce));

  size_t ct_len = plaintext_len + SIGNET_PASSKEY_MAC_LEN;
  uint8_t *ciphertext = (uint8_t *)malloc(ct_len);
  if (!ciphertext) {
    sodium_memzero(plaintext, plaintext_len);
    free(plaintext);
    return -1;
  }

  int rc = crypto_secretbox_easy(ciphertext, plaintext, plaintext_len, nonce, fleet_psk);
  sodium_memzero(plaintext, plaintext_len);
  free(plaintext);
  if (rc != 0) {
    free(ciphertext);
    return -1;
  }

  rc = sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    free(ciphertext);
    return -1;
  }

  const char *sql =
      "INSERT INTO passkey_credentials "
      "(credential_id, agent_id, rp_id, rp_id_hash, user_handle, sign_count, "
      " aaguid, discoverable, payload, nonce, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    free(ciphertext);
    return -1;
  }

  if (bind_blob_or_error(stmt, 1, credential->credential_id, credential->credential_id_len) != 0 ||
      sqlite3_bind_text(stmt, 2, credential->agent_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(stmt, 3, credential->rp_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
      bind_blob_or_error(stmt, 4, rp_hash, sizeof(rp_hash)) != 0 ||
      bind_blob_or_error(stmt, 5, credential->user_handle, credential->user_handle_len) != 0 ||
      bind_blob_or_error(stmt, 6, credential->aaguid, SIGNET_PASSKEY_AAGUID_LEN) != 0 ||
      sqlite3_bind_int(stmt, 7, credential->discoverable ? 1 : 0) != SQLITE_OK ||
      bind_blob_or_error(stmt, 8, ciphertext, ct_len) != 0 ||
      bind_blob_or_error(stmt, 9, nonce, sizeof(nonce)) != 0 ||
      sqlite3_bind_int64(stmt, 10, credential->created_at) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 11, credential->created_at) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    free(ciphertext);
    return -1;
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  free(ciphertext);

  if (rc != SQLITE_DONE) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return rc == SQLITE_CONSTRAINT ? 1 : -1;
  }

  rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
  }

  return 0;
}

int signet_store_passkey_find_by_credential_id(SignetStore *store,
                                               const uint8_t *credential_id,
                                               size_t credential_id_len,
                                               const uint8_t *fleet_psk,
                                               size_t fleet_psk_len,
                                               SignetPasskeyCredential *out) {
  sqlite3 *db = signet_store_get_db(store);
  if (out) memset(out, 0, sizeof(*out));
  if (!db || !credential_id || credential_id_len == 0 ||
      !valid_psk(fleet_psk, fleet_psk_len) || !out) {
    return -1;
  }

  const char *sql =
      "SELECT " PASSKEY_SELECT_COLUMNS " "
      "FROM passkey_credentials WHERE credential_id = ? LIMIT 1;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;
  if (bind_blob_or_error(stmt, 1, credential_id, credential_id_len) != 0) {
    sqlite3_finalize(stmt);
    return -1;
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return 1;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }

  rc = load_record(stmt, fleet_psk, fleet_psk_len, out);
  sqlite3_finalize(stmt);
  return rc == 0 ? 0 : -1;
}

int signet_store_passkey_find_by_agent_rp(SignetStore *store,
                                          const char *agent_id,
                                          const char *rp_id,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          SignetPasskeyCredential **out_records,
                                          size_t *out_count) {
  sqlite3 *db = signet_store_get_db(store);
  if (out_records) *out_records = NULL;
  if (out_count) *out_count = 0;
  if (!db || !agent_id || !agent_id[0] || !rp_id || !rp_id[0] ||
      !valid_psk(fleet_psk, fleet_psk_len) || !out_records || !out_count) {
    return -1;
  }

  const char *sql =
      "SELECT " PASSKEY_SELECT_COLUMNS " "
      "FROM passkey_credentials "
      "WHERE agent_id = ? AND rp_id = ? AND discoverable != 0 "
      "ORDER BY created_at, id;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;
  sqlite3_bind_text(stmt, 1, agent_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, rp_id, -1, SQLITE_TRANSIENT);

  SignetPasskeyCredential *records = NULL;
  size_t count = 0;

  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    SignetPasskeyCredential rec;
    if (load_record(stmt, fleet_psk, fleet_psk_len, &rec) != 0) {
      signet_passkey_credential_list_free(records, count);
      sqlite3_finalize(stmt);
      return -1;
    }
    SignetPasskeyCredential *next =
        (SignetPasskeyCredential *)realloc(records, (count + 1) * sizeof(*records));
    if (!next) {
      signet_passkey_credential_clear(&rec);
      signet_passkey_credential_list_free(records, count);
      sqlite3_finalize(stmt);
      return -1;
    }
    records = next;
    records[count++] = rec;
  }

  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    signet_passkey_credential_list_free(records, count);
    return -1;
  }

  *out_records = records;
  *out_count = count;
  return 0;
}

int signet_store_passkey_has_excluded(SignetStore *store,
                                      const char *agent_id,
                                      const char *rp_id,
                                      const uint8_t *const *credential_ids,
                                      const size_t *credential_id_lens,
                                      size_t credential_id_count,
                                      bool *out_has_match) {
  sqlite3 *db = signet_store_get_db(store);
  if (out_has_match) *out_has_match = false;
  if (!db || !agent_id || !agent_id[0] || !rp_id || !rp_id[0] ||
      !out_has_match ||
      (credential_id_count > 0 && (!credential_ids || !credential_id_lens))) {
    return -1;
  }
  if (credential_id_count == 0) return 0;

  const char *sql =
      "SELECT 1 FROM passkey_credentials "
      "WHERE credential_id = ? AND agent_id = ? AND rp_id = ? LIMIT 1;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;

  for (size_t i = 0; i < credential_id_count; i++) {
    if (!credential_ids[i] || credential_id_lens[i] == 0) {
      sqlite3_finalize(stmt);
      return -1;
    }
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    if (bind_blob_or_error(stmt, 1, credential_ids[i], credential_id_lens[i]) != 0 ||
        sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, rp_id, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
      sqlite3_finalize(stmt);
      return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      *out_has_match = true;
      sqlite3_finalize(stmt);
      return 0;
    }
    if (rc != SQLITE_DONE) {
      sqlite3_finalize(stmt);
      return -1;
    }
  }

  sqlite3_finalize(stmt);
  return 0;
}

int signet_store_passkey_export_container(SignetStore *store,
                                          const char *agent_id,
                                          const uint8_t *credential_id,
                                          size_t credential_id_len,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          uint8_t **out_container,
                                          size_t *out_container_len) {
  sqlite3 *db = signet_store_get_db(store);
  if (out_container) *out_container = NULL;
  if (out_container_len) *out_container_len = 0;
  if (!db || !agent_id || !agent_id[0] || !credential_id || credential_id_len == 0 ||
      !valid_psk(fleet_psk, fleet_psk_len) || !out_container || !out_container_len) {
    return -1;
  }

  const char *sql =
      "SELECT " PASSKEY_SELECT_COLUMNS " "
      "FROM passkey_credentials WHERE credential_id = ? LIMIT 1;";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) return -1;
  if (bind_blob_or_error(stmt, 1, credential_id, credential_id_len) != 0) {
    sqlite3_finalize(stmt);
    return -1;
  }

  rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    sqlite3_finalize(stmt);
    return 1;
  }
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return -1;
  }

  SignetPasskeyCredential rec;
  rc = load_record(stmt, fleet_psk, fleet_psk_len, &rec);
  if (rc != 0) {
    sqlite3_finalize(stmt);
    return -1;
  }
  if (!rec.agent_id || strcmp(rec.agent_id, agent_id) != 0) {
    signet_passkey_credential_clear(&rec);
    sqlite3_finalize(stmt);
    return 1;
  }

  const uint8_t *payload = (const uint8_t *)sqlite3_column_blob(stmt, 8);
  int payload_len = sqlite3_column_bytes(stmt, 8);
  const uint8_t *nonce = (const uint8_t *)sqlite3_column_blob(stmt, 9);
  int nonce_len = sqlite3_column_bytes(stmt, 9);
  rc = pack_export_container(&rec, payload, (size_t)payload_len,
                             nonce, (size_t)nonce_len,
                             out_container, out_container_len);

  signet_passkey_credential_clear(&rec);
  sqlite3_finalize(stmt);
  return rc == 0 ? 0 : -1;
}

int signet_store_passkey_import_container(SignetStore *store,
                                          const char *agent_id,
                                          const uint8_t *container,
                                          size_t container_len,
                                          const uint8_t *fleet_psk,
                                          size_t fleet_psk_len,
                                          int64_t now,
                                          SignetPasskeyCredential *out) {
  sqlite3 *db = signet_store_get_db(store);
  if (out) memset(out, 0, sizeof(*out));
  if (!db || !agent_id || !agent_id[0] ||
      !container || container_len == 0 ||
      !valid_psk(fleet_psk, fleet_psk_len)) {
    return -1;
  }

  ParsedExport pe;
  if (parse_export_container(container, container_len, &pe) != 0) return -1;

  SignetPasskeyCredential payload_meta;
  memset(&payload_meta, 0, sizeof(payload_meta));
  int rc = decrypt_payload(pe.payload, pe.payload_len,
                           pe.nonce, pe.nonce_len,
                           fleet_psk, fleet_psk_len,
                           &payload_meta);
  if (rc != 0) {
    parsed_export_clear(&pe);
    return -1;
  }

  if (!payload_meta.rp_id || strcmp(payload_meta.rp_id, pe.rp_id) != 0 ||
      payload_meta.user_handle_len != pe.user_handle_len ||
      memcmp(payload_meta.user_handle, pe.user_handle, pe.user_handle_len) != 0 ||
      payload_meta.cose_alg != SIGNET_PASSKEY_COSE_ALG_ES256) {
    signet_passkey_credential_clear(&payload_meta);
    parsed_export_clear(&pe);
    return -1;
  }
  signet_passkey_credential_clear(&payload_meta);

  int64_t ts = now > 0 ? now : (pe.created_at > 0 ? pe.created_at : 0);
  rc = sqlite3_exec(db, "BEGIN IMMEDIATE TRANSACTION;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    parsed_export_clear(&pe);
    return -1;
  }

  const char *sql =
      "INSERT INTO passkey_credentials "
      "(credential_id, agent_id, rp_id, rp_id_hash, user_handle, sign_count, "
      " aaguid, discoverable, payload, nonce, created_at, updated_at) "
      "VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    parsed_export_clear(&pe);
    return -1;
  }

  if (bind_blob_or_error(stmt, 1, pe.credential_id, pe.credential_id_len) != 0 ||
      sqlite3_bind_text(stmt, 2, agent_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
      sqlite3_bind_text(stmt, 3, pe.rp_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
      bind_blob_or_error(stmt, 4, pe.rp_id_hash, SIGNET_PASSKEY_RP_ID_HASH_LEN) != 0 ||
      bind_blob_or_error(stmt, 5, pe.user_handle, pe.user_handle_len) != 0 ||
      bind_blob_or_error(stmt, 6, pe.aaguid, SIGNET_PASSKEY_AAGUID_LEN) != 0 ||
      sqlite3_bind_int(stmt, 7, pe.discoverable ? 1 : 0) != SQLITE_OK ||
      bind_blob_or_error(stmt, 8, pe.payload, pe.payload_len) != 0 ||
      bind_blob_or_error(stmt, 9, pe.nonce, pe.nonce_len) != 0 ||
      sqlite3_bind_int64(stmt, 10, ts) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 11, ts) != SQLITE_OK) {
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    parsed_export_clear(&pe);
    return -1;
  }

  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    parsed_export_clear(&pe);
    return rc == SQLITE_CONSTRAINT ? 1 : -1;
  }

  rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    parsed_export_clear(&pe);
    return -1;
  }

  if (out) {
    rc = signet_store_passkey_find_by_credential_id(store,
        pe.credential_id, pe.credential_id_len,
        fleet_psk, fleet_psk_len, out);
  } else {
    rc = 0;
  }
  parsed_export_clear(&pe);
  return rc == 0 ? 0 : -1;
}

void signet_passkey_export_container_free(uint8_t *container) {
  free(container);
}

void signet_passkey_credential_clear(SignetPasskeyCredential *credential) {
  if (!credential) return;
  free(credential->credential_id);
  free(credential->agent_id);
  free(credential->rp_id);
  free(credential->user_handle);
  free(credential->backend_id);
  if (credential->key_blob) sodium_free(credential->key_blob);
  free(credential->cose_public_key);
  free(credential->user_name);
  free(credential->user_display_name);
  memset(credential, 0, sizeof(*credential));
}

void signet_passkey_credential_list_free(SignetPasskeyCredential *records,
                                         size_t count) {
  if (!records) return;
  for (size_t i = 0; i < count; i++) {
    signet_passkey_credential_clear(&records[i]);
  }
  free(records);
}
