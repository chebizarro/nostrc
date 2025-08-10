#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "nostr/nip49/nip49.h"

// Include official vector(s) from the NIP-49 spec document when available.
// Current official test data (from NIP-49):
// Decrypt the following with password='nostr' and log_n=16 should yield the hex key below.
// ncryptsec:
//   ncryptsec1qgg9947rlpvqu76pj5ecreduf9jxhselq2nae2kghhvd5g7dgjtcxfqtd67p9m0w57lspw8gsq6yphnm8623nsl8xn9j4jdzz84zm3frztj3z7s35vpzmqf6ksu8r89qk5z2zxfmu5gv8th8wclt0h4p
// expected privkey hex:
//   3501454135014541350145413501453fefb02227e449e57cf4d3a3ce05378683

static int parse_hex(const char *hex, uint8_t *out, size_t out_len) {
  size_t n = strlen(hex);
  if (n != out_len * 2) return -1;
  for (size_t i = 0; i < out_len; i++) {
    unsigned int b;
    if (sscanf(hex + i*2, "%02x", &b) != 1) return -1;
    out[i] = (uint8_t)b;
  }
  return 0;
}

int main(void) {
  // Official decryption vector
  {
    const char *enc = "ncryptsec1qgg9947rlpvqu76pj5ecreduf9jxhselq2nae2kghhvd5g7dgjtcxfqtd67p9m0w57lspw8gsq6yphnm8623nsl8xn9j4jdzz84zm3frztj3z7s35vpzmqf6ksu8r89qk5z2zxfmu5gv8th8wclt0h4p";
    const char *pw = "nostr";
    const char *hex = "3501454135014541350145413501453fefb02227e449e57cf4d3a3ce05378683";
    uint8_t expected[32];
    assert(parse_hex(hex, expected, sizeof expected) == 0);
    uint8_t out_sk[32]; NostrNip49SecurityByte sec = 0; uint8_t ln = 0;
    int rc = nostr_nip49_decrypt(enc, pw, out_sk, &sec, &ln);
    assert(rc == 0);
    assert(memcmp(out_sk, expected, 32) == 0);
    assert(ln == 16);
    // Security byte is not specified by the vector; don't assert it.
  }

  const char *pw = "nostr"; // placeholder per common examples, ASCII
  uint8_t sk[32]; for (int i=0;i<32;i++) sk[i] = (uint8_t)i;
  char *enc = NULL;
  int rc = nostr_nip49_encrypt(sk, NOSTR_NIP49_SECURITY_SECURE, pw, 16, &enc);
  assert(rc == 0 && enc);
  // HRP must be ncryptsec
  assert(strncmp(enc, "ncryptsec", 9) == 0);

  // Decrypt and compare
  uint8_t out_sk[32]; NostrNip49SecurityByte sec=0; uint8_t ln=0;
  rc = nostr_nip49_decrypt(enc, pw, out_sk, &sec, &ln);
  assert(rc == 0);
  assert(memcmp(sk, out_sk, 32) == 0);
  assert(sec == NOSTR_NIP49_SECURITY_SECURE);
  assert(ln == 16);

  free(enc);
  printf("nip49 vectors placeholder: ok\n");
  return 0;
}
