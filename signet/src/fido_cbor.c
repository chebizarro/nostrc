/* SPDX-License-Identifier: MIT */
/* signet FIDO CBOR/COSE helpers backed by libcbor. */

#include "signet/fido_cbor.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <cbor.h>
#include <openssl/sha.h>

#define SIGNET_FIDO_CBOR_MAX_CTAP_MAP_PAIRS 32u
#define SIGNET_FIDO_CBOR_MAX_CTAP_ARRAY 64u

/* ---- tiny growable byte buffer for WebAuthn authenticatorData ----------- */
typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  int err;
} buf_t;

static void buf_init(buf_t *b) { memset(b, 0, sizeof(*b)); }

static int buf_reserve(buf_t *b, size_t extra) {
  if (!b || b->err) return -1;
  if (extra > SIZE_MAX - b->len) { b->err = 1; return -1; }
  size_t need = b->len + extra;
  if (need <= b->cap) return 0;
  size_t ncap = b->cap ? b->cap * 2 : 64;
  while (ncap < need) {
    if (ncap > SIZE_MAX / 2) { ncap = need; break; }
    ncap *= 2;
  }
  uint8_t *nd = (uint8_t *)realloc(b->data, ncap);
  if (!nd) { b->err = 1; return -1; }
  b->data = nd;
  b->cap = ncap;
  return 0;
}

static void buf_put(buf_t *b, const uint8_t *p, size_t n) {
  if (!p && n) { if (b) b->err = 1; return; }
  if (buf_reserve(b, n) != 0) return;
  if (n) memcpy(b->data + b->len, p, n);
  b->len += n;
}

static void buf_u8(buf_t *b, uint8_t v) { buf_put(b, &v, 1); }

static int buf_finish(buf_t *b, uint8_t **out, size_t *out_len) {
  if (!out || !out_len || !b || b->err) {
    if (b) free(b->data);
    return -1;
  }
  *out = b->data;
  *out_len = b->len;
  b->data = NULL;
  b->len = b->cap = 0;
  return 0;
}

/* ---- libcbor construction helpers -------------------------------------- */
static cbor_item_t *cbor_uint_item(uint64_t v) {
  if (v <= UINT8_MAX) return cbor_build_uint8((uint8_t)v);
  if (v <= UINT16_MAX) return cbor_build_uint16((uint16_t)v);
  if (v <= UINT32_MAX) return cbor_build_uint32((uint32_t)v);
  return cbor_build_uint64(v);
}

static cbor_item_t *cbor_int_item(int64_t v) {
  if (v >= 0) return cbor_uint_item((uint64_t)v);
  uint64_t arg = (uint64_t)(-(v + 1));
  if (arg <= UINT8_MAX) return cbor_build_negint8((uint8_t)arg);
  if (arg <= UINT16_MAX) return cbor_build_negint16((uint16_t)arg);
  if (arg <= UINT32_MAX) return cbor_build_negint32((uint32_t)arg);
  return cbor_build_negint64(arg);
}

static bool cbor_map_add_owned(cbor_item_t *map, cbor_item_t *key, cbor_item_t *value) {
  if (!map || !key || !value) {
    if (key) cbor_decref(&key);
    if (value) cbor_decref(&value);
    return false;
  }
  bool ok = cbor_map_add(map, (struct cbor_pair){ .key = key, .value = value });
  cbor_decref(&key);
  cbor_decref(&value);
  return ok;
}

static int serialize_item(cbor_item_t *item, uint8_t **out, size_t *out_len) {
  if (!item || !out || !out_len) return -1;
  *out = NULL;
  *out_len = 0;
  unsigned char *buf = NULL;
  size_t len = 0;
  size_t rc = cbor_serialize_alloc(item, &buf, &len);
  (void)rc;
  if (!buf || len == 0) {
    free(buf);
    return -1;
  }
  *out = buf;
  *out_len = len;
  return 0;
}

static cbor_item_t *load_single_item(const uint8_t *p, size_t n) {
  if (!p || n == 0) return NULL;
  struct cbor_load_result result;
  cbor_item_t *item = cbor_load((cbor_data)p, n, &result);
  if (!item) return NULL;
  if (result.error.code != CBOR_ERR_NONE || result.read != n) {
    cbor_decref(&item);
    return NULL;
  }
  return item;
}

