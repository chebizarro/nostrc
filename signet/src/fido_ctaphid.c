/* SPDX-License-Identifier: MIT */
/* Linux /dev/uhid virtual CTAP-HID device for Signet. */

#include "signet/fido_ctaphid.h"

#include "signet/fido_cbor.h"
#include "signet/store_passkeys.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#if defined(__linux__)
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CTAPHID_PING      0x81u
#define CTAPHID_MSG       0x83u
#define CTAPHID_LOCK      0x84u
#define CTAPHID_INIT      0x86u
#define CTAPHID_WINK      0x88u
#define CTAPHID_CBOR      0x90u
#define CTAPHID_CANCEL    0x91u
#define CTAPHID_KEEPALIVE 0xbbu
#define CTAPHID_ERROR     0xbfu

#define CTAPHID_ERR_INVALID_CMD     0x01u
#define CTAPHID_ERR_INVALID_PAR     0x02u
#define CTAPHID_ERR_INVALID_LEN     0x03u
#define CTAPHID_ERR_INVALID_SEQ     0x04u
#define CTAPHID_ERR_CHANNEL_BUSY    0x06u
#define CTAPHID_ERR_INVALID_CHANNEL 0x0bu
#define CTAPHID_ERR_OTHER           0x7fu

#define CTAPHID_CAP_CBOR 0x04u

#define CTAP2_MAKE_CREDENTIAL 0x01u
#define CTAP2_GET_ASSERTION   0x02u
#define CTAP2_GET_INFO        0x04u
#define CTAP2_CLIENT_PIN      0x06u
#define CTAP2_RESET           0x07u
#define CTAP2_GET_NEXT_ASSERTION 0x08u
#define CTAP2_SELECTION       0x0bu

#define CTAP2_OK                    0x00u
#define CTAP2_ERR_INVALID_COMMAND   0x01u
#define CTAP2_ERR_INVALID_PARAMETER 0x02u
#define CTAP2_ERR_INVALID_LENGTH    0x03u
#define CTAP2_ERR_INVALID_CBOR      0x12u
#define CTAP2_ERR_MISSING_PARAMETER 0x14u
#define CTAP2_ERR_LIMIT_EXCEEDED    0x15u
#define CTAP2_ERR_CREDENTIAL_EXCLUDED 0x19u
#define CTAP2_ERR_INVALID_CREDENTIAL 0x22u
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26u
#define CTAP2_ERR_OPERATION_DENIED  0x27u
#define CTAP2_ERR_UNSUPPORTED_OPTION 0x2bu
#define CTAP2_ERR_NO_CREDENTIALS    0x2eu
#define CTAP2_ERR_NOT_ALLOWED       0x30u
#define CTAP2_ERR_PIN_REQUIRED      0x36u
#define CTAP2_ERR_REQUEST_TOO_LARGE 0x39u
#define CTAP2_ERR_OTHER             0x7fu

#define CTAP2_MAX_CRED_LIST 64u
#define SIGNET_CTAPHID_MAX_CHANNELS 16u

/* ---- byte buffer / CBOR encoder --------------------------------------- */
typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
  int err;
} ByteBuf;

static void bb_init(ByteBuf *b) { memset(b, 0, sizeof(*b)); }
static void bb_free(ByteBuf *b) { if (b) { free(b->data); memset(b, 0, sizeof(*b)); } }

static int bb_reserve(ByteBuf *b, size_t extra) {
  if (!b || b->err) return -1;
  if (extra > SIZE_MAX - b->len) { b->err = 1; return -1; }
  size_t need = b->len + extra;
  if (need <= b->cap) return 0;
  size_t cap = b->cap ? b->cap * 2 : 128;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) { cap = need; break; }
    cap *= 2;
  }
  uint8_t *p = (uint8_t *)realloc(b->data, cap);
  if (!p) { b->err = 1; return -1; }
  b->data = p;
  b->cap = cap;
  return 0;
}

static void bb_put(ByteBuf *b, const void *p, size_t n) {
  if (!p && n) { if (b) b->err = 1; return; }
  if (bb_reserve(b, n) != 0) return;
  memcpy(b->data + b->len, p, n);
  b->len += n;
}
static void bb_u8(ByteBuf *b, uint8_t v) { bb_put(b, &v, 1); }
static void bb_be16(ByteBuf *b, uint16_t v) { bb_u8(b, (uint8_t)(v >> 8)); bb_u8(b, (uint8_t)v); }
static void bb_be32(ByteBuf *b, uint32_t v) { bb_u8(b, (uint8_t)(v >> 24)); bb_u8(b, (uint8_t)(v >> 16)); bb_u8(b, (uint8_t)(v >> 8)); bb_u8(b, (uint8_t)v); }

static int bb_finish(ByteBuf *b, uint8_t **out, size_t *out_len) {
  if (!out || !out_len || !b || b->err) { bb_free(b); return -1; }
  *out = b->data;
  *out_len = b->len;
  b->data = NULL;
  b->len = b->cap = 0;
  return 0;
}

static void cbor_head(ByteBuf *b, uint8_t major, uint64_t arg) {
  uint8_t m = (uint8_t)(major << 5);
  if (arg < 24) bb_u8(b, (uint8_t)(m | arg));
  else if (arg <= 0xffu) { bb_u8(b, (uint8_t)(m | 24)); bb_u8(b, (uint8_t)arg); }
  else if (arg <= 0xffffu) { bb_u8(b, (uint8_t)(m | 25)); bb_be16(b, (uint16_t)arg); }
  else if (arg <= 0xffffffffu) { bb_u8(b, (uint8_t)(m | 26)); bb_be32(b, (uint32_t)arg); }
  else {
    bb_u8(b, (uint8_t)(m | 27));
    for (int s = 56; s >= 0; s -= 8) bb_u8(b, (uint8_t)(arg >> s));
  }
}
static void cbor_uint(ByteBuf *b, uint64_t v) { cbor_head(b, 0, v); }
static void cbor_nint(ByteBuf *b, int64_t v) { cbor_head(b, 1, (uint64_t)(-1 - v)); }
static void cbor_int(ByteBuf *b, int64_t v) { if (v >= 0) cbor_uint(b, (uint64_t)v); else cbor_nint(b, v); }
static void cbor_bstr(ByteBuf *b, const uint8_t *p, size_t n) { cbor_head(b, 2, n); bb_put(b, p, n); }
static void cbor_tstr(ByteBuf *b, const char *s) { size_t n = s ? strlen(s) : 0; cbor_head(b, 3, n); if (n) bb_put(b, s, n); }
static void cbor_array(ByteBuf *b, size_t n) { cbor_head(b, 4, n); }
static void cbor_map(ByteBuf *b, size_t n) { cbor_head(b, 5, n); }
static void cbor_bool(ByteBuf *b, bool v) { bb_u8(b, v ? 0xf5u : 0xf4u); }

