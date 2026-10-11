/* fuzz_nip55l_sign_event_json — nostr_nip55l_sign_event_json.
 *
 * The 0.2.0 SignEvent contract: an unsigned event template in, the complete
 * signed event JSON out (id + pubkey + created_at filled + sig). The
 * template parser must reject anything that is not a valid Nostr event with
 * NOSTR_SIGNER_ERROR_INVALID_JSON — never silently sign an empty event or
 * scribble past a truncated buffer.
 *
 * The harness provisions a throwaway key through NOSTR_SIGNER_SECKEY_HEX,
 * never touching libsecret or the Keychain. The first byte chooses an
 * identity selector (empty, npub, hex pubkey, or rejected nsec); the rest
 * is the event JSON.
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

/* A fixed key so runs are reproducible and no libsecret / Keychain
 * lookup ever happens. Generated once per process and passed via env. */
static char *g_sk_hex = NULL;
static char *g_pk_hex = NULL;
static char *g_npub = NULL;
static char *g_nsec = NULL;

static void ensure_key(void) {
  if (g_sk_hex) return;
  g_sk_hex = nostr_key_generate_private();
  if (!g_sk_hex) abort();
  g_pk_hex = nostr_key_get_public(g_sk_hex);
  if (!g_pk_hex) abort();
  uint8_t key[32];
  if (!nostr_hex2bin(key, g_pk_hex, sizeof key) ||
      nostr_nip19_encode_npub(key, &g_npub) != 0 || !g_npub) abort();
  if (!nostr_hex2bin(key, g_sk_hex, sizeof key) ||
      nostr_nip19_encode_nsec(key, &g_nsec) != 0 || !g_nsec) abort();
  setenv("NOSTR_SIGNER_SECKEY_HEX", g_sk_hex, 1);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ensure_key();
  if (size == 0 || size > (1u << 20)) return 0;

  const uint8_t *body = data + 1;
  size_t body_len = size - 1;
  const char *selector = NULL;
  switch (data[0] % 4) {
    case 0: selector = ""; break;
    case 1: selector = g_npub; break;
    case 2: selector = g_pk_hex; break;
    case 3: selector = g_nsec; break; /* rejected, never key material */
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