/* ---- primitive decode helpers ----------------------------------------- */
static int item_get_int64(const cbor_item_t *item, int64_t *out) {
  if (!item || !out) return -1;
  if (cbor_isa_uint(item)) {
    uint64_t v = cbor_get_uint64(item);
    if (v > (uint64_t)INT64_MAX) return -1;
    *out = (int64_t)v;
    return 0;
  }
  if (cbor_isa_negint(item)) {
    uint64_t arg = cbor_get_uint64(item);
    if (arg > (uint64_t)INT64_MAX) return -1;
    *out = -1 - (int64_t)arg;
    return 0;
  }
  return -1;
}

static bool item_text_eq(const cbor_item_t *item, const char *s) {
  if (!item || !s || !cbor_isa_string(item) || !cbor_string_is_definite(item)) return false;
  size_t len = cbor_string_length(item);
  return len == strlen(s) && memcmp(cbor_string_handle(item), s, len) == 0;
}

static int item_dup_text(const cbor_item_t *item, char **out) {
  if (out) *out = NULL;
  if (!item || !out || !cbor_isa_string(item) || !cbor_string_is_definite(item)) return -1;
  size_t len = cbor_string_length(item);
  if (len > SIZE_MAX - 1) return -1;
  char *s = (char *)malloc(len + 1);
  if (!s) return -1;
  if (len) memcpy(s, cbor_string_handle(item), len);
  s[len] = '\0';
  *out = s;
  return 0;
}

static int item_dup_bstr(const cbor_item_t *item, uint8_t **out, size_t *out_len) {
  if (out) *out = NULL;
  if (out_len) *out_len = 0;
  if (!item || !out || !out_len || !cbor_isa_bytestring(item) ||
      !cbor_bytestring_is_definite(item)) return -1;
  size_t len = cbor_bytestring_length(item);
  uint8_t *p = NULL;
  if (len > 0) {
    p = (uint8_t *)malloc(len);
    if (!p) return -1;
    memcpy(p, cbor_bytestring_handle(item), len);
  }
  *out = p;
  *out_len = len;
  return 0;
}

static bool item_is_definite_map_with_limit(const cbor_item_t *item, size_t limit) {
  return item && cbor_isa_map(item) && cbor_map_is_definite(item) &&
         cbor_map_size(item) <= limit;
}

static bool item_is_definite_array_with_limit(const cbor_item_t *item, size_t limit) {
  return item && cbor_isa_array(item) && cbor_array_is_definite(item) &&
         cbor_array_size(item) <= limit;
}

/* ---- COSE_Key EC2 / P-256 / ES256 ------------------------------------- */
int signet_cose_ec2_p256(const uint8_t x[32], const uint8_t y[32],
                         uint8_t **out, size_t *out_len) {
  if (!x || !y || !out || !out_len) return -1;
  cbor_item_t *map = cbor_new_definite_map(5);
  if (!map) return -1;

  /* CTAP2 canonical order by encoded key bytes: 1, 3, -1, -2, -3. */
  bool ok =
    cbor_map_add_owned(map, cbor_uint_item(1), cbor_uint_item(2)) &&        /* kty: EC2 */
    cbor_map_add_owned(map, cbor_uint_item(3), cbor_int_item(-7)) &&       /* alg: ES256 */
    cbor_map_add_owned(map, cbor_int_item(-1), cbor_uint_item(1)) &&       /* crv: P-256 */
    cbor_map_add_owned(map, cbor_int_item(-2), cbor_build_bytestring(x, 32)) &&
    cbor_map_add_owned(map, cbor_int_item(-3), cbor_build_bytestring(y, 32));
  int rc = ok ? serialize_item(map, out, out_len) : -1;
  cbor_decref(&map);
  return rc;
}