/* ---- small CBOR decoder for CTAP2 request maps ------------------------- */
typedef struct {
  const uint8_t *p;
  size_t n;
  size_t off;
} CborIn;

static int cbor_read_u8(CborIn *in, uint8_t *v) {
  if (!in || in->off >= in->n) return -1;
  *v = in->p[in->off++];
  return 0;
}

static int cbor_read_head(CborIn *in, uint8_t *major, uint64_t *arg) {
  uint8_t ib = 0;
  if (cbor_read_u8(in, &ib) != 0) return -1;
  *major = (uint8_t)(ib >> 5);
  uint8_t ai = (uint8_t)(ib & 0x1f);
  if (ai < 24) { *arg = ai; return 0; }
  if (ai == 24) { uint8_t b; if (cbor_read_u8(in, &b) != 0) return -1; *arg = b; return 0; }
  if (ai == 25) {
    uint8_t a, b;
    if (cbor_read_u8(in, &a) || cbor_read_u8(in, &b)) return -1;
    *arg = ((uint64_t)a << 8) | b;
    return 0;
  }
  if (ai == 26) {
    uint8_t b[4];
    for (size_t i = 0; i < 4; i++) if (cbor_read_u8(in, &b[i])) return -1;
    *arg = ((uint64_t)b[0] << 24) | ((uint64_t)b[1] << 16) | ((uint64_t)b[2] << 8) | b[3];
    return 0;
  }
  if (ai == 27) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) { uint8_t b; if (cbor_read_u8(in, &b)) return -1; v = (v << 8) | b; }
    *arg = v;
    return 0;
  }
  return -1; /* indefinite lengths are intentionally unsupported */
}

static int cbor_skip(CborIn *in, int depth) {
  if (depth > 32) return -1;
  uint8_t major = 0;
  uint64_t arg = 0;
  if (cbor_read_head(in, &major, &arg) != 0) return -1;
  switch (major) {
    case 0: case 1: return 0;
    case 2: case 3:
      if (arg > in->n - in->off) return -1;
      in->off += (size_t)arg;
      return 0;
    case 4:
      if (arg > 1024) return -1;
      for (uint64_t i = 0; i < arg; i++) if (cbor_skip(in, depth + 1) != 0) return -1;
      return 0;
    case 5:
      if (arg > 1024) return -1;
      for (uint64_t i = 0; i < arg; i++) if (cbor_skip(in, depth + 1) || cbor_skip(in, depth + 1)) return -1;
      return 0;
    case 6:
      return cbor_skip(in, depth + 1);
    case 7:
      return 0;
    default:
      return -1;
  }
}

static int cbor_get_int(CborIn *in, int64_t *out) {
  uint8_t major = 0;
  uint64_t arg = 0;
  if (cbor_read_head(in, &major, &arg) != 0) return -1;
  if (major == 0) { if (arg > INT64_MAX) return -1; *out = (int64_t)arg; return 0; }
  if (major == 1) { if (arg > (uint64_t)INT64_MAX) return -1; *out = -1 - (int64_t)arg; return 0; }
  return -1;
}

static int cbor_get_bool(CborIn *in, bool *out) {
  if (!in || in->off >= in->n) return -1;
  uint8_t b = in->p[in->off++];
  if (b == 0xf4u) { *out = false; return 0; }
  if (b == 0xf5u) { *out = true; return 0; }
  return -1;
}

static int cbor_get_bstr(CborIn *in, uint8_t **out, size_t *out_len) {
  uint8_t major = 0;
  uint64_t arg = 0;
  if (out) *out = NULL;
  if (out_len) *out_len = 0;
  if (cbor_read_head(in, &major, &arg) != 0 || major != 2 || arg > in->n - in->off || arg > SIZE_MAX) return -1;
  uint8_t *p = NULL;
  if (arg > 0) {
    p = (uint8_t *)malloc((size_t)arg);
    if (!p) return -1;
    memcpy(p, in->p + in->off, (size_t)arg);
  }
  in->off += (size_t)arg;
  if (out) *out = p; else free(p);
  if (out_len) *out_len = (size_t)arg;
  return 0;
}

static int cbor_get_tstr(CborIn *in, char **out) {
  uint8_t major = 0;
  uint64_t arg = 0;
  if (out) *out = NULL;
  if (cbor_read_head(in, &major, &arg) != 0 || major != 3 || arg > in->n - in->off || arg > SIZE_MAX - 1) return -1;
  char *s = (char *)malloc((size_t)arg + 1);
  if (!s) return -1;
  memcpy(s, in->p + in->off, (size_t)arg);
  s[arg] = '\0';
  in->off += (size_t)arg;
  if (out) *out = s; else free(s);
  return 0;
}

static int cbor_get_text_key(CborIn *in, char *buf, size_t buflen) {
  char *tmp = NULL;
  if (cbor_get_tstr(in, &tmp) != 0) return -1;
  if (!buf || buflen == 0) { free(tmp); return -1; }
  g_strlcpy(buf, tmp, buflen);
  free(tmp);
  return 0;
}

