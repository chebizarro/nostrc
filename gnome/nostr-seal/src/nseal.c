/*
 * nseal.c — .nsealed container: header, key stanzas, chunk stream, index.
 * SPDX-License-Identifier: MIT
 *
 * Bead nostrc-da9c. Byte layout: gnome/nostr-seal/README.md ("Format").
 *
 * Primitive reuse (no new KDFs, no new AEAD constructions):
 *   - key agreement + per-recipient wrap: NIP-44 v2 (nostr_nip44_convkey,
 *     nostr_nip44_{en,de}crypt_v2_with_convkey) of the 32-byte file key,
 *     sender = a fresh ephemeral secp256k1 key per stanza;
 *   - passphrase wrap: NIP-49 (scrypt → XChaCha20-Poly1305) of the file key,
 *     i.e. the stanza is a literal ncryptsec1 string;
 *   - body: porthome D4 (nh_porthome_encrypt_chunk / _encrypt_manifest)
 *     rooted at HKDF(salt "nostr-seal/v1", file_key, "root"); the header
 *     carries a key commitment HKDF(…, "commit").
 */

#include "nostr-seal.h"
#include "nseal-private.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>

#include "nh_porthome_crypto.h"
#include <nostr/nip19/nip19.h>
#include <nostr/nip44/nip44.h>
#include <nostr/nip49/nip49.h>

G_DEFINE_QUARK(nseal-error-quark, nseal_error)

/* In-tree HKDF-SHA256 (nips/nip44/src/core/nip44_hkdf_hmac.c, linked via
 * nostr_nip44_core) — the same TU porthome reuses. */
extern int nip44_hkdf_extract(const uint8_t *salt, size_t salt_len,
                              const uint8_t *ikm, size_t ikm_len, uint8_t prk_out[32]);
extern int nip44_hkdf_expand(const uint8_t prk[32], const uint8_t *info, size_t info_len,
                             uint8_t okm_out[], size_t okm_len);

/* ─── Layout constants (README "Format") ─────────────────────────────── */

#define HDR_FIXED_LEN        44   /* magic7 ver1 flags1 chunk_log2 1 count2 commit32 */
#define HDR_COMMIT_OFF       12
#define STANZA_HDR_LEN        3   /* type1 len2 */
#define NIP44_PAYLOAD_B64_LEN 132 /* base64(1 + 32 + (2+32) + 32) for a 32-byte pt */
#define NIP44_STANZA_LEN     (32 + 32 + NIP44_PAYLOAD_B64_LEN)
#define NIP49_STANZA_MAX     256
#define IDX_MAGIC            "NSEALIDX"
#define IDX_VERSION          0x01
#define IDX_FIXED_LEN        (8 + 1 + 1 + 2 + 32 + 8 + 8)  /* magic ver log2 rsv hdr_sha total n */
#define FOOTER_MAGIC         "NSEALEND"
#define FOOTER_LEN           (8 + 32 + 8)
#define CHUNK_INDEX_LEN       8
#define NSEAL_MAX_CHUNKS     ((guint64)1 << 22)

struct _NsealHeader {
  GBytes  *raw;           /* exact header bytes (hashed into the index) */
  guint    chunk_log2;
  uint8_t  commit[32];    /* key commitment */
  GPtrArray *stanzas;     /* NsealStanza* */
};

typedef struct {
  NsealStanzaType type;
  uint8_t recipient[32];  /* NIP44 */
  uint8_t ephemeral[32];  /* NIP44 */
  char   *text;           /* NIP44: base64 payload; NIP49: ncryptsec1… */
} NsealStanza;

static void stanza_free(gpointer p) {
  NsealStanza *s = p;
  if (!s) return;
  g_free(s->text);
  g_free(s);
}

/* ─── Small helpers ──────────────────────────────────────────────────── */

static void put_be16(uint8_t *p, guint16 v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static guint16 get_be16(const uint8_t *p) { return (guint16)((p[0] << 8) | p[1]); }
static void put_be64(uint8_t *p, guint64 v) {
  for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}
static guint64 get_be64(const uint8_t *p) {
  guint64 v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

gboolean nseal_write_all(int fd, const void *buf, gsize len, GError **error) {
  const uint8_t *p = buf;
  while (len > 0) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "write failed: %s", g_strerror(e));
      return FALSE;
    }
    p += n; len -= (gsize)n;
  }
  return TRUE;
}

/* Read until len bytes or EOF. Returns bytes read, or -1 on error. */
static gssize read_full(int fd, void *buf, gsize len, GError **error) {
  uint8_t *p = buf;
  gsize got = 0;
  while (got < len) {
    ssize_t n = read(fd, p + got, len - got);
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "read failed: %s", g_strerror(e));
      return -1;
    }
    if (n == 0) break;
    got += (gsize)n;
  }
  return (gssize)got;
}