int signet_cose_ec2_p256_parse(const uint8_t *cose_key, size_t cose_key_len,
                               uint8_t x[32], uint8_t y[32]) {
  if (!cose_key || cose_key_len == 0 || !x || !y) return -1;
  cbor_item_t *root = load_single_item(cose_key, cose_key_len);
  if (!item_is_definite_map_with_limit(root, 16)) {
    if (root) cbor_decref(&root);
    return -1;
  }

  bool have_kty = false, have_alg = false, have_crv = false;
  bool have_x = false, have_y = false;
  struct cbor_pair *pairs = cbor_map_handle(root);
  for (size_t i = 0; i < cbor_map_size(root); i++) {
    int64_t key = 0;
    if (item_get_int64(pairs[i].key, &key) != 0) { cbor_decref(&root); return -1; }
    if (key == 1 || key == 3 || key == -1) {
      int64_t val = 0;
      if (item_get_int64(pairs[i].value, &val) != 0) { cbor_decref(&root); return -1; }
      if (key == 1) { if (val != 2) { cbor_decref(&root); return -1; } have_kty = true; }
      else if (key == 3) { if (val != -7) { cbor_decref(&root); return -1; } have_alg = true; }
      else { if (val != 1) { cbor_decref(&root); return -1; } have_crv = true; }
    } else if (key == -2 || key == -3) {
      if (!pairs[i].value || !cbor_isa_bytestring(pairs[i].value) ||
          !cbor_bytestring_is_definite(pairs[i].value) ||
          cbor_bytestring_length(pairs[i].value) != 32) {
        cbor_decref(&root);
        return -1;
      }
      if (key == -2) {
        memcpy(x, cbor_bytestring_handle(pairs[i].value), 32);
        have_x = true;
      } else {
        memcpy(y, cbor_bytestring_handle(pairs[i].value), 32);
        have_y = true;
      }
    }
  }

  int rc = (have_kty && have_alg && have_crv && have_x && have_y) ? 0 : -1;
  cbor_decref(&root);
  return rc;
}

/* ---- authenticatorData ------------------------------------------------- */
int signet_fido_auth_data(const char *rp_id, uint8_t flags, uint32_t sign_count,
                          const uint8_t aaguid[16],
                          const uint8_t *cred_id, size_t cred_id_len,
                          const uint8_t *cose_key, size_t cose_key_len,
                          uint8_t **out, size_t *out_len) {
  if (!rp_id || !out || !out_len) return -1;

  buf_t b;
  buf_init(&b);

  uint8_t rp_hash[SHA256_DIGEST_LENGTH];
  SHA256((const unsigned char *)rp_id, strlen(rp_id), rp_hash);
  buf_put(&b, rp_hash, sizeof(rp_hash));
  buf_u8(&b, flags);

  uint8_t sc[4] = {
    (uint8_t)(sign_count >> 24), (uint8_t)(sign_count >> 16),
    (uint8_t)(sign_count >> 8),  (uint8_t)sign_count
  };
  buf_put(&b, sc, sizeof(sc));

  if (flags & SIGNET_FIDO_FLAG_AT) {
    if (!aaguid || !cred_id || !cose_key || cred_id_len > 0xFFFFu) {
      free(b.data);
      return -1;
    }
    buf_put(&b, aaguid, 16);
    uint8_t cl[2] = { (uint8_t)(cred_id_len >> 8), (uint8_t)cred_id_len };
    buf_put(&b, cl, sizeof(cl));
    buf_put(&b, cred_id, cred_id_len);
    buf_put(&b, cose_key, cose_key_len);
  }

  return buf_finish(&b, out, out_len);
}

/* ---- "none" attestation object ---------------------------------------- */
int signet_fido_attestation_none(const uint8_t *auth_data, size_t auth_data_len,
                                 uint8_t **out, size_t *out_len) {
  if (!auth_data || !out || !out_len) return -1;
  cbor_item_t *map = cbor_new_definite_map(3);
  cbor_item_t *att_stmt = cbor_new_definite_map(0);
  if (!map || !att_stmt) {
    if (map) cbor_decref(&map);
    if (att_stmt) cbor_decref(&att_stmt);
    return -1;
  }

  /* Canonical order for these text keys: "fmt", "attStmt", "authData". */
  bool ok =
    cbor_map_add_owned(map, cbor_build_string("fmt"), cbor_build_string("none")) &&
    cbor_map_add_owned(map, cbor_build_string("attStmt"), att_stmt) &&
    cbor_map_add_owned(map, cbor_build_string("authData"),
                       cbor_build_bytestring(auth_data, auth_data_len));
  int rc = ok ? serialize_item(map, out, out_len) : -1;
  cbor_decref(&map);
  return rc;
}

