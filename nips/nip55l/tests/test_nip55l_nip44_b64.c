/* The local signer's binary NIP-44 lane (nostrc-3m86).
 *
 * D-Bus carries strings and requires valid UTF-8, so a payload of raw key
 * material cannot ride NIP44Encrypt/NIP44Decrypt: it is rejected or rewritten
 * to U+FFFD, and the signer then encrypts corrupted bytes with nothing
 * reporting a failure. A Concord CORD-06 rekey blob is 72, 104 or 136 bytes
 * and any other width is dropped as malformed, so the mangling is not a
 * recoverable error — it is a rotation that silently never lands.
 *
 * What these assert: the widths survive exactly, the bytes survive exactly
 * (NULs and non-UTF-8 sequences included), the ciphertext is an ordinary
 * NIP-44 v2 payload that the plain primitive opens — so the lane a sender
 * used never constrains the receiver — and a malformed parameter fails the
 * call instead of decoding to something shorter. */

#include "nostr/nip55l/signer_ops.h"

#include <nostr-keys.h>
#include <nostr/nip44/nip44.h>
#include <nostr-utils.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int nip44_base64_encode(const uint8_t *buf, size_t len, char **out_b64);

/* A blob is key material: high bytes, embedded NULs, and byte sequences that
 * are not valid UTF-8 anywhere in it. */
static void fill_blob(uint8_t *blob, size_t len) {
  for (size_t i = 0; i < len; i++) blob[i] = (uint8_t)((i * 7u + 0x80u) & 0xffu);
  blob[0] = 0x00;
  if (len > 3) {
    blob[1] = 0xff; /* never a valid UTF-8 lead byte */
    blob[2] = 0xfe;
    blob[len / 2] = 0x00;
    blob[len - 1] = 0xc0; /* a lead byte with no continuation */
  }
}

static void roundtrip_width(const char *a_sk, const char *a_pk,
                            const char *b_sk, const char *b_pk, size_t width) {
  uint8_t blob[136];
  assert(width <= sizeof(blob));
  fill_blob(blob, width);

  char *pt_b64 = NULL;
  assert(nip44_base64_encode(blob, width, &pt_b64) == 0 && pt_b64);

  char *payload = NULL;
  int rc = nostr_nip55l_nip44_encrypt_b64(pt_b64, b_pk, a_sk, &payload);
  assert(rc == 0 && payload);
  free(pt_b64);

  /* The recipient recovers the exact bytes, at the exact width. */
  char *out_b64 = NULL;
  rc = nostr_nip55l_nip44_decrypt_b64(payload, a_pk, b_sk, &out_b64);
  assert(rc == 0 && out_b64);

  char *expect_b64 = NULL;
  assert(nip44_base64_encode(blob, width, &expect_b64) == 0);
  assert(strcmp(out_b64, expect_b64) == 0);
  free(out_b64);
  free(expect_b64);

  /* The ciphertext is an ordinary NIP-44 v2 payload: the plain primitive
   * opens it, byte for byte. A recipient never has to know which lane the
   * sender used. */
  uint8_t b_sk_raw[32], a_pk_raw[32];
  assert(nostr_hex2bin(b_sk_raw, b_sk, sizeof b_sk_raw));
  assert(nostr_hex2bin(a_pk_raw, a_pk, sizeof a_pk_raw));
  uint8_t *plain = NULL;
  size_t plain_len = 0;
  assert(nostr_nip44_decrypt_v2(b_sk_raw, a_pk_raw, payload, &plain,
                                &plain_len) == 0);
  assert(plain_len == width);
  assert(memcmp(plain, blob, width) == 0);
  free(plain);
  free(payload);

  printf("  ok: %zu-byte blob round-trips byte-identically\n", width);
}

int main(void) {
  char *a_sk = nostr_key_generate_private();
  char *b_sk = nostr_key_generate_private();
  assert(a_sk && b_sk);
  char *a_pk = nostr_key_get_public(a_sk);
  char *b_pk = nostr_key_get_public(b_sk);
  assert(a_pk && b_pk);

  /* Every CORD-06 rekey blob width: a channel rotation, a member's base
   * rotation, and a staff recipient's. */
  roundtrip_width(a_sk, a_pk, b_sk, b_pk, 72);
  roundtrip_width(a_sk, a_pk, b_sk, b_pk, 104);
  roundtrip_width(a_sk, a_pk, b_sk, b_pk, 136);
  /* A width nobody mints, to prove nothing here is width-aware. */
  roundtrip_width(a_sk, a_pk, b_sk, b_pk, 1);

  /* A malformed plaintext parameter fails the call. A decoder that skipped
   * characters outside the alphabet would turn a typo into a shorter
   * plaintext — a blob of the wrong width, encrypted and delivered. */
  char *payload = NULL;
  assert(nostr_nip55l_nip44_encrypt_b64("AAAA AAAA", b_pk, a_sk, &payload) != 0);
  assert(payload == NULL);
  assert(nostr_nip55l_nip44_encrypt_b64("AAA", b_pk, a_sk, &payload) != 0);
  assert(payload == NULL);
  assert(nostr_nip55l_nip44_encrypt_b64("AA=A", b_pk, a_sk, &payload) != 0);
  assert(payload == NULL);
  printf("  ok: a malformed base64 plaintext is refused, not truncated\n");

  /* An unopenable ciphertext is an error, never an empty plaintext. */
  char *out = NULL;
  assert(nostr_nip55l_nip44_decrypt_b64("not-a-payload", a_pk, b_sk, &out) != 0);
  assert(out == NULL);
  printf("  ok: a malformed payload is refused\n");

  free(a_sk); free(b_sk); free(a_pk); free(b_pk);
  printf("test_nip55l_nip44_b64: PASS\n");
  return 0;
}
