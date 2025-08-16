#include <nostr/crypto/bip39.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <ctype.h>
#ifdef NOSTR_HAVE_GLIB
#include <glib.h>
#endif
#include <string.h>
#include <stdlib.h>

/* Embedded English wordlist blob from bip39_wordlist_en.c */
extern const char nostr_bip39_en_blob[];
extern const size_t nostr_bip39_en_blob_len;

/* Parsed word pointers (2048) initialized on first use */
static const char *g_words[2048];
static int g_words_init = 0;
/* Forward decls */
static int allowed_word_count(int n);

static void bip39_init_words(void) {
  if (g_words_init) return;
  size_t i = 0;
  const char *p = nostr_bip39_en_blob;
  const char *end = nostr_bip39_en_blob + nostr_bip39_en_blob_len;
  while (p < end && i < 2048) {
    g_words[i++] = p;
    const char *nl = (const char*)memchr(p, '\n', (size_t)(end - p));
    if (!nl) break;
    p = nl + 1;
  }
  g_words_init = 1;
}

/* Generate a BIP-39 English mnemonic with checksum. Caller must free(). */
char *nostr_bip39_generate(int word_count) {
  if (!allowed_word_count(word_count)) return NULL;
  bip39_init_words();

  const int ENT_bits = (word_count == 12 ? 128 : word_count == 15 ? 160 : word_count == 18 ? 192 : word_count == 21 ? 224 : 256);
  const int CS_bits = ENT_bits / 32;
  const int ENT_bytes = ENT_bits / 8;
  unsigned char entropy[32] = {0};
  if (RAND_bytes(entropy, ENT_bytes) != 1) return NULL;

  /* Compute checksum */
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(entropy, (size_t)ENT_bytes, hash);

  /* Build bitstream ENT||CS */
  unsigned char bits[33] = {0};
  memcpy(bits, entropy, (size_t)ENT_bytes);
  for (int b = 0; b < CS_bits; ++b) {
    int cs_bit = (hash[0] >> (7 - b)) & 1;
    int bitpos = ENT_bits + b;
    int byte = bitpos / 8;
    int off = 7 - (bitpos % 8);
    bits[byte] |= (unsigned char)(cs_bit << off);
  }

  /* Extract 11-bit word indices */
  int idx[24] = {0};
  int bitpos = 0;
  for (int i = 0; i < word_count; ++i) {
    int v = 0;
    for (int b = 0; b < 11; ++b) {
      int byte = (bitpos + b) / 8;
      int off = 7 - ((bitpos + b) % 8);
      int bit = (bits[byte] >> off) & 1;
      v = (v << 1) | bit;
    }
    idx[i] = v;
    bitpos += 11;
  }

  /* Build space-separated mnemonic string */
  size_t total_len = 0;
  for (int i = 0; i < word_count; ++i) {
    const char *w = g_words[idx[i]];
    size_t wl = 0; while (w[wl] && w[wl] != '\n' && w[wl] != '\r') wl++;
    total_len += wl;
  }
  total_len += (size_t)(word_count - 1) + 1; /* spaces + NUL */
  char *out = (char*)malloc(total_len);
  if (!out) return NULL;
  size_t p = 0;
  for (int i = 0; i < word_count; ++i) {
    const char *w = g_words[idx[i]];
    size_t wl = 0; while (w[wl] && w[wl] != '\n' && w[wl] != '\r') wl++;
    memcpy(out + p, w, wl);
    p += wl;
    if (i + 1 < word_count) out[p++] = ' ';
  }
  out[p] = '\0';
  return out;
}

static int word_index(const char *w, size_t wl) {
  /* Binary search over 2048 sorted words */
  int lo = 0, hi = 2047;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const char *mw = g_words[mid];
    size_t ml = 0; while (mw[ml] && mw[ml] != '\n' && mw[ml] != '\r') ml++;
    int c = (int)memcmp(w, mw, wl < ml ? wl : ml);
    if (c == 0) {
      if (wl == ml) return mid;
      c = (wl < ml) ? -1 : 1;
    }
    if (c < 0) hi = mid - 1; else lo = mid + 1;
  }
  return -1;
}

static int allowed_word_count(int n) {
  return (n == 12 || n == 15 || n == 18 || n == 21 || n == 24);
}