typedef struct {
  uint8_t *client_data_hash;
  size_t client_data_hash_len;
  char *rp_id;
  uint8_t *user_handle;
  size_t user_handle_len;
  char *user_name;
  char *user_display_name;
  bool discoverable;
  bool uv_required;
  bool unsupported_pin_uv;
  int *algs;
  size_t alg_count;
  uint8_t **exclude_ids;
  size_t *exclude_lens;
  size_t exclude_count;
} DecodedMakeCredential;

typedef struct {
  char *rp_id;
  uint8_t *client_data_hash;
  size_t client_data_hash_len;
  bool uv_required;
  bool unsupported_pin_uv;
  uint8_t **allow_ids;
  size_t *allow_lens;
  size_t allow_count;
} DecodedGetAssertion;

static void decoded_mc_clear(DecodedMakeCredential *d) {
  if (!d) return;
  free(d->client_data_hash); free(d->rp_id); free(d->user_handle);
  free(d->user_name); free(d->user_display_name); free(d->algs);
  for (size_t i = 0; i < d->exclude_count; i++) free(d->exclude_ids[i]);
  free(d->exclude_ids); free(d->exclude_lens);
  memset(d, 0, sizeof(*d));
}
static void decoded_ga_clear(DecodedGetAssertion *d) {
  if (!d) return;
  free(d->rp_id); free(d->client_data_hash);
  for (size_t i = 0; i < d->allow_count; i++) free(d->allow_ids[i]);
  free(d->allow_ids); free(d->allow_lens);
  memset(d, 0, sizeof(*d));
}

static int parse_rp_map(CborIn *in, char **rp_id) {
  uint8_t major = 0; uint64_t n = 0;
  if (cbor_read_head(in, &major, &n) || major != 5 || n > 32) return -1;
  for (uint64_t i = 0; i < n; i++) {
    char key[32];
    if (cbor_get_text_key(in, key, sizeof(key))) return -1;
    if (strcmp(key, "id") == 0) {
      free(*rp_id);
      if (cbor_get_tstr(in, rp_id)) return -1;
    } else if (cbor_skip(in, 0)) return -1;
  }
  return 0;
}

static int parse_user_map(CborIn *in, DecodedMakeCredential *d) {
  uint8_t major = 0; uint64_t n = 0;
  if (cbor_read_head(in, &major, &n) || major != 5 || n > 32) return -1;
  for (uint64_t i = 0; i < n; i++) {
    char key[32];
    if (cbor_get_text_key(in, key, sizeof(key))) return -1;
    if (strcmp(key, "id") == 0) {
      free(d->user_handle); d->user_handle = NULL; d->user_handle_len = 0;
      if (cbor_get_bstr(in, &d->user_handle, &d->user_handle_len)) return -1;
    } else if (strcmp(key, "name") == 0) {
      free(d->user_name); if (cbor_get_tstr(in, &d->user_name)) return -1;
    } else if (strcmp(key, "displayName") == 0) {
      free(d->user_display_name); if (cbor_get_tstr(in, &d->user_display_name)) return -1;
    } else if (cbor_skip(in, 0)) return -1;
  }
  return 0;
}

static int parse_pubkey_params(CborIn *in, DecodedMakeCredential *d) {
  uint8_t major = 0; uint64_t n = 0;
  if (cbor_read_head(in, &major, &n) || major != 4 || n > CTAP2_MAX_CRED_LIST) return -1;
  int *algs = (int *)calloc(n ? (size_t)n : 1, sizeof(int));
  if (!algs) return -1;
  size_t count = 0;
  for (uint64_t i = 0; i < n; i++) {
    uint8_t m = 0; uint64_t pairs = 0;
    if (cbor_read_head(in, &m, &pairs) || m != 5 || pairs > 16) { free(algs); return -1; }
    bool have_alg = false;
    int64_t alg = 0;
    for (uint64_t j = 0; j < pairs; j++) {
      char key[32];
      if (cbor_get_text_key(in, key, sizeof(key))) { free(algs); return -1; }
      if (strcmp(key, "alg") == 0) {
        if (cbor_get_int(in, &alg)) { free(algs); return -1; }
        have_alg = true;
      } else if (cbor_skip(in, 0)) { free(algs); return -1; }
    }
    if (have_alg) algs[count++] = (int)alg;
  }
  free(d->algs);
  d->algs = algs;
  d->alg_count = count;
  return 0;
}

static int parse_cred_descriptors(CborIn *in, uint8_t ***ids, size_t **lens, size_t *count) {
  uint8_t major = 0; uint64_t n = 0;
  if (cbor_read_head(in, &major, &n) || major != 4 || n > CTAP2_MAX_CRED_LIST) return -1;
  uint8_t **out_ids = (uint8_t **)calloc(n ? (size_t)n : 1, sizeof(uint8_t *));
  size_t *out_lens = (size_t *)calloc(n ? (size_t)n : 1, sizeof(size_t));
  if (!out_ids || !out_lens) { free(out_ids); free(out_lens); return -1; }
  size_t out_count = 0;
  for (uint64_t i = 0; i < n; i++) {
    uint8_t m = 0; uint64_t pairs = 0;
    if (cbor_read_head(in, &m, &pairs) || m != 5 || pairs > 16) goto fail;
    uint8_t *id = NULL; size_t id_len = 0;
    for (uint64_t j = 0; j < pairs; j++) {
      char key[32];
      if (cbor_get_text_key(in, key, sizeof(key))) goto fail;
      if (strcmp(key, "id") == 0) {
        free(id); id = NULL; id_len = 0;
        if (cbor_get_bstr(in, &id, &id_len)) goto fail;
      } else if (cbor_skip(in, 0)) goto fail;
    }
    if (id && id_len > 0) { out_ids[out_count] = id; out_lens[out_count] = id_len; out_count++; }
    else free(id);
  }
  *ids = out_ids; *lens = out_lens; *count = out_count;
  return 0;
fail:
  for (size_t i = 0; i < out_count; i++) free(out_ids[i]);
  free(out_ids); free(out_lens);
  return -1;
}

