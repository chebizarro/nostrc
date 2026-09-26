/* fuzz_nip55l_decrypt_zap_event — nostr_nip55l_decrypt_zap_event.
 *
 * A zap event (NIP-57) carries an encrypted content field addressed to the
 * user; the signer parses the event, walks tags to find the `p` peer, and
 * attempts a NIP-44 v2 decrypt of the content (with a NIP-04 fallback). Any
 * bug in that parse — tag walk, hex decode of the peer, decoder call chain
 * — must fail the call, never crash.
 *
 * Issue: nostrc-tf3b (deferred from nostrc-p7f6).
 */

#include "nostr/nip55l/signer_ops.h"

#include <nostr-keys.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *g_sk_hex = NULL;

static void ensure_key(void) {
  if (g_sk_hex) return;
  g_sk_hex = nostr_key_generate_private();
  if (!g_sk_hex) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  ensure_key();
  if (size > (1u << 20)) return 0;
  char *js = (char *)malloc(size + 1);
  if (!js) return 0;
  memcpy(js, data, size);
  js[size] = '\0';
  char *out = NULL;
  int rc = nostr_nip55l_decrypt_zap_event(js, g_sk_hex, &out);
  if (rc == 0) {
    if (!out) abort();
    free(out);
  } else if (out) {
    abort();
  }
  free(js);
  return 0;
}
