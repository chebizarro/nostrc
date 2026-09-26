/* fuzz_nip55l_relays — nostr_nip55l_relays_normalize_json.
 *
 * The input is the whole relays.conf document: a JSON array of ws:// / wss://
 * URL strings. The normaliser lowercases scheme+host, drops a bare trailing
 * "/", removes duplicates, and refuses non-relay URLs / JSON escapes / any
 * shape that is not exactly a JSON array of plain strings.
 *
 * A crash, an ASAN/UBSan report or a leak here is a normaliser bug, not a
 * caller bug: the whole point is to keep the D-Bus dispatch (GetRelays) from
 * having to interpret adversarial config.
 *
 * Issue: nostrc-tf3b (deferred from nostrc-p7f6).
 */

#include "nostr/nip55l/signer_ops.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  /* Documents larger than the enforced cap are refused up front by the
   * normaliser; still exercise the check so any off-by-one shows up here. */
  if (size > (1u << 20)) return 0;

  char *out = NULL;
  int rc = nostr_nip55l_relays_normalize_json((const char *)data, size, &out);
  if (rc == 0 && out) free(out);

  /* Also exercise the list variant with a small parse of the input into a
   * URL list. Take up to 8 short slices of the input, each with a synthetic
   * "wss://" prefix, so the list-side normaliser sees candidate URLs it
   * would otherwise never be shown. */
  const char *slices[8] = { NULL };
  char *bufs[8] = { NULL };
  size_t n = 0;
  size_t i = 0;
  while (n < 8 && i < size) {
    size_t j = i;
    while (j < size && data[j] != '\n' && data[j] != ',') j++;
    size_t frag = j - i;
    if (frag > 128) frag = 128;
    char *b = (char *)malloc(6 + frag + 1);
    if (!b) break;
    memcpy(b, "wss://", 6);
    memcpy(b + 6, data + i, frag);
    b[6 + frag] = '\0';
    bufs[n] = b;
    slices[n] = b;
    n++;
    i = (j < size) ? j + 1 : j;
  }
  if (n > 0) {
    char *out2 = NULL;
    rc = nostr_nip55l_relays_from_list(slices, n, &out2);
    if (rc == 0 && out2) free(out2);
  }
  for (size_t k = 0; k < n; k++) free(bufs[k]);

  return 0;
}