static int parse_options(CborIn *in, bool *rk, bool *uv_required, bool *bad_up) {
  uint8_t major = 0; uint64_t n = 0;
  if (cbor_read_head(in, &major, &n) || major != 5 || n > 32) return -1;
  for (uint64_t i = 0; i < n; i++) {
    char key[32]; bool val = false;
    if (cbor_get_text_key(in, key, sizeof(key)) || cbor_get_bool(in, &val)) return -1;
    if (strcmp(key, "rk") == 0 && rk) *rk = val;
    else if (strcmp(key, "uv") == 0 && uv_required) *uv_required = val;
    else if (strcmp(key, "up") == 0 && bad_up && !val) *bad_up = true;
  }
  return 0;
}

static int decode_make_credential(const uint8_t *p, size_t n, DecodedMakeCredential *d) {
  memset(d, 0, sizeof(*d));
  d->discoverable = false;
  CborIn in = { .p = p, .n = n, .off = 0 };
  uint8_t major = 0; uint64_t pairs = 0;
  if (cbor_read_head(&in, &major, &pairs) || major != 5 || pairs > 32) return -1;
  bool bad_up = false;
  for (uint64_t i = 0; i < pairs; i++) {
    int64_t key = 0;
    if (cbor_get_int(&in, &key)) return -1;
    switch (key) {
      case 1: if (cbor_get_bstr(&in, &d->client_data_hash, &d->client_data_hash_len)) return -1; break;
      case 2: if (parse_rp_map(&in, &d->rp_id)) return -1; break;
      case 3: if (parse_user_map(&in, d)) return -1; break;
      case 4: if (parse_pubkey_params(&in, d)) return -1; break;
      case 5: if (parse_cred_descriptors(&in, &d->exclude_ids, &d->exclude_lens, &d->exclude_count)) return -1; break;
      case 7: if (parse_options(&in, &d->discoverable, &d->uv_required, &bad_up)) return -1; break;
      case 8: case 9: d->unsupported_pin_uv = true; if (cbor_skip(&in, 0)) return -1; break;
      default: if (cbor_skip(&in, 0)) return -1; break;
    }
  }
  if (bad_up) d->unsupported_pin_uv = true;
  return (in.off == in.n) ? 0 : -1;
}

static int decode_get_assertion(const uint8_t *p, size_t n, DecodedGetAssertion *d) {
  memset(d, 0, sizeof(*d));
  CborIn in = { .p = p, .n = n, .off = 0 };
  uint8_t major = 0; uint64_t pairs = 0;
  if (cbor_read_head(&in, &major, &pairs) || major != 5 || pairs > 32) return -1;
  bool bad_up = false;
  for (uint64_t i = 0; i < pairs; i++) {
    int64_t key = 0;
    if (cbor_get_int(&in, &key)) return -1;
    switch (key) {
      case 1: free(d->rp_id); if (cbor_get_tstr(&in, &d->rp_id)) return -1; break;
      case 2: free(d->client_data_hash); d->client_data_hash = NULL; d->client_data_hash_len = 0; if (cbor_get_bstr(&in, &d->client_data_hash, &d->client_data_hash_len)) return -1; break;
      case 3: if (parse_cred_descriptors(&in, &d->allow_ids, &d->allow_lens, &d->allow_count)) return -1; break;
      case 5: { bool rk_ignored = false; if (parse_options(&in, &rk_ignored, &d->uv_required, &bad_up)) return -1; break; }
      case 6: case 7: d->unsupported_pin_uv = true; if (cbor_skip(&in, 0)) return -1; break;
      default: if (cbor_skip(&in, 0)) return -1; break;
    }
  }
  if (bad_up) d->unsupported_pin_uv = true;
  return (in.off == in.n) ? 0 : -1;
}

/* ---- device state ------------------------------------------------------- */
typedef struct {
  uint32_t cid;
  bool active;
  uint8_t cmd;
  size_t expected;
  size_t received;
  uint8_t next_seq;
  uint8_t *buf;
} ChannelState;

struct SignetFidoCtapHid {
  bool enabled;
  SignetFidoService *fido;
  char *agent_id;
  char *device_name;
  uint16_t vendor_id;
  uint16_t product_id;
  uint16_t version;
  uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN];
  size_t max_msg_size;
  uint32_t next_cid;
  ChannelState channels[SIGNET_CTAPHID_MAX_CHANNELS];
#if defined(__linux__)
  int fd;
  GIOChannel *io;
  guint watch_id;
#endif
};

static uint32_t frame_cid(const uint8_t *f) {
  return ((uint32_t)f[0] << 24) | ((uint32_t)f[1] << 16) | ((uint32_t)f[2] << 8) | f[3];
}
static void put_cid(uint8_t *f, uint32_t cid) {
  f[0] = (uint8_t)(cid >> 24); f[1] = (uint8_t)(cid >> 16); f[2] = (uint8_t)(cid >> 8); f[3] = (uint8_t)cid;
}

static ChannelState *find_channel(SignetFidoCtapHid *dev, uint32_t cid, bool create) {
  ChannelState *free_slot = NULL;
  for (size_t i = 0; i < SIGNET_CTAPHID_MAX_CHANNELS; i++) {
    if (dev->channels[i].active && dev->channels[i].cid == cid) return &dev->channels[i];
    if (!dev->channels[i].active && !free_slot) free_slot = &dev->channels[i];
  }
  if (!create || !free_slot) return NULL;
  memset(free_slot, 0, sizeof(*free_slot));
  free_slot->cid = cid;
  return free_slot;
}

static void clear_channel(ChannelState *ch) {
  if (!ch) return;
  free(ch->buf);
  memset(ch, 0, sizeof(*ch));
}

static int build_response_frames(uint32_t cid, uint8_t cmd, const uint8_t *payload,
                                 size_t payload_len, ByteBuf *out) {
  if (payload_len > 0xffffu) return -1;
  uint8_t frame[SIGNET_CTAPHID_REPORT_SIZE];
  memset(frame, 0, sizeof(frame));
  put_cid(frame, cid);
  frame[4] = cmd;
  frame[5] = (uint8_t)(payload_len >> 8);
  frame[6] = (uint8_t)payload_len;
  size_t n0 = payload_len < 57 ? payload_len : 57;
  if (n0) memcpy(frame + 7, payload, n0);
  bb_put(out, frame, sizeof(frame));
  size_t off = n0;
  uint8_t seq = 0;
  while (off < payload_len) {
    memset(frame, 0, sizeof(frame));
    put_cid(frame, cid);
    frame[4] = seq++;
    size_t chunk = payload_len - off;
    if (chunk > 59) chunk = 59;
    memcpy(frame + 5, payload + off, chunk);
    bb_put(out, frame, sizeof(frame));
    off += chunk;
  }
  return out->err ? -1 : 0;
}