/* ---- CTAP2 request-map decode ----------------------------------------- */
void signet_fido_cbor_make_credential_clear(SignetFidoCborMakeCredential *d) {
  if (!d) return;
  free(d->client_data_hash);
  free(d->rp_id);
  free(d->user_handle);
  free(d->user_name);
  free(d->user_display_name);
  free(d->algs);
  for (size_t i = 0; i < d->exclude_count; i++) free(d->exclude_ids[i]);
  free(d->exclude_ids);
  free(d->exclude_lens);
  memset(d, 0, sizeof(*d));
}

void signet_fido_cbor_get_assertion_clear(SignetFidoCborGetAssertion *d) {
  if (!d) return;
  free(d->rp_id);
  free(d->client_data_hash);
  for (size_t i = 0; i < d->allow_count; i++) free(d->allow_ids[i]);
  free(d->allow_ids);
  free(d->allow_lens);
  memset(d, 0, sizeof(*d));
}

static int parse_rp_map(const cbor_item_t *item, char **rp_id) {
  if (!item_is_definite_map_with_limit(item, SIGNET_FIDO_CBOR_MAX_CTAP_MAP_PAIRS)) return -1;
  struct cbor_pair *pairs = cbor_map_handle(item);
  for (size_t i = 0; i < cbor_map_size(item); i++) {
    if (!cbor_isa_string(pairs[i].key) || !cbor_string_is_definite(pairs[i].key)) return -1;
    if (item_text_eq(pairs[i].key, "id")) {
      char *s = NULL;
      if (item_dup_text(pairs[i].value, &s) != 0) return -1;
      free(*rp_id);
      *rp_id = s;
    }
  }
  return 0;
}

static int parse_user_map(const cbor_item_t *item, SignetFidoCborMakeCredential *d) {
  if (!item_is_definite_map_with_limit(item, SIGNET_FIDO_CBOR_MAX_CTAP_MAP_PAIRS)) return -1;
  struct cbor_pair *pairs = cbor_map_handle(item);
  for (size_t i = 0; i < cbor_map_size(item); i++) {
    if (!cbor_isa_string(pairs[i].key) || !cbor_string_is_definite(pairs[i].key)) return -1;
    if (item_text_eq(pairs[i].key, "id")) {
      uint8_t *p = NULL;
      size_t len = 0;
      if (item_dup_bstr(pairs[i].value, &p, &len) != 0) return -1;
      free(d->user_handle);
      d->user_handle = p;
      d->user_handle_len = len;
    } else if (item_text_eq(pairs[i].key, "name")) {
      char *s = NULL;
      if (item_dup_text(pairs[i].value, &s) != 0) return -1;
      free(d->user_name);
      d->user_name = s;
    } else if (item_text_eq(pairs[i].key, "displayName")) {
      char *s = NULL;
      if (item_dup_text(pairs[i].value, &s) != 0) return -1;
      free(d->user_display_name);
      d->user_display_name = s;
    }
  }
  return 0;
}