static gboolean pread_exact(int fd, void *buf, gsize len, guint64 off, GError **error) {
  uint8_t *p = buf;
  gsize got = 0;
  while (got < len) {
    ssize_t n = pread(fd, p + got, len - got, (off_t)(off + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      int e = errno;
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_IO, "read failed: %s", g_strerror(e));
      return FALSE;
    }
    if (n == 0) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_INTEGRITY,
                  "sealed file is truncated (wanted %" G_GSIZE_FORMAT " bytes at offset %" G_GUINT64_FORMAT ")",
                  len, off);
      return FALSE;
    }
    got += (gsize)n;
  }
  return TRUE;
}

static gboolean random_bytes(uint8_t *out, gsize n, GError **error) {
  if (RAND_bytes(out, (int)n) != 1) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "system RNG failed");
    return FALSE;
  }
  return TRUE;
}

/* Key schedule (README "Keys"): one HKDF-extract under a nostr-seal salt,
 * two expansions. `root` feeds the porthome chunk/manifest seal; `commit`
 * is stored in the header so every recipient provably holds the same
 * file_key (ChaCha20-Poly1305 is not key-committing on its own). */
static gboolean key_schedule(const uint8_t file_key[32], uint8_t root[32], uint8_t commit[32],
                             GError **error) {
  static const char SALT[] = "nostr-seal/v1";
  uint8_t prk[32];
  gboolean ok =
    nip44_hkdf_extract((const uint8_t *)SALT, sizeof SALT - 1, file_key, 32, prk) == 0 &&
    nip44_hkdf_expand(prk, (const uint8_t *)"root", 4, root, 32) == 0 &&
    nip44_hkdf_expand(prk, (const uint8_t *)"commit", 6, commit, 32) == 0;
  OPENSSL_cleanse(prk, sizeof prk);
  if (!ok) {
    OPENSSL_cleanse(root, 32);
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "key derivation failed");
  }
  return ok;
}

static secp256k1_context *secp_ctx(void) {
  static secp256k1_context *ctx = NULL;
  if (g_once_init_enter(&ctx)) {
    secp256k1_context *c = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    uint8_t seed[32];
    /* Randomization is side-channel hardening only; an unrandomized
     * context is still correct, so a failure here is not fatal. */
    if (RAND_bytes(seed, sizeof seed) == 1 && !secp256k1_context_randomize(c, seed))
      g_debug("nostr-seal: secp256k1 context randomization failed");
    OPENSSL_cleanse(seed, sizeof seed);
    g_once_init_leave(&ctx, c);
  }
  return ctx;
}

static gboolean pubkey_on_curve(const uint8_t pk[32]) {
  secp256k1_xonly_pubkey x;
  return secp256k1_xonly_pubkey_parse(secp_ctx(), &x, pk) == 1;
}

static gboolean ephemeral_keypair(uint8_t sk[32], uint8_t pk[32], GError **error) {
  secp256k1_keypair kp;
  secp256k1_xonly_pubkey x;
  for (int tries = 0; tries < 8; tries++) {
    if (!random_bytes(sk, 32, error)) return FALSE;
    if (secp256k1_keypair_create(secp_ctx(), &kp, sk) == 1 &&
        secp256k1_keypair_xonly_pub(secp_ctx(), &x, NULL, &kp) == 1 &&
        secp256k1_xonly_pubkey_serialize(secp_ctx(), pk, &x) == 1) {
      OPENSSL_cleanse(&kp, sizeof kp);
      return TRUE;
    }
  }
  OPENSSL_cleanse(sk, 32);
  OPENSSL_cleanse(&kp, sizeof kp);
  g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "could not generate ephemeral key");
  return FALSE;
}

gboolean nseal_parse_pubkey(const char *text, uint8_t out[32], GError **error) {
  if (!text) { g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "missing public key"); return FALSE; }
  if (g_str_has_prefix(text, "nostr:")) text += 6;
  gboolean ok = FALSE;
  if (g_str_has_prefix(text, "npub1")) {
    ok = nostr_nip19_decode_npub(text, out) == 0;
  } else if (strlen(text) == 64) {
    ok = TRUE;
    for (int i = 0; i < 32 && ok; i++) {
      int hi = g_ascii_xdigit_value(text[2*i]), lo = g_ascii_xdigit_value(text[2*i+1]);
      if (hi < 0 || lo < 0) ok = FALSE; else out[i] = (uint8_t)((hi << 4) | lo);
    }
  }
  if (ok && !pubkey_on_curve(out)) ok = FALSE;
  if (!ok) {
    memset(out, 0, 32);
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                "'%s' is not an npub or 64-hex secp256k1 public key", text);
  }
  return ok;
}

