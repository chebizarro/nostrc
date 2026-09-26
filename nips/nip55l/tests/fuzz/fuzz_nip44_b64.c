/* fuzz_nip55l_nip44_b64 — nostr_nip55l_nip44_{encrypt,decrypt}_b64.
 *
 * The binary NIP-44 lane exists so payloads of raw bytes can ride D-Bus's
 * UTF-8-only string transport. The base64 decoder is strict and canonical,
 * and the ciphertext is an ordinary NIP-44 v2 payload — so a mangled input
 * must fail the call, not silently truncate the plaintext or overrun a
 * buffer.
 *
 * The first byte of the fuzz input picks the lane:
 *   0: encrypt round — take the tail as raw bytes, base64-encode, feed to
 *      encrypt_b64, decrypt_b64 back, and check exact round-trip.
 *   1: decrypt round — take the tail as a candidate ciphertext string and
 *      feed to decrypt_b64. Most inputs fail parse; a lucky one succeeds
 *      and the harness just frees the plaintext.
 *   2: malformed base64 plaintext — feed the tail (as a string, high bytes
 *      allowed) to encrypt_b64. The decoder must refuse anything that is
 *      not exactly canonical base64 with a NUL-terminated string.
 *
 * Issue: nostrc-tf3b (deferred from nostrc-p7f6 / nostrc-3m86).
 */

#include "nostr/nip55l/signer_ops.h"

#include <nostr-keys.h>
#include <nostr-utils.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern int nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);

static char *g_a_sk = NULL;   /* sender secret hex */
static char *g_b_pk = NULL;   /* recipient x-only pub hex */
static char *g_a_pk = NULL;   /* sender x-only pub hex (for reverse decrypt) */
static char *g_b_sk = NULL;   /* recipient secret hex (for reverse decrypt) */

static void ensure_keys(void) {
  if (g_a_sk) return;
  g_a_sk = nostr_key_generate_private();
  g_b_sk = nostr_key_generate_private();
  if (!g_a_sk || !g_b_sk) abort();
  g_a_pk = nostr_key_get_public(g_a_sk);
  g_b_pk = nostr_key_get_public(g_b_sk);
  if (!g_a_pk || !g_b_pk) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ensure_keys();
  if (size < 1) return 0;
  if (size > (1u << 20)) return 0;

  uint8_t mode = data[0] % 3;
  const uint8_t *tail = data + 1;
  size_t tail_len = size - 1;

  switch (mode) {
  case 0: {
    /* Encrypt the raw tail bytes, decrypt back, check equality. */
    char *pt_b64 = NULL;
    if (nip44_base64_encode(tail, tail_len, &pt_b64) != 0 || !pt_b64) return 0;

    char *ct = NULL;
    int rc = nostr_nip55l_nip44_encrypt_b64(pt_b64, g_b_pk, g_a_sk, &ct);
    if (rc != 0) {
      if (ct) abort(); /* API contract on failure */
      free(pt_b64);
      return 0;
    }
    if (!ct) abort();

    char *back_b64 = NULL;
    rc = nostr_nip55l_nip44_decrypt_b64(ct, g_a_pk, g_b_sk, &back_b64);
    free(ct);
    if (rc != 0 || !back_b64) {
      free(pt_b64);
      abort(); /* a payload we just produced must decrypt */
    }
    if (strcmp(back_b64, pt_b64) != 0) {
      /* Bytes must round-trip exactly, including embedded NULs and high
       * bytes. Anything else is a lane bug. */
      abort();
    }
    free(back_b64);
    free(pt_b64);
    return 0;
  }

  case 1: {
    /* Feed the tail as a candidate NIP-44 v2 payload string. Almost
     * everything fails parse; the harness only needs to keep the API
     * contract (*out == NULL on failure). */
    char *ct = (char *)malloc(tail_len + 1);
    if (!ct) return 0;
    memcpy(ct, tail, tail_len);
    ct[tail_len] = '\0';
    char *out = NULL;
    int rc = nostr_nip55l_nip44_decrypt_b64(ct, g_a_pk, g_b_sk, &out);
    if (rc == 0) {
      if (!out) abort();
      free(out);
    } else if (out) {
      abort();
    }
    free(ct);
    return 0;
  }

  case 2: {
    /* Feed the tail as a candidate base64 plaintext. The decoder must
     * refuse anything that is not canonical, contract-tested; the fuzzer
     * checks it also does not crash on any input string. */
    char *b64 = (char *)malloc(tail_len + 1);
    if (!b64) return 0;
    memcpy(b64, tail, tail_len);
    b64[tail_len] = '\0';
    char *ct = NULL;
    int rc = nostr_nip55l_nip44_encrypt_b64(b64, g_b_pk, g_a_sk, &ct);
    if (rc == 0) {
      if (!ct) abort();
      free(ct);
    } else if (ct) {
      abort();
    }
    free(b64);
    return 0;
  }
  }
  return 0;
}
