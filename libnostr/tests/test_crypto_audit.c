/* Crypto hardening regression tests (nostrc-2kf).
 *
 * 1. BIP-32 derivation still produces the correct child private key against a
 *    well-documented BIP-32 test vector. This proves the zeroization changes in
 *    bip32.c (OPENSSL_cleanse on all return paths, BN_clear_free for secret
 *    scalars) do NOT alter derivation results.
 *
 * 2. BIP-39 wordlist lazy-init is race-free: many threads call
 *    nostr_bip39_validate()/nostr_bip39_generate() concurrently. Before the
 *    pthread_once fix, a partially-populated g_words table could be observed,
 *    causing wrong validation or a crash.
 */
#include <nostr/crypto/bip32.h>
#include <nostr/crypto/bip39.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parse a hex string into bytes. Returns number of bytes written, -1 on error. */
static int hex2bin(const char *hex, uint8_t *out, size_t out_cap) {
  size_t hl = strlen(hex);
  if (hl % 2 != 0 || hl / 2 > out_cap) return -1;
  for (size_t i = 0; i < hl / 2; ++i) {
    int hi = hexval(hex[2 * i]);
    int lo = hexval(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return (int)(hl / 2);
}

static int test_bip32_vector(void) {
  /* BIP-32 test vector 1:
   *   seed = 000102030405060708090a0b0c0d0e0f
   *   chain m/0'/1/2'/2/1000000000
   *   -> private key 471b76e389e528d6de6d816857e012c5455051cad6660850e58372a6c3e6e7c8
   */
  uint8_t seed[16];
  int slen = hex2bin("000102030405060708090a0b0c0d0e0f", seed, sizeof(seed));
  if (slen != 16) {
    fprintf(stderr, "bip32: bad seed hex\n");
    return 1;
  }

  const uint32_t path[] = {
    0x80000000u,   /* 0' hardened */
    1u,            /* 1  */
    0x80000002u,   /* 2' hardened */
    2u,            /* 2  */
    1000000000u    /* 1000000000 */
  };

  uint8_t out[32];
  if (!nostr_bip32_priv_from_master_seed(seed, (size_t)slen, path,
                                         sizeof(path) / sizeof(path[0]), out)) {
    fprintf(stderr, "bip32: derivation failed\n");
    return 1;
  }

  uint8_t expected[32];
  if (hex2bin("471b76e389e528d6de6d816857e012c5455051cad6660850e58372a6c3e6e7c8",
              expected, sizeof(expected)) != 32) {
    fprintf(stderr, "bip32: bad expected hex\n");
    return 1;
  }

  if (memcmp(out, expected, 32) != 0) {
    fprintf(stderr, "bip32: derived key mismatch\n  got: ");
    for (int i = 0; i < 32; ++i) fprintf(stderr, "%02x", out[i]);
    fprintf(stderr, "\n want: ");
    for (int i = 0; i < 32; ++i) fprintf(stderr, "%02x", expected[i]);
    fprintf(stderr, "\n");
    return 1;
  }

  /* Also derive the master node (empty path) to exercise the no-loop path. */
  uint8_t master[32];
  if (!nostr_bip32_priv_from_master_seed(seed, (size_t)slen, NULL, 0, master)) {
    fprintf(stderr, "bip32: master derivation failed\n");
    return 1;
  }
  uint8_t master_expected[32];
  if (hex2bin("e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35",
              master_expected, sizeof(master_expected)) != 32) {
    fprintf(stderr, "bip32: bad master expected hex\n");
    return 1;
  }
  if (memcmp(master, master_expected, 32) != 0) {
    fprintf(stderr, "bip32: master key mismatch\n");
    return 1;
  }

  printf("bip32 vector OK\n");
  return 0;
}

/* A valid BIP-39 12-word mnemonic (all-zero entropy). */
static const char *kValidMnemonic =
  "abandon abandon abandon abandon abandon abandon "
  "abandon abandon abandon abandon abandon about";

#define N_THREADS 16
#define ITERS 2000

static volatile int g_race_fail = 0;

static void *worker_validate(void *arg) {
  (void)arg;
  for (int i = 0; i < ITERS; ++i) {
    if (!nostr_bip39_validate(kValidMnemonic)) {
      g_race_fail = 1;
      return NULL;
    }
    /* Occasionally exercise the reject path too (exercises word_index over the
     * lazily-built table). Kept infrequent to avoid flooding stderr with the
     * library's own unknown-word diagnostics. */
    if (i == 0 && nostr_bip39_validate("zzzz zzzz zzzz zzzz")) {
      g_race_fail = 1;
      return NULL;
    }
  }
  return NULL;
}

static void *worker_generate(void *arg) {
  (void)arg;
  for (int i = 0; i < ITERS; ++i) {
    char *m = nostr_bip39_generate(12);
    if (!m) {
      g_race_fail = 1;
      return NULL;
    }
    /* Anything we generate must validate (checksum + wordlist). */
    if (!nostr_bip39_validate(m)) {
      g_race_fail = 1;
      free(m);
      return NULL;
    }
    free(m);
  }
  return NULL;
}

static int test_bip39_threaded(void) {
  pthread_t th[N_THREADS];
  for (int i = 0; i < N_THREADS; ++i) {
    void *(*fn)(void *) = (i % 2 == 0) ? worker_validate : worker_generate;
    if (pthread_create(&th[i], NULL, fn, NULL) != 0) {
      fprintf(stderr, "bip39: pthread_create failed\n");
      return 1;
    }
  }
  for (int i = 0; i < N_THREADS; ++i) {
    pthread_join(th[i], NULL);
  }
  if (g_race_fail) {
    fprintf(stderr, "bip39: concurrent validate/generate failed (race?)\n");
    return 1;
  }
  printf("bip39 threaded OK\n");
  return 0;
}

int main(void) {
  int rc = 0;
  rc |= test_bip32_vector();
  rc |= test_bip39_threaded();
  if (rc == 0) printf("ALL CRYPTO AUDIT TESTS PASSED\n");
  return rc;
}