char *nseal_pubkey_to_npub(const uint8_t pk[32]) {
  char *bech = NULL;
  if (nostr_nip19_encode_npub(pk, &bech) != 0 || !bech) { free(bech); return NULL; }
  char *dup = g_strdup(bech);
  free(bech);
  return dup;
}

/* NFKC per NIP-49, via GLib (same hook nip49_g.c installs). */
static int nfkc_cb(const char *in, char **out) {
  gchar *n = g_utf8_normalize(in, -1, G_NORMALIZE_ALL_COMPOSE);
  if (!n) return -1;
  *out = n;
  return 0;
}
static void ensure_nip49_normalizer(void) {
  static gsize once = 0;
  if (g_once_init_enter(&once)) {
    nostr_nip49_set_normalize_cb(nfkc_cb);
    g_once_init_leave(&once, 1);
  }
}

/* log_n of an ncryptsec1 string without running scrypt. */
static gboolean ncryptsec_log_n(const char *bech, guint8 *out_log_n) {
  char *hrp = NULL; uint8_t *d5 = NULL, *d8 = NULL; size_t l5 = 0, l8 = 0;
  gboolean ok = FALSE;
  if (nostr_b32_decode(bech, &hrp, &d5, &l5) == 0 && hrp && strcmp(hrp, "ncryptsec") == 0 &&
      nostr_b32_to_8bit(d5, l5, &d8, &l8) == 0 && l8 == 91 && d8[0] == 0x02) {
    *out_log_n = d8[1];
    ok = TRUE;
  }
  free(hrp); free(d5); free(d8);
  return ok;
}

/* ─── Header ─────────────────────────────────────────────────────────── */

void nseal_header_free(NsealHeader *h) {
  if (!h) return;
  g_clear_pointer(&h->raw, g_bytes_unref);
  g_clear_pointer(&h->stanzas, g_ptr_array_unref);
  g_free(h);
}

guint nseal_header_chunk_log2(const NsealHeader *h) { return h->chunk_log2; }
gsize nseal_header_n_stanzas(const NsealHeader *h) { return h->stanzas->len; }
NsealStanzaType nseal_header_stanza_type(const NsealHeader *h, gsize i) {
  return ((NsealStanza *)g_ptr_array_index(h->stanzas, i))->type;
}
const uint8_t *nseal_header_stanza_recipient(const NsealHeader *h, gsize i) {
  NsealStanza *s = g_ptr_array_index(h->stanzas, i);
  return s->type == NSEAL_STANZA_NIP44 ? s->recipient : NULL;
}
gboolean nseal_header_is_passphrase(const NsealHeader *h) {
  return h->stanzas->len == 1 &&
         ((NsealStanza *)g_ptr_array_index(h->stanzas, 0))->type == NSEAL_STANZA_NIP49;
}
gboolean nseal_header_has_recipient(const NsealHeader *h, const uint8_t pk[32]) {
  for (guint i = 0; i < h->stanzas->len; i++) {
    NsealStanza *s = g_ptr_array_index(h->stanzas, i);
    if (s->type == NSEAL_STANZA_NIP44 && memcmp(s->recipient, pk, 32) == 0) return TRUE;
  }
  return FALSE;
}

static gboolean is_b64_char(char c) {
  return g_ascii_isalnum(c) || c == '+' || c == '/' || c == '=';
}