static int send_hid_error(uint32_t cid, uint8_t code, ByteBuf *out) {
  return build_response_frames(cid, CTAPHID_ERROR, &code, 1, out);
}

static int send_keepalive(uint32_t cid, ByteBuf *out) {
  uint8_t status_processing = 0x01; /* CTAPHID_KEEPALIVE_STATUS_PROCESSING */
  return build_response_frames(cid, CTAPHID_KEEPALIVE, &status_processing, 1, out);
}

static uint8_t status_to_ctap2(SignetFidoStatus st) {
  switch (st) {
    case SIGNET_FIDO_OK: return CTAP2_OK;
    case SIGNET_FIDO_ERR_BAD_REQUEST: return CTAP2_ERR_INVALID_PARAMETER;
    case SIGNET_FIDO_ERR_UNSUPPORTED_ALGORITHM: return CTAP2_ERR_UNSUPPORTED_ALGORITHM;
    case SIGNET_FIDO_ERR_EXCLUDED: return CTAP2_ERR_CREDENTIAL_EXCLUDED;
    case SIGNET_FIDO_ERR_NOT_FOUND: return CTAP2_ERR_NO_CREDENTIALS;
    case SIGNET_FIDO_ERR_UV_REQUIRED: return CTAP2_ERR_PIN_REQUIRED;
    case SIGNET_FIDO_ERR_NOT_CONFIGURED: return CTAP2_ERR_NOT_ALLOWED;
    case SIGNET_FIDO_ERR_INTERNAL: default: return CTAP2_ERR_OTHER;
  }
}

static int ctap2_get_info(SignetFidoCtapHid *dev, uint8_t **out, size_t *out_len) {
  ByteBuf b; bb_init(&b);
  bb_u8(&b, CTAP2_OK);
  cbor_map(&b, 7);
  cbor_uint(&b, 1); cbor_array(&b, 1); cbor_tstr(&b, "FIDO_2_0");
  cbor_uint(&b, 3); cbor_bstr(&b, dev->aaguid, SIGNET_FIDO_AAGUID_LEN);
  cbor_uint(&b, 4); cbor_map(&b, 4);
    cbor_tstr(&b, "rk"); cbor_bool(&b, true);
    cbor_tstr(&b, "up"); cbor_bool(&b, true);
    cbor_tstr(&b, "uv"); cbor_bool(&b, false);
    cbor_tstr(&b, "clientPin"); cbor_bool(&b, false);
  cbor_uint(&b, 5); cbor_uint(&b, dev->max_msg_size);
  cbor_uint(&b, 7); cbor_uint(&b, CTAP2_MAX_CRED_LIST);
  cbor_uint(&b, 8); cbor_uint(&b, 128);
  cbor_uint(&b, 9); cbor_array(&b, 1); cbor_tstr(&b, "usb");
  cbor_uint(&b, 10); cbor_array(&b, 1); cbor_map(&b, 2);
    cbor_tstr(&b, "alg"); cbor_int(&b, SIGNET_PASSKEY_COSE_ALG_ES256);
    cbor_tstr(&b, "type"); cbor_tstr(&b, "public-key");
  return bb_finish(&b, out, out_len);
}

static int ctap2_make_credential(SignetFidoCtapHid *dev, const uint8_t *p, size_t n,
                                  uint8_t **out, size_t *out_len) {
  DecodedMakeCredential d;
  if (decode_make_credential(p, n, &d) != 0) {
    decoded_mc_clear(&d);
    uint8_t e = CTAP2_ERR_INVALID_CBOR;
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = e; *out_len = 1; return 0;
  }
  if (d.unsupported_pin_uv || d.uv_required) {
    decoded_mc_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_UNSUPPORTED_OPTION; *out_len = 1; return 0;
  }
  if (!d.client_data_hash || d.client_data_hash_len != SIGNET_FIDO_CLIENT_DATA_HASH_LEN ||
      !d.rp_id || !d.user_handle || d.user_handle_len == 0 || !d.algs || d.alg_count == 0) {
    decoded_mc_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_MISSING_PARAMETER; *out_len = 1; return 0;
  }

  SignetFidoMakeCredentialRequest req = {
    .rp_id = d.rp_id,
    .client_data_hash = d.client_data_hash,
    .client_data_hash_len = d.client_data_hash_len,
    .user_handle = d.user_handle,
    .user_handle_len = d.user_handle_len,
    .user_name = d.user_name,
    .user_display_name = d.user_display_name,
    .discoverable = d.discoverable,
    .user_verification = SIGNET_FIDO_UV_DISCOURAGED,
    .pub_key_cred_params = d.algs,
    .pub_key_cred_param_count = d.alg_count,
    .exclude_credential_ids = (const uint8_t *const *)d.exclude_ids,
    .exclude_credential_id_lens = d.exclude_lens,
    .exclude_credential_count = d.exclude_count,
  };
  SignetFidoMakeCredentialResult res;
  SignetFidoError err;
  SignetFidoStatus st = signet_fido_make_credential(dev->fido, dev->agent_id, &req, &res, &err);
  if (st != SIGNET_FIDO_OK) {
    decoded_mc_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = status_to_ctap2(st); *out_len = 1; return 0;
  }

  ByteBuf b; bb_init(&b);
  bb_u8(&b, CTAP2_OK);
  cbor_map(&b, 3);
  cbor_uint(&b, 1); cbor_tstr(&b, "none");
  cbor_uint(&b, 2); cbor_bstr(&b, res.auth_data, res.auth_data_len);
  cbor_uint(&b, 3); cbor_map(&b, 0);
  int rc = bb_finish(&b, out, out_len);
  signet_fido_make_credential_result_clear(&res);
  decoded_mc_clear(&d);
  return rc;
}

