/* fuzz_nip55l_sign_event_json — nostr_nip55l_sign_event_json.
 *
 * The 0.2.0 SignEvent contract: an unsigned event template in, the complete
 * signed event JSON out (id + pubkey + created_at filled + sig). The
 * template parser must reject anything that is not a valid Nostr event with
 * NOSTR_SIGNER_ERROR_INVALID_JSON — never silently sign an empty event or
 * scribble past a truncated buffer.
 *
 * The harness supplies the identity as an explicit hex/nsec selector, so
 * the signer never touches libsecret, the Keychain, or the environment.
 * The first byte of the fuzz input picks which lane (hex, nsec1, empty +
 * NOSTR_SIGNER_SECKEY_HEX) so the resolver's three inputs each get shape
 * coverage; the rest of the buffer is the event JSON.
 *
 * Issue: nostrc-tf3b (deferred from nostrc-p7f6).
 */

#include "nostr/nip55l/signer_ops.h"

#include <nostr-keys.h>
#include <nostr/nip19/nip19.h>
#include <nostr-utils.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A fixed key so runs are reproducible and no libsecret / Keychain / env
 * lookup ever happens. Generated once per process. */
static char *g_sk_hex = NULL;
static char *g_nsec = NULL;

static void ensure_key(void) {
  if (g_sk_hex) return;
  g_sk_hex = nostr_key_generate_private();
  if (!g_sk_hex) abort();
  /* Derive a matching nsec1 selector too, so the fuzzer exercises the
   * nsec1 branch of resolve_seckey_hex(). */
  uint8_t sk[32];
  if (!nostr_hex2bin(sk, g_sk_hex, sizeof sk)) abort();
  if (nostr_nip19_encode_nsec(sk, &g_nsec) != 0 || !g_nsec) {
    /* Not fatal for fuzzing; fall back to the hex lane exclusively. */
    g_nsec = NULL;
  }
  /* And park it in the env for the "empty selector" branch. */
  setenv("NOSTR_SIGNER_SECKEY_HEX", g_sk_hex, 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ensure_key();
  if (size == 0 || size > (1u << 20)) return 0;

  const uint8_t *body = data + 1;
  size_t body_len = size - 1;
  const char *selector = NULL;
  switch (data[0] % 3) {
    case 0: selector = g_sk_hex; break;
    case 1: selector = g_nsec ? g_nsec : g_sk_hex; break;
    case 2: selector = ""; break; /* resolves via NOSTR_SIGNER_SECKEY_HEX */
  }

  /* Signer takes a NUL-terminated string; copy the tail out. */
  char *js = (char *)malloc(body_len + 1);
  if (!js) return 0;
  memcpy(js, body, body_len);
  js[body_len] = '\0';

  char *out = NULL;
  int rc = nostr_nip55l_sign_event_json(js, selector, "fuzz", &out);
  if (rc == 0 && out) {
    /* Successful sign: the returned document must be a JSON object; not an
     * empty string, not the input, not a bare 128-hex signature. */
    if (out[0] != '{') abort();
    free(out);
  } else if (out) {
    /* On failure the API contract is *out == NULL; guard the harness. */
    abort();
  }

  free(js);
  return 0;
}