NsealHeader *nseal_header_read_fd(int fd, GError **error) {
  uint8_t fixed[HDR_FIXED_LEN];
  if (!pread_exact(fd, fixed, sizeof fixed, 0, error)) {
    if (error && *error && (*error)->code == NSEAL_ERROR_INTEGRITY) {
      g_clear_error(error);
      g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT, "not a .nsealed file (too short)");
    }
    return NULL;
  }
  if (memcmp(fixed, NSEAL_MAGIC, 7) != 0) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT, "not a .nsealed file (bad magic)");
    return NULL;
  }
  if (fixed[7] != NSEAL_FORMAT_VERSION) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_UNSUPPORTED, "unsupported .nsealed version %u", fixed[7]);
    return NULL;
  }
  if (fixed[8] != 0) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_UNSUPPORTED, "unsupported header flags 0x%02x", fixed[8]);
    return NULL;
  }
  guint chunk_log2 = fixed[9];
  guint count = get_be16(fixed + 10);
  if (chunk_log2 < NSEAL_CHUNK_LOG2_MIN || chunk_log2 > NSEAL_CHUNK_LOG2_MAX) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT, "chunk size 2^%u out of range", chunk_log2);
    return NULL;
  }
  if (count == 0 || count > NSEAL_MAX_STANZAS) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT, "bad recipient count %u", count);
    return NULL;
  }

  GByteArray *raw = g_byte_array_new();
  g_byte_array_append(raw, fixed, sizeof fixed);
  NsealHeader *h = g_new0(NsealHeader, 1);
  h->chunk_log2 = chunk_log2;
  memcpy(h->commit, fixed + HDR_COMMIT_OFF, 32);
  h->stanzas = g_ptr_array_new_with_free_func(stanza_free);
  guint64 off = sizeof fixed;
  gboolean have_nip49 = FALSE;

  for (guint i = 0; i < count; i++) {
    uint8_t sh[STANZA_HDR_LEN];
    if (!pread_exact(fd, sh, sizeof sh, off, error)) goto fail;
    guint type = sh[0], len = get_be16(sh + 1);
    off += sizeof sh;
    if (type != NSEAL_STANZA_NIP44 && type != NSEAL_STANZA_NIP49) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_UNSUPPORTED, "unknown key stanza type 0x%02x", type);
      goto fail;
    }
    if ((type == NSEAL_STANZA_NIP44 && len != NIP44_STANZA_LEN) ||
        (type == NSEAL_STANZA_NIP49 && (len < 10 || len > NIP49_STANZA_MAX))) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT, "key stanza %u has bad length %u", i, len);
      goto fail;
    }
    uint8_t *body = g_malloc(len);
    if (!pread_exact(fd, body, len, off, error)) { g_free(body); goto fail; }
    off += len;
    g_byte_array_append(raw, sh, sizeof sh);
    g_byte_array_append(raw, body, len);

    NsealStanza *s = g_new0(NsealStanza, 1);
    s->type = (NsealStanzaType)type;
    gboolean ok = TRUE;
    if (type == NSEAL_STANZA_NIP44) {
      memcpy(s->recipient, body, 32);
      memcpy(s->ephemeral, body + 32, 32);
      for (guint k = 64; k < len && ok; k++) ok = is_b64_char((char)body[k]);
      if (ok) s->text = g_strndup((const char *)body + 64, len - 64);
      ok = ok && pubkey_on_curve(s->recipient) && pubkey_on_curve(s->ephemeral);
      for (guint k = 0; k < h->stanzas->len && ok; k++) {
        NsealStanza *o = g_ptr_array_index(h->stanzas, k);
        if (o->type == NSEAL_STANZA_NIP44 && memcmp(o->recipient, s->recipient, 32) == 0) ok = FALSE;
      }
    } else {
      for (guint k = 0; k < len && ok; k++) ok = g_ascii_isalnum((char)body[k]);
      if (ok) s->text = g_strndup((const char *)body, len);
      guint8 log_n = 0;
      ok = ok && g_str_has_prefix(s->text, "ncryptsec1") && ncryptsec_log_n(s->text, &log_n);
      have_nip49 = TRUE;
    }
    g_free(body);
    g_ptr_array_add(h->stanzas, s);
    if (!ok) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT, "key stanza %u is malformed", i);
      goto fail;
    }
  }
  /* A passphrase stanza is only valid alone (README "Stanzas"). */
  if (have_nip49 && count != 1) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_FORMAT,
                        "passphrase stanza mixed with other recipients");
    goto fail;
  }
  h->raw = g_byte_array_free_to_bytes(raw);
  return h;

fail:
  g_byte_array_unref(raw);
  nseal_header_free(h);
  return NULL;
}

/* ─── Key wrapping ───────────────────────────────────────────────────── */

static gboolean wrap_nip44(const uint8_t recipient[32], const uint8_t file_key[32],
                           GByteArray *out, GError **error) {
  uint8_t esk[32], epk[32], ck[32];
  char *payload = NULL;
  gboolean ok = FALSE;
  if (!ephemeral_keypair(esk, epk, error)) return FALSE;
  if (nostr_nip44_convkey(esk, recipient, ck) != 0) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "recipient key is not on secp256k1");
    goto out;
  }
  if (nostr_nip44_encrypt_v2_with_convkey(ck, file_key, 32, &payload) != 0 || !payload ||
      strlen(payload) != NIP44_PAYLOAD_B64_LEN) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "NIP-44 wrap failed");
    goto out;
  }
  uint8_t sh[STANZA_HDR_LEN] = { NSEAL_STANZA_NIP44, 0, 0 };
  put_be16(sh + 1, NIP44_STANZA_LEN);
  g_byte_array_append(out, sh, sizeof sh);
  g_byte_array_append(out, recipient, 32);
  g_byte_array_append(out, epk, 32);
  g_byte_array_append(out, (const uint8_t *)payload, NIP44_PAYLOAD_B64_LEN);
  ok = TRUE;
