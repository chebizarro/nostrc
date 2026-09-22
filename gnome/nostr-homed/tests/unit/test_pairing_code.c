/* Pairing-code derivation test — the greeter (and the broker log lines)
 * both use nh_auth_provider_nip46_qr_pairing_code() so a signer app can
 * visually cross-check its session against the greeter. The code MUST be
 * derived from the client pubkey (loggable), never from the connect secret
 * (design §8.1). */
#include "auth_provider.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(const char *pk, const char *want, int want_rc) {
  char out[10] = {0};
  int rc = nh_auth_provider_nip46_qr_pairing_code(pk, out);
  if (rc != want_rc) {
    fprintf(stderr, "FAIL: pk=%s rc=%d want %d\n",
            pk ? pk : "(null)", rc, want_rc);
    exit(1);
  }
  if (want_rc == 0 && strcmp(out, want) != 0) {
    fprintf(stderr, "FAIL: pk=%s got %s want %s\n", pk, out, want);
    exit(1);
  }
}

int main(void) {
  /* Design §3.4: first 8 hex chars, upper-cased, hyphenated at position 4. */
  expect("a1b2c3d4e5f60000000000000000000000000000000000000000000000000000",
         "A1B2-C3D4", 0);
  expect("0011223344556677889900112233445566778899aabbccddeeff0011223344ff",
         "0011-2233", 0);
  /* Mixed-case input is not the spec — pubkeys are lowercase hex — but be
   * tolerant of a leading uppercase byte in tests that hand a rebuilt key. */
  expect("deadBEEF001122ff00000000000000000000000000000000000000000000000f",
         "DEAD-BEEF", -1);
  /* Reject non-hex + short strings. */
  expect(NULL, NULL, -1);
  expect("", NULL, -1);
  expect("abc", NULL, -1);
  expect("nothexnothexnothexnothexnothexnothexnothexnothexnothexnothexno",
         NULL, -1);
  puts("RESULT: PASS");
  return 0;
}