static int parse_pubkey_params(const cbor_item_t *item, SignetFidoCborMakeCredential *d) {
  if (!item_is_definite_array_with_limit(item, SIGNET_FIDO_CBOR_MAX_CTAP_ARRAY)) return -1;
  size_t n = cbor_array_size(item);
  int *algs = (int *)calloc(n ? n : 1, sizeof(int));
  if (!algs) return -1;
  size_t count = 0;
  cbor_item_t **items = cbor_array_handle(item);
  for (size_t i = 0; i < n; i++) {
    cbor_item_t *entry = items[i];
    if (!item_is_definite_map_with_limit(entry, 16)) { free(algs); return -1; }
    bool have_alg = false;
    int64_t alg = 0;
    struct cbor_pair *pairs = cbor_map_handle(entry);
    for (size_t j = 0; j < cbor_map_size(entry); j++) {
      if (!cbor_isa_string(pairs[j].key) || !cbor_string_is_definite(pairs[j].key)) {
        free(algs);
        return -1;
      }
      if (item_text_eq(pairs[j].key, "alg")) {
        if (item_get_int64(pairs[j].value, &alg) != 0 ||
            alg < (int64_t)INT_MIN || alg > (int64_t)INT_MAX) {
          free(algs);
          return -1;
        }
        have_alg = true;
      }
    }
    if (have_alg) algs[count++] = (int)alg;
  }
  free(d->algs);
  d->algs = algs;
  d->alg_count = count;
  return 0;
}

static int parse_cred_descriptors(const cbor_item_t *item,
                                  uint8_t ***ids, size_t **lens, size_t *count) {
  if (!ids || !lens || !count ||
      !item_is_definite_array_with_limit(item, SIGNET_FIDO_CBOR_MAX_CTAP_ARRAY)) return -1;
  size_t n = cbor_array_size(item);
  uint8_t **out_ids = (uint8_t **)calloc(n ? n : 1, sizeof(uint8_t *));
  size_t *out_lens = (size_t *)calloc(n ? n : 1, sizeof(size_t));
  if (!out_ids || !out_lens) { free(out_ids); free(out_lens); return -1; }

  size_t out_count = 0;
  cbor_item_t **items = cbor_array_handle(item);
  for (size_t i = 0; i < n; i++) {
    cbor_item_t *entry = items[i];
    if (!item_is_definite_map_with_limit(entry, 16)) goto fail;
    uint8_t *id = NULL;
    size_t id_len = 0;
    struct cbor_pair *pairs = cbor_map_handle(entry);
    for (size_t j = 0; j < cbor_map_size(entry); j++) {
      if (!cbor_isa_string(pairs[j].key) || !cbor_string_is_definite(pairs[j].key)) {
        free(id);
        goto fail;
      }
      if (item_text_eq(pairs[j].key, "id")) {
        uint8_t *p = NULL;
        size_t len = 0;
        if (item_dup_bstr(pairs[j].value, &p, &len) != 0) {
          free(id);
          goto fail;
        }
        free(id);
        id = p;
        id_len = len;
      }
    }
    if (id && id_len > 0) {
      out_ids[out_count] = id;
      out_lens[out_count] = id_len;
      out_count++;
    } else {
      free(id);
    }
  }

  *ids = out_ids;
  *lens = out_lens;
  *count = out_count;
  return 0;

fail:
  for (size_t i = 0; i < out_count; i++) free(out_ids[i]);
  free(out_ids);
  free(out_lens);
  return -1;
}

static int parse_options(const cbor_item_t *item,
                         bool *rk, bool *uv_required, bool *bad_up) {
  if (!item_is_definite_map_with_limit(item, SIGNET_FIDO_CBOR_MAX_CTAP_MAP_PAIRS)) return -1;
  struct cbor_pair *pairs = cbor_map_handle(item);
  for (size_t i = 0; i < cbor_map_size(item); i++) {
    if (!cbor_isa_string(pairs[i].key) || !cbor_string_is_definite(pairs[i].key) ||
        !cbor_is_bool(pairs[i].value)) return -1;
    bool val = cbor_get_bool(pairs[i].value);
    if (item_text_eq(pairs[i].key, "rk") && rk) *rk = val;
    else if (item_text_eq(pairs[i].key, "uv") && uv_required) *uv_required = val;
    else if (item_text_eq(pairs[i].key, "up") && bad_up && !val) *bad_up = true;
  }
  return 0;
}