out:
  OPENSSL_cleanse(esk, sizeof esk);
  OPENSSL_cleanse(ck, sizeof ck);
  free(payload);
  return ok;
}

static gboolean wrap_nip49(const char *passphrase, guint8 log_n, const uint8_t file_key[32],
                           GByteArray *out, GError **error) {
  ensure_nip49_normalizer();
  char *ncs = NULL;
  int rc = nostr_nip49_encrypt(file_key, NOSTR_NIP49_SECURITY_SECURE, passphrase, log_n, &ncs);
  if (rc != 0 || !ncs || strlen(ncs) > NIP49_STANZA_MAX) {
    free(ncs);
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "NIP-49 passphrase wrap failed (rc=%d)", rc);
    return FALSE;
  }
  gsize len = strlen(ncs);
  uint8_t sh[STANZA_HDR_LEN] = { NSEAL_STANZA_NIP49, 0, 0 };
  put_be16(sh + 1, (guint16)len);
  g_byte_array_append(out, sh, sizeof sh);
  g_byte_array_append(out, (const uint8_t *)ncs, (guint)len);
  free(ncs);
  return TRUE;
}

gboolean nseal_unwrap_with_seckey(gpointer seckey32, const uint8_t ephemeral_pk[32],
                                  const char *payload, uint8_t out_file_key[32],
                                  GError **error) {
  uint8_t ck[32];
  if (nostr_nip44_convkey((const uint8_t *)seckey32, ephemeral_pk, ck) != 0) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_UNWRAP, "conversation key derivation failed");
    return FALSE;
  }
  gboolean ok = nseal_unwrap_with_convkey(ck, payload, out_file_key, error);
  OPENSSL_cleanse(ck, sizeof ck);
  return ok;
}

gboolean nseal_unwrap_with_convkey(const uint8_t convkey[32], const char *payload,
                                   uint8_t out_file_key[32], GError **error) {
  uint8_t *pt = NULL; size_t pt_len = 0;
  if (nostr_nip44_decrypt_v2_with_convkey(convkey, payload, &pt, &pt_len) != 0 || !pt ||
      pt_len != NSEAL_FILE_KEY_LEN) {
    if (pt) { OPENSSL_cleanse(pt, pt_len); free(pt); }
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_UNWRAP,
                        "key stanza did not open with this identity");
    return FALSE;
  }
  memcpy(out_file_key, pt, 32);
  OPENSSL_cleanse(pt, pt_len);
  free(pt);
  return TRUE;
}

/* ─── Encrypt ────────────────────────────────────────────────────────── */