static int ctap2_get_assertion(SignetFidoCtapHid *dev, const uint8_t *p, size_t n,
                               uint8_t **out, size_t *out_len) {
  DecodedGetAssertion d;
  if (decode_get_assertion(p, n, &d) != 0) {
    decoded_ga_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_INVALID_CBOR; *out_len = 1; return 0;
  }
  if (d.unsupported_pin_uv || d.uv_required) {
    decoded_ga_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_UNSUPPORTED_OPTION; *out_len = 1; return 0;
  }
  if (!d.rp_id || !d.client_data_hash || d.client_data_hash_len != SIGNET_FIDO_CLIENT_DATA_HASH_LEN) {
    decoded_ga_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_MISSING_PARAMETER; *out_len = 1; return 0;
  }

  SignetFidoGetAssertionRequest req = {
    .rp_id = d.rp_id,
    .client_data_hash = d.client_data_hash,
    .client_data_hash_len = d.client_data_hash_len,
    .user_verification = SIGNET_FIDO_UV_DISCOURAGED,
    .allow_credential_ids = (const uint8_t *const *)d.allow_ids,
    .allow_credential_id_lens = d.allow_lens,
    .allow_credential_count = d.allow_count,
  };
  SignetFidoGetAssertionResult res;
  SignetFidoError err;
  SignetFidoStatus st = signet_fido_get_assertion(dev->fido, dev->agent_id, &req, &res, &err);
  if (st != SIGNET_FIDO_OK) {
    decoded_ga_clear(&d);
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = status_to_ctap2(st); *out_len = 1; return 0;
  }

  ByteBuf b; bb_init(&b);
  bb_u8(&b, CTAP2_OK);
  cbor_map(&b, 5);
  cbor_uint(&b, 1); cbor_map(&b, 2);
    cbor_tstr(&b, "id"); cbor_bstr(&b, res.credential_id, res.credential_id_len);
    cbor_tstr(&b, "type"); cbor_tstr(&b, "public-key");
  cbor_uint(&b, 2); cbor_bstr(&b, res.auth_data, res.auth_data_len);
  cbor_uint(&b, 3); cbor_bstr(&b, res.signature_der, res.signature_der_len);
  cbor_uint(&b, 4); cbor_map(&b, 1); cbor_tstr(&b, "id"); cbor_bstr(&b, res.user_handle, res.user_handle_len);
  cbor_uint(&b, 5); cbor_uint(&b, 1);
  int rc = bb_finish(&b, out, out_len);
  signet_fido_get_assertion_result_clear(&res);
  decoded_ga_clear(&d);
  return rc;
}

static int process_ctap2(SignetFidoCtapHid *dev, const uint8_t *payload, size_t payload_len,
                         uint8_t **out, size_t *out_len) {
  if (!payload || payload_len == 0) {
    *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_INVALID_LENGTH; *out_len = 1; return 0;
  }
  uint8_t cmd = payload[0];
  const uint8_t *params = payload + 1;
  size_t params_len = payload_len - 1;
  switch (cmd) {
    case CTAP2_GET_INFO: return ctap2_get_info(dev, out, out_len);
    case CTAP2_MAKE_CREDENTIAL: return ctap2_make_credential(dev, params, params_len, out, out_len);
    case CTAP2_GET_ASSERTION: return ctap2_get_assertion(dev, params, params_len, out, out_len);
    case CTAP2_CLIENT_PIN:
    case CTAP2_SELECTION:
    case CTAP2_RESET:
    case CTAP2_GET_NEXT_ASSERTION:
      *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_UNSUPPORTED_OPTION; *out_len = 1; return 0;
    default:
      *out = (uint8_t *)malloc(1); if (!*out) return -1; **out = CTAP2_ERR_INVALID_COMMAND; *out_len = 1; return 0;
  }
}

static uint32_t allocate_cid(SignetFidoCtapHid *dev) {
  uint32_t cid = dev->next_cid++;
  if (dev->next_cid == 0 || dev->next_cid == SIGNET_CTAPHID_BROADCAST_CID) dev->next_cid = 1;
  if (cid == 0 || cid == SIGNET_CTAPHID_BROADCAST_CID) cid = dev->next_cid++;
  return cid;
}

static int process_complete(SignetFidoCtapHid *dev, uint32_t cid, uint8_t cmd,
                            const uint8_t *payload, size_t payload_len, ByteBuf *responses) {
  if (cmd == CTAPHID_INIT) {
    if (payload_len != 8) return send_hid_error(cid, CTAPHID_ERR_INVALID_LEN, responses);
    uint8_t resp[17];
    memcpy(resp, payload, 8);
    uint32_t new_cid = allocate_cid(dev);
    resp[8] = (uint8_t)(new_cid >> 24); resp[9] = (uint8_t)(new_cid >> 16);
    resp[10] = (uint8_t)(new_cid >> 8); resp[11] = (uint8_t)new_cid;
    resp[12] = 2; resp[13] = 0; resp[14] = 1; resp[15] = 0;
    resp[16] = CTAPHID_CAP_CBOR;
    return build_response_frames(cid, CTAPHID_INIT, resp, sizeof(resp), responses);
  }
  if (cid == SIGNET_CTAPHID_BROADCAST_CID) return send_hid_error(cid, CTAPHID_ERR_INVALID_CHANNEL, responses);
  if (cmd == CTAPHID_PING) return build_response_frames(cid, CTAPHID_PING, payload, payload_len, responses);
  if (cmd == CTAPHID_CBOR) {
    if (payload_len > 0 && (payload[0] == CTAP2_MAKE_CREDENTIAL || payload[0] == CTAP2_GET_ASSERTION)) {
      if (send_keepalive(cid, responses) != 0) return -1;
    }
    uint8_t *ctap = NULL; size_t ctap_len = 0;
    if (process_ctap2(dev, payload, payload_len, &ctap, &ctap_len) != 0) return send_hid_error(cid, CTAPHID_ERR_OTHER, responses);
    int rc = build_response_frames(cid, CTAPHID_CBOR, ctap, ctap_len, responses);
    free(ctap);
    return rc;
  }
  if (cmd == CTAPHID_CANCEL) {
    ChannelState *ch = find_channel(dev, cid, false);
    clear_channel(ch);
    return 0; /* CTAPHID CANCEL is best-effort and normally has no response. */
  }
  return send_hid_error(cid, CTAPHID_ERR_INVALID_CMD, responses);
}