int signet_fido_cbor_decode_make_credential(const uint8_t *cbor, size_t cbor_len,
                                            SignetFidoCborMakeCredential *out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!cbor || cbor_len == 0) return -1;
  out->discoverable = false;

  cbor_item_t *root = load_single_item(cbor, cbor_len);
  if (!item_is_definite_map_with_limit(root, SIGNET_FIDO_CBOR_MAX_CTAP_MAP_PAIRS)) {
    if (root) cbor_decref(&root);
    return -1;
  }

  bool bad_up = false;
  struct cbor_pair *pairs = cbor_map_handle(root);
  for (size_t i = 0; i < cbor_map_size(root); i++) {
    int64_t key = 0;
    if (item_get_int64(pairs[i].key, &key) != 0) goto fail;
    switch (key) {
      case 1:
        free(out->client_data_hash);
        out->client_data_hash = NULL;
        out->client_data_hash_len = 0;
        if (item_dup_bstr(pairs[i].value, &out->client_data_hash,
                          &out->client_data_hash_len) != 0) goto fail;
        break;
      case 2:
        if (parse_rp_map(pairs[i].value, &out->rp_id) != 0) goto fail;
        break;
      case 3:
        if (parse_user_map(pairs[i].value, out) != 0) goto fail;
        break;
      case 4:
        if (parse_pubkey_params(pairs[i].value, out) != 0) goto fail;
        break;
      case 5:
        free(out->exclude_ids);
        free(out->exclude_lens);
        out->exclude_ids = NULL;
        out->exclude_lens = NULL;
        out->exclude_count = 0;
        if (parse_cred_descriptors(pairs[i].value, &out->exclude_ids,
                                   &out->exclude_lens, &out->exclude_count) != 0) goto fail;
        break;
      case 7:
        if (parse_options(pairs[i].value, &out->discoverable, &out->uv_required,
                          &bad_up) != 0) goto fail;
        break;
      case 8:
      case 9:
        out->unsupported_pin_uv = true;
        break;
      default:
        break;
    }
  }
  if (bad_up) out->unsupported_pin_uv = true;
  cbor_decref(&root);
  return 0;

fail:
  cbor_decref(&root);
  signet_fido_cbor_make_credential_clear(out);
  return -1;
}

int signet_fido_cbor_decode_get_assertion(const uint8_t *cbor, size_t cbor_len,
                                          SignetFidoCborGetAssertion *out) {
  if (!out) return -1;
  memset(out, 0, sizeof(*out));
  if (!cbor || cbor_len == 0) return -1;

  cbor_item_t *root = load_single_item(cbor, cbor_len);
  if (!item_is_definite_map_with_limit(root, SIGNET_FIDO_CBOR_MAX_CTAP_MAP_PAIRS)) {
    if (root) cbor_decref(&root);
    return -1;
  }

  bool bad_up = false;
  struct cbor_pair *pairs = cbor_map_handle(root);
  for (size_t i = 0; i < cbor_map_size(root); i++) {
    int64_t key = 0;
    if (item_get_int64(pairs[i].key, &key) != 0) goto fail;
    switch (key) {
      case 1: {
        char *s = NULL;
        if (item_dup_text(pairs[i].value, &s) != 0) goto fail;
        free(out->rp_id);
        out->rp_id = s;
        break;
      }
      case 2:
        free(out->client_data_hash);
        out->client_data_hash = NULL;
        out->client_data_hash_len = 0;
        if (item_dup_bstr(pairs[i].value, &out->client_data_hash,
                          &out->client_data_hash_len) != 0) goto fail;
        break;
      case 3:
        free(out->allow_ids);
        free(out->allow_lens);
        out->allow_ids = NULL;
        out->allow_lens = NULL;
        out->allow_count = 0;
        if (parse_cred_descriptors(pairs[i].value, &out->allow_ids,
                                   &out->allow_lens, &out->allow_count) != 0) goto fail;
        break;
      case 5: {
        bool rk_ignored = false;
        if (parse_options(pairs[i].value, &rk_ignored, &out->uv_required,
                          &bad_up) != 0) goto fail;
        break;
      }
      case 6:
      case 7:
        out->unsupported_pin_uv = true;
        break;
      default:
        break;
    }
  }
  if (bad_up) out->unsupported_pin_uv = true;
  cbor_decref(&root);
  return 0;

fail:
  cbor_decref(&root);
  signet_fido_cbor_get_assertion_clear(out);
  return -1;
}