gboolean nseal_encrypt_fd(int in_fd, int out_fd, const NsealEncryptOptions *opts, GError **error) {
  g_return_val_if_fail(opts != NULL, FALSE);
  const guint chunk_log2 = opts->chunk_log2 ? opts->chunk_log2 : NSEAL_CHUNK_LOG2_DEFAULT;
  const guint8 log_n = opts->log_n ? opts->log_n : NSEAL_LOG_N_DEFAULT;
  const gboolean pass = opts->passphrase != NULL;

  if (chunk_log2 < NSEAL_CHUNK_LOG2_MIN || chunk_log2 > NSEAL_CHUNK_LOG2_MAX) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "chunk size 2^%u out of range [2^%d, 2^%d]",
                chunk_log2, NSEAL_CHUNK_LOG2_MIN, NSEAL_CHUNK_LOG2_MAX);
    return FALSE;
  }
  if (pass == (opts->n_recipients > 0)) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                        pass ? "a passphrase file cannot also have recipients"
                             : "no recipients and no passphrase");
    return FALSE;
  }
  if (pass && (log_n < NSEAL_LOG_N_MIN || log_n > NSEAL_LOG_N_MAX)) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "scrypt work factor %u out of range [%d, %d]",
                log_n, NSEAL_LOG_N_MIN, NSEAL_LOG_N_MAX);
    return FALSE;
  }
  if (pass && !g_utf8_validate(opts->passphrase, -1, NULL)) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "passphrase is not valid UTF-8");
    return FALSE;
  }
  if (opts->n_recipients > NSEAL_MAX_STANZAS) {
    g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "at most %d recipients", NSEAL_MAX_STANZAS);
    return FALSE;
  }
  for (gsize i = 0; i < opts->n_recipients; i++) {
    if (!pubkey_on_curve(opts->recipients[i])) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "recipient %" G_GSIZE_FORMAT " is not a secp256k1 key", i + 1);
      return FALSE;
    }
    for (gsize j = 0; j < i; j++)
      if (memcmp(opts->recipients[i], opts->recipients[j], 32) == 0) {
        g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG, "recipient %" G_GSIZE_FORMAT " is listed twice", i + 1);
        return FALSE;
      }
  }

  gboolean ok = FALSE;
  uint8_t file_key[32], root[32];
  uint8_t *buf = NULL;
  GByteArray *hdr = g_byte_array_new();
  GByteArray *addrs = g_byte_array_new();
  uint8_t *sealed_idx = NULL; size_t sealed_idx_len = 0;
  uint8_t *idx = NULL; gsize idx_len = 0;

  uint8_t commit[32];
  if (!random_bytes(file_key, sizeof file_key, error)) goto out;
  if (!key_schedule(file_key, root, commit, error)) goto out;

  /* Header */
  uint8_t fixed[HDR_FIXED_LEN];
  memcpy(fixed, NSEAL_MAGIC, 7);
  fixed[7] = NSEAL_FORMAT_VERSION;
  fixed[8] = 0;
  fixed[9] = (uint8_t)chunk_log2;
  put_be16(fixed + 10, (guint16)(pass ? 1 : opts->n_recipients));
  memcpy(fixed + HDR_COMMIT_OFF, commit, 32);
  g_byte_array_append(hdr, fixed, sizeof fixed);
  if (pass) {
    if (!wrap_nip49(opts->passphrase, log_n, file_key, hdr, error)) goto out;
  } else {
    for (gsize i = 0; i < opts->n_recipients; i++)
      if (!wrap_nip44(opts->recipients[i], file_key, hdr, error)) goto out;
  }
  uint8_t hdr_sha[32];
  if (nh_porthome_sha256(hdr->data, hdr->len, hdr_sha) != NH_PORTHOME_OK) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "hash failed");
    goto out;
  }
  if (!nseal_write_all(out_fd, hdr->data, hdr->len, error)) goto out;

  /* Body: sealed chunks, pt = index(8) || data */
  const gsize chunk = (gsize)1 << chunk_log2;
  buf = g_malloc(CHUNK_INDEX_LEN + chunk);
  guint64 index = 0, total = 0;
  for (;;) {
    gssize n = read_full(in_fd, buf + CHUNK_INDEX_LEN, chunk, error);
    if (n < 0) goto out;
    if (n == 0) break;
    if (index >= NSEAL_MAX_CHUNKS) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                  "input exceeds %" G_GUINT64_FORMAT " chunks of 2^%u bytes; use a larger --chunk-size",
                  NSEAL_MAX_CHUNKS, chunk_log2);
      goto out;
    }
    put_be64(buf, index);
    uint8_t *ct = NULL; size_t ct_len = 0; uint8_t addr[32];
    if (nh_porthome_encrypt_chunk(root, buf, CHUNK_INDEX_LEN + (gsize)n, &ct, &ct_len, addr) != NH_PORTHOME_OK) {
      g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "chunk seal failed");
      goto out;
    }
    gboolean wok = nseal_write_all(out_fd, ct, ct_len, error);
    free(ct);
    if (!wok) goto out;
    g_byte_array_append(addrs, addr, sizeof addr);
    total += (guint64)n;
    index++;
    if ((gsize)n < chunk) break;
  }
  OPENSSL_cleanse(buf, CHUNK_INDEX_LEN + chunk);

  /* Index (sealed as a porthome manifest) + footer */
  idx_len = IDX_FIXED_LEN + addrs->len;
  idx = g_malloc(idx_len);
  memcpy(idx, IDX_MAGIC, 8);
  idx[8] = IDX_VERSION;
  idx[9] = (uint8_t)chunk_log2;
  idx[10] = idx[11] = 0;
  memcpy(idx + 12, hdr_sha, 32);
  put_be64(idx + 44, total);
  put_be64(idx + 52, index);
  if (addrs->len) memcpy(idx + IDX_FIXED_LEN, addrs->data, addrs->len);
  if (nh_porthome_encrypt_manifest(root, idx, idx_len, &sealed_idx, &sealed_idx_len) != NH_PORTHOME_OK) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "index seal failed");
    goto out;
  }
  uint8_t footer[FOOTER_LEN];
  put_be64(footer, (guint64)sealed_idx_len);
  if (nh_porthome_sha256(sealed_idx, sealed_idx_len, footer + 8) != NH_PORTHOME_OK) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "hash failed");
    goto out;
  }
  memcpy(footer + 40, FOOTER_MAGIC, 8);
  if (!nseal_write_all(out_fd, sealed_idx, sealed_idx_len, error)) goto out;
  if (!nseal_write_all(out_fd, footer, sizeof footer, error)) goto out;
  ok = TRUE;

out:
  OPENSSL_cleanse(file_key, sizeof file_key);
  OPENSSL_cleanse(root, sizeof root);
  g_free(buf);
  g_free(idx);
  free(sealed_idx);
  g_byte_array_unref(hdr);
  g_byte_array_unref(addrs);
  return ok;
}