static int process_frame(SignetFidoCtapHid *dev, const uint8_t *frame, ByteBuf *responses) {
  uint32_t cid = frame_cid(frame);
  uint8_t tag = frame[4];
  if (tag & 0x80u) {
    uint8_t cmd = tag;
    size_t total = ((size_t)frame[5] << 8) | frame[6];
    if (total > dev->max_msg_size) return send_hid_error(cid, CTAPHID_ERR_INVALID_LEN, responses);
    size_t first = total < 57 ? total : 57;
    if (total <= 57) return process_complete(dev, cid, cmd, frame + 7, first, responses);
    ChannelState *existing = find_channel(dev, cid, false);
    if (existing && existing->active) return send_hid_error(cid, CTAPHID_ERR_CHANNEL_BUSY, responses);
    ChannelState *ch = find_channel(dev, cid, true);
    if (!ch) return send_hid_error(cid, CTAPHID_ERR_CHANNEL_BUSY, responses);
    ch->buf = (uint8_t *)malloc(total);
    if (!ch->buf) { clear_channel(ch); return send_hid_error(cid, CTAPHID_ERR_OTHER, responses); }
    ch->active = true;
    ch->cmd = cmd;
    ch->expected = total;
    ch->received = first;
    ch->next_seq = 0;
    memcpy(ch->buf, frame + 7, first);
    return 1;
  }

  ChannelState *ch = find_channel(dev, cid, false);
  if (!ch || !ch->active) return send_hid_error(cid, CTAPHID_ERR_INVALID_SEQ, responses);
  uint8_t seq = (uint8_t)(tag & 0x7fu);
  if (seq != ch->next_seq) { clear_channel(ch); return send_hid_error(cid, CTAPHID_ERR_INVALID_SEQ, responses); }
  ch->next_seq++;
  size_t remain = ch->expected - ch->received;
  size_t chunk = remain < 59 ? remain : 59;
  memcpy(ch->buf + ch->received, frame + 5, chunk);
  ch->received += chunk;
  if (ch->received == ch->expected) {
    uint8_t *payload = ch->buf;
    size_t payload_len = ch->expected;
    uint8_t cmd = ch->cmd;
    ch->buf = NULL;
    clear_channel(ch);
    int rc = process_complete(dev, cid, cmd, payload, payload_len, responses);
    free(payload);
    return rc;
  }
  return 1;
}

SignetFidoCtapHid *signet_fido_ctaphid_new(const SignetFidoCtapHidConfig *cfg) {
  SignetFidoCtapHid *dev = (SignetFidoCtapHid *)calloc(1, sizeof(*dev));
  if (!dev) return NULL;
  dev->enabled = cfg ? cfg->enabled : false;
  dev->fido = cfg ? cfg->fido : NULL;
  dev->agent_id = g_strdup((cfg && cfg->agent_id && cfg->agent_id[0]) ? cfg->agent_id : "default");
  dev->device_name = g_strdup((cfg && cfg->device_name) ? cfg->device_name : "Signet virtual FIDO2 authenticator");
  dev->vendor_id = (cfg && cfg->vendor_id) ? cfg->vendor_id : 0x1209;
  dev->product_id = (cfg && cfg->product_id) ? cfg->product_id : 0x5074;
  dev->version = (cfg && cfg->version) ? cfg->version : 1;
  dev->max_msg_size = (cfg && cfg->max_msg_size) ? cfg->max_msg_size : SIGNET_CTAPHID_MAX_MSG_SIZE;
  if (dev->max_msg_size > SIGNET_CTAPHID_MAX_MSG_SIZE) dev->max_msg_size = SIGNET_CTAPHID_MAX_MSG_SIZE;
  if (cfg && cfg->aaguid) memcpy(dev->aaguid, cfg->aaguid, SIGNET_FIDO_AAGUID_LEN);
  else (void)signet_fido_parse_aaguid(SIGNET_FIDO_DEFAULT_AAGUID, dev->aaguid);
  dev->next_cid = 0x01020304u;
#if defined(__linux__)
  dev->fd = -1;
#endif
  if (!dev->agent_id || !dev->device_name) { signet_fido_ctaphid_free(dev); return NULL; }
  return dev;
}

int signet_ctaphid_test_process_frames(SignetFidoService *fido,
                                       const char *agent_id,
                                       const uint8_t aaguid[SIGNET_FIDO_AAGUID_LEN],
                                       const uint8_t *frames,
                                       size_t frames_len,
                                       uint8_t **out_frames,
                                       size_t *out_frames_len) {
  if (out_frames) *out_frames = NULL;
  if (out_frames_len) *out_frames_len = 0;
  if (!frames || !out_frames || !out_frames_len || frames_len == 0 ||
      frames_len % SIGNET_CTAPHID_REPORT_SIZE != 0) return -1;
  SignetFidoCtapHidConfig cfg = { .enabled = true, .fido = fido, .agent_id = agent_id, .aaguid = aaguid };
  SignetFidoCtapHid *dev = signet_fido_ctaphid_new(&cfg);
  if (!dev) return -1;
  ByteBuf responses; bb_init(&responses);
  int rc = 1;
  for (size_t off = 0; off < frames_len; off += SIGNET_CTAPHID_REPORT_SIZE) {
    int r = process_frame(dev, frames + off, &responses);
    if (r < 0) { rc = -1; break; }
    if (responses.len > 0) rc = 0;
  }
  if (rc == 0) rc = bb_finish(&responses, out_frames, out_frames_len);
  else bb_free(&responses);
  signet_fido_ctaphid_free(dev);
  return rc;
}