#ifdef NOSTR_HAVE_GLIB
/* NFKD normalization using GLib. Returns newly allocated char* (UTF-8), caller frees with g_free(). */
static char *nfkd_dup(const char *s) {
  if (!s) return NULL;
  /* Fast-path ASCII */
  const unsigned char *p = (const unsigned char*)s;
  while (*p) {
    if (*p & 0x80) break;
    p++;
  }
  if (*p == 0) {
    /* ASCII: just duplicate as UTF-8 (also lower case expected by BIP-39 english set) */
    return g_strdup(s);
  }
  /* Normalize to NFKD */
  char *norm = g_utf8_normalize(s, -1, G_NORMALIZE_NFKD);
  return norm ? norm : g_strdup(s);
}
static inline void nfkd_free(char *p) { if (p) g_free(p); }
#else
/* Fallback: ASCII-only duplication (no normalization). */
static char *nfkd_dup(const char *s) {
  size_t n = strlen(s);
  char *dup = (char*)malloc(n + 1);
  if (!dup) return NULL;
  memcpy(dup, s, n + 1);
  return dup;
}
static inline void nfkd_free(char *p) { if (p) free(p); }
#endif

bool nostr_bip39_validate(const char *mnemonic) {
  if (!mnemonic) return false;
  bip39_init_words();

  /* Tokenize into up to 24 words */
  const char *s = mnemonic;
  int idx[24];
  int wc = 0;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    const char *start = s;
    while (*s && *s != ' ') {
      if (!(*s >= 'a' && *s <= 'z')) return false; /* enforce a-z */
      s++;
    }
    size_t wl = (size_t)(s - start);
    if (wl == 0 || wl > 8) return false; /* max len ~8 for english list */
    if (wc >= 24) return false;
    int wi = word_index(start, wl);
    if (wi < 0) {
      fprintf(stderr, "bip39: unknown word: %.*s\n", (int)wl, start);
      return false;
    }
    idx[wc++] = wi;
    while (*s == ' ') s++;
  }
  if (!allowed_word_count(wc)) return false;

  /* Rebuild bitstream from indices (11 bits each) */
  const int ENT_bits = (wc == 12 ? 128 : wc == 15 ? 160 : wc == 18 ? 192 : wc == 21 ? 224 : 256);
  const int CS_bits = ENT_bits / 32;
  unsigned char bits[33] = {0}; /* up to 264 bits */
  int bitpos = 0;
  for (int i = 0; i < wc; ++i) {
    int v = idx[i];
    for (int b = 10; b >= 0; --b) {
      int bit = (v >> b) & 1;
      int byte = bitpos / 8;
      int off = 7 - (bitpos % 8);
      bits[byte] |= (unsigned char)(bit << off);
      bitpos++;
    }
  }
  /* Extract entropy bytes */
  int ENT_bytes = ENT_bits / 8;
  unsigned char entropy[32];
  memcpy(entropy, bits, (size_t)ENT_bytes);

  /* Compute checksum and compare first CS_bits */
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(entropy, (size_t)ENT_bytes, hash);
  int ok = 1;
  for (int b = 0; b < CS_bits; ++b) {
    int want = (hash[0] >> (7 - b)) & 1;
    int have = (bits[ENT_bytes] >> (7 - b)) & 1;
    if (want != have) { ok = 0; break; }
  }
  if (!ok) fprintf(stderr, "bip39: checksum mismatch (wc=%d)\n", wc);
  return ok ? true : false;
}

/* Seed = PBKDF2-HMAC-SHA512(mnemonic, "mnemonic" + passphrase, 2048, 64) */
bool nostr_bip39_seed(const char *mnemonic, const char *passphrase, uint8_t out[64]) {
  if (!mnemonic || !out) return false;
  const char *pp = passphrase ? passphrase : "";
  /* Apply NFKD normalization per BIP-39 */
  char *mn_norm = nfkd_dup(mnemonic);
  char *pf_norm = nfkd_dup(pp);
  char salt[9 + 256];
  memcpy(salt, "mnemonic", 8);
  salt[8] = '\0';
  strncat(salt, pf_norm, sizeof(salt) - 9);
  if (PKCS5_PBKDF2_HMAC(mn_norm, (int)strlen(mn_norm),
                         (const unsigned char*)salt, (int)strlen(salt),
                         2048, EVP_sha512(), 64, out) != 1) {
    nfkd_free(mn_norm);
    nfkd_free(pf_norm);
    return false;
  }
  nfkd_free(mn_norm);
  nfkd_free(pf_norm);
  return true;
}