/* ─── Decrypt ────────────────────────────────────────────────────────── */

static gboolean recover_file_key(const NsealHeader *h, const NsealDecryptOptions *opts,
                                 uint8_t file_key[32], GError **error) {
  if (nseal_header_is_passphrase(h)) {
    NsealStanza *s = g_ptr_array_index(h->stanzas, 0);
    if (!opts->passphrase) {
      g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_NOT_A_RECIPIENT,
                          "this file is sealed with a passphrase");
      return FALSE;
    }
    guint8 log_n = 0, max = opts->max_log_n ? opts->max_log_n : NSEAL_LOG_N_MAX;
    if (!ncryptsec_log_n(s->text, &log_n) || log_n > max || log_n < 10) {
      g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_UNSUPPORTED,
                  "passphrase work factor 2^%u exceeds the limit 2^%u", log_n, max);
      return FALSE;
    }
    ensure_nip49_normalizer();
    if (nostr_nip49_decrypt(s->text, opts->passphrase, file_key, NULL, NULL) != 0) {
      OPENSSL_cleanse(file_key, 32);
      g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_UNWRAP, "wrong passphrase");
      return FALSE;
    }
    return TRUE;
  }

  if (!opts->identity_pubkey || !opts->unwrap) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_NOT_A_RECIPIENT,
                        "this file is sealed for Nostr public keys; no identity offered");
    return FALSE;
  }
  for (guint i = 0; i < h->stanzas->len; i++) {
    NsealStanza *s = g_ptr_array_index(h->stanzas, i);
    if (s->type != NSEAL_STANZA_NIP44 || memcmp(s->recipient, opts->identity_pubkey, 32) != 0)
      continue;
    /* Recipients are unique (checked at parse), so this is the one. */
    return opts->unwrap(opts->unwrap_data, s->ephemeral, s->text, file_key, error);
  }
  g_autofree char *npub = nseal_pubkey_to_npub(opts->identity_pubkey);
  g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_NOT_A_RECIPIENT,
              "%s is not a recipient of this file", npub ? npub : "this identity");
  return FALSE;
}

static gboolean integrity_fail(GError **error, const char *what) {
  g_set_error(error, NSEAL_ERROR, NSEAL_ERROR_INTEGRITY, "sealed file failed verification: %s", what);
  return FALSE;
}