#if defined(__linux__)
static const uint8_t signet_fido_report_desc[] = {
  0x06, 0xd0, 0xf1,       /* Usage Page (FIDO Alliance) */
  0x09, 0x01,             /* Usage (U2F Authenticator Device) */
  0xa1, 0x01,             /* Collection (Application) */
  0x09, 0x20,             /*   Usage (Input Report Data) */
  0x15, 0x00,             /*   Logical Minimum (0) */
  0x26, 0xff, 0x00,       /*   Logical Maximum (255) */
  0x75, 0x08,             /*   Report Size (8) */
  0x95, 0x40,             /*   Report Count (64) */
  0x81, 0x02,             /*   Input (Data,Var,Abs) */
  0x09, 0x21,             /*   Usage (Output Report Data) */
  0x15, 0x00,
  0x26, 0xff, 0x00,
  0x75, 0x08,
  0x95, 0x40,
  0x91, 0x02,             /*   Output (Data,Var,Abs) */
  0xc0
};

static int uhid_write_event(SignetFidoCtapHid *dev, const struct uhid_event *ev) {
  ssize_t n;
  do {
    n = write(dev->fd, ev, sizeof(*ev));
  } while (n < 0 && errno == EINTR);
  return n == (ssize_t)sizeof(*ev) ? 0 : -1;
}

static int uhid_send_frames(SignetFidoCtapHid *dev, const uint8_t *frames, size_t len) {
  for (size_t off = 0; off < len; off += SIGNET_CTAPHID_REPORT_SIZE) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;
    ev.u.input2.size = SIGNET_CTAPHID_REPORT_SIZE;
    memcpy(ev.u.input2.data, frames + off, SIGNET_CTAPHID_REPORT_SIZE);
    if (uhid_write_event(dev, &ev) != 0) return -1;
  }
  return 0;
}

static gboolean uhid_io_cb(GIOChannel *source, GIOCondition cond, gpointer data) {
  (void)source;
  SignetFidoCtapHid *dev = (SignetFidoCtapHid *)data;
  if (!dev || (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))) return G_SOURCE_REMOVE;
  for (;;) {
    struct uhid_event ev;
    ssize_t n = read(dev->fd, &ev, sizeof(ev));
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      g_warning("[signetd] virtual CTAP read failed: %s", strerror(errno));
      return G_SOURCE_REMOVE;
    }
    if (n == 0) break;
    if (n != (ssize_t)sizeof(ev)) continue;
    if (ev.type == UHID_OUTPUT) {
      if (ev.u.output.size < SIGNET_CTAPHID_REPORT_SIZE) continue;
      ByteBuf responses; bb_init(&responses);
      int rc = process_frame(dev, ev.u.output.data, &responses);
      if (rc >= 0 && responses.len > 0) {
        if (uhid_send_frames(dev, responses.data, responses.len) != 0)
          g_warning("[signetd] virtual CTAP write failed: %s", strerror(errno));
      }
      bb_free(&responses);
    }
  }
  return G_SOURCE_CONTINUE;
}

int signet_fido_ctaphid_start(SignetFidoCtapHid *dev) {
  if (!dev || !dev->enabled) return 0;
  if (!dev->fido || !signet_fido_service_is_enabled(dev->fido)) {
    g_warning("[signetd] virtual CTAP requested but passkey service is not enabled");
    return -1;
  }
  if (dev->fd >= 0) return 0;
  int fd = open("/dev/uhid", O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    g_warning("[signetd] failed to open /dev/uhid for virtual CTAP: %s", strerror(errno));
    return -1;
  }
  dev->fd = fd;

  struct uhid_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = UHID_CREATE2;
  g_strlcpy((char *)ev.u.create2.name, dev->device_name, sizeof(ev.u.create2.name));
  memcpy(ev.u.create2.rd_data, signet_fido_report_desc, sizeof(signet_fido_report_desc));
  ev.u.create2.rd_size = sizeof(signet_fido_report_desc);
  ev.u.create2.bus = BUS_USB;
  ev.u.create2.vendor = dev->vendor_id;
  ev.u.create2.product = dev->product_id;
  ev.u.create2.version = dev->version;
  ev.u.create2.country = 0;
  if (uhid_write_event(dev, &ev) != 0) {
    g_warning("[signetd] UHID_CREATE2 failed for virtual CTAP: %s", strerror(errno));
    close(dev->fd); dev->fd = -1;
    return -1;
  }
  dev->io = g_io_channel_unix_new(dev->fd);
  g_io_channel_set_encoding(dev->io, NULL, NULL);
  g_io_channel_set_buffered(dev->io, FALSE);
  dev->watch_id = g_io_add_watch(dev->io, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, uhid_io_cb, dev);
  g_message("[signetd] virtual CTAP-HID device created via /dev/uhid for agent_id=%s", dev->agent_id);
  return 0;
}

void signet_fido_ctaphid_stop(SignetFidoCtapHid *dev) {
  if (!dev) return;
  if (dev->watch_id) { g_source_remove(dev->watch_id); dev->watch_id = 0; }
  if (dev->io) { g_io_channel_unref(dev->io); dev->io = NULL; }
  if (dev->fd >= 0) {
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    (void)uhid_write_event(dev, &ev);
    close(dev->fd);
    dev->fd = -1;
  }
}
#else
int signet_fido_ctaphid_start(SignetFidoCtapHid *dev) {
  if (!dev || !dev->enabled) return 0;
  g_warning("[signetd] virtual CTAP-HID requires Linux /dev/uhid; disabled on this platform");
  return -1;
}

void signet_fido_ctaphid_stop(SignetFidoCtapHid *dev) { (void)dev; }
#endif

void signet_fido_ctaphid_free(SignetFidoCtapHid *dev) {
  if (!dev) return;
  signet_fido_ctaphid_stop(dev);
  for (size_t i = 0; i < SIGNET_CTAPHID_MAX_CHANNELS; i++) clear_channel(&dev->channels[i]);
  g_free(dev->agent_id);
  g_free(dev->device_name);
  free(dev);
}