gboolean nseal_decrypt_fd(int in_fd, int out_fd, const NsealDecryptOptions *opts, GError **error) {
  g_return_val_if_fail(opts != NULL, FALSE);
  gboolean ok = FALSE;
  uint8_t file_key[32] = {0}, root[32] = {0};
  uint8_t *sealed_idx = NULL, *idx = NULL, *cbuf = NULL;
  size_t idx_len = 0;

  g_autoptr(NsealHeader) h = nseal_header_read_fd(in_fd, error);
  if (!h) return FALSE;
  const guint64 hdr_len = g_bytes_get_size(h->raw);

  struct stat st;
  if (fstat(in_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_ARG,
                        "decrypt needs a regular (seekable) input file");
    return FALSE;
  }
  const guint64 size = (guint64)st.st_size;
  if (size < hdr_len + FOOTER_LEN) return integrity_fail(error, "truncated (no index)");

  if (!recover_file_key(h, opts, file_key, error)) goto out;
  uint8_t commit[32];
  if (!key_schedule(file_key, root, commit, error)) goto out;
  /* Every recipient must land on the same file_key: a sender cannot hand
   * different recipients different keys (and so different plaintexts). */
  if (CRYPTO_memcmp(commit, h->commit, 32) != 0) {
    integrity_fail(error, "key commitment mismatch");
    goto out;
  }

  /* Footer → sealed index; address verified before decrypt. */
  uint8_t footer[FOOTER_LEN];
  if (!pread_exact(in_fd, footer, sizeof footer, size - FOOTER_LEN, error)) goto out;
  if (memcmp(footer + 40, FOOTER_MAGIC, 8) != 0) { integrity_fail(error, "truncated (bad trailer)"); goto out; }
  const guint64 sidx_len = get_be64(footer);
  const guint64 sidx_max = NH_PORTHOME_SEAL_OVERHEAD + IDX_FIXED_LEN + 32 * NSEAL_MAX_CHUNKS;
  if (sidx_len < NH_PORTHOME_SEAL_OVERHEAD + IDX_FIXED_LEN || sidx_len > sidx_max ||
      sidx_len > size - hdr_len - FOOTER_LEN) {
    integrity_fail(error, "index length out of range");
    goto out;
  }
  const guint64 sidx_off = size - FOOTER_LEN - sidx_len;
  sealed_idx = g_malloc((gsize)sidx_len);
  if (!pread_exact(in_fd, sealed_idx, (gsize)sidx_len, sidx_off, error)) goto out;
  uint8_t addr[32];
  if (nh_porthome_sha256(sealed_idx, (size_t)sidx_len, addr) != NH_PORTHOME_OK ||
      CRYPTO_memcmp(addr, footer + 8, 32) != 0) {
    integrity_fail(error, "index address mismatch");
    goto out;
  }
  if (nh_porthome_decrypt_manifest(root, sealed_idx, (size_t)sidx_len, &idx, &idx_len) != NH_PORTHOME_OK) {
    integrity_fail(error, "index did not authenticate");
    goto out;
  }

  /* Strict index parse; binds header bytes and file geometry. */
  uint8_t hdr_sha[32];
  if (nh_porthome_sha256(g_bytes_get_data(h->raw, NULL), (size_t)hdr_len, hdr_sha) != NH_PORTHOME_OK) {
    g_set_error_literal(error, NSEAL_ERROR, NSEAL_ERROR_CRYPTO, "hash failed");
    goto out;
  }
  if (idx_len < IDX_FIXED_LEN || memcmp(idx, IDX_MAGIC, 8) != 0 ||
      idx[8] != IDX_VERSION || idx[10] != 0 || idx[11] != 0) {
    integrity_fail(error, "bad index");
    goto out;
  }
  if (CRYPTO_memcmp(idx + 12, hdr_sha, 32) != 0) { integrity_fail(error, "header was modified"); goto out; }
  const guint chunk_log2 = idx[9];
  const guint64 total = get_be64(idx + 44);
  const guint64 n_chunks = get_be64(idx + 52);
  if (chunk_log2 != h->chunk_log2) { integrity_fail(error, "chunk size mismatch"); goto out; }
  const guint64 chunk = (guint64)1 << chunk_log2;
  /* ceil without the (total + chunk - 1) wrap */
  const guint64 want_chunks = total / chunk + (total % chunk != 0);
  if (n_chunks > NSEAL_MAX_CHUNKS || n_chunks != want_chunks ||
      (guint64)idx_len != IDX_FIXED_LEN + 32 * n_chunks) {
    integrity_fail(error, "index geometry is inconsistent");
    goto out;
  }
  const guint64 body_len = total + n_chunks * (CHUNK_INDEX_LEN + NH_PORTHOME_SEAL_OVERHEAD);
  if (hdr_len + body_len != sidx_off) { integrity_fail(error, "body length mismatch"); goto out; }

  /* Stream chunks: address check → open → index check → write. */
  cbuf = g_malloc((gsize)(CHUNK_INDEX_LEN + chunk + NH_PORTHOME_SEAL_OVERHEAD));
  guint64 off = hdr_len, remaining = total;
  for (guint64 i = 0; i < n_chunks; i++) {
    const guint64 dlen = remaining < chunk ? remaining : chunk;
    const gsize clen = (gsize)(CHUNK_INDEX_LEN + dlen + NH_PORTHOME_SEAL_OVERHEAD);
    if (!pread_exact(in_fd, cbuf, clen, off, error)) goto out;
    if (nh_porthome_sha256(cbuf, clen, addr) != NH_PORTHOME_OK ||
        CRYPTO_memcmp(addr, idx + IDX_FIXED_LEN + 32 * i, 32) != 0) {
      g_autofree char *m = g_strdup_printf("chunk %" G_GUINT64_FORMAT " address mismatch", i);
      integrity_fail(error, m);
      goto out;
    }
    uint8_t *pt = NULL; size_t pt_len = 0;
    if (nh_porthome_decrypt_chunk(root, cbuf, clen, &pt, &pt_len) != NH_PORTHOME_OK ||
        pt_len != CHUNK_INDEX_LEN + dlen || get_be64(pt) != i) {
      if (pt) { OPENSSL_cleanse(pt, pt_len); free(pt); }
      g_autofree char *m = g_strdup_printf("chunk %" G_GUINT64_FORMAT " did not authenticate", i);
      integrity_fail(error, m);
      goto out;
    }
    gboolean wok = nseal_write_all(out_fd, pt + CHUNK_INDEX_LEN, (gsize)dlen, error);
    OPENSSL_cleanse(pt, pt_len);
    free(pt);
    if (!wok) goto out;
    off += clen;
    remaining -= dlen;
  }
  ok = TRUE;

out:
  OPENSSL_cleanse(file_key, sizeof file_key);
  OPENSSL_cleanse(root, sizeof root);
  g_free(sealed_idx);
  if (idx) { free(idx); }
  g_free(cbuf);
  return ok;
}
