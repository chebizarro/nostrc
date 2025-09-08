#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "json.h"
#include "nostr-filter.h"

// Fuzz harness for filter parser/validator
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!data || size == 0) return 0;
  // NUL-terminate input
  char *buf = (char*)malloc(size + 1);
  if (!buf) return 0;
  memcpy(buf, data, size);
  buf[size] = '\0';

  // Attempt to parse a single filter JSON
  NostrFilter *f = nostr_filter_new();
  if (f) {
    // Try robust parser; it will enforce bounds
    (void)nostr_filter_deserialize(f, buf);
    // Optionally serialize back (exercise serializer)
    char *round = nostr_filter_serialize(f);
    if (round) free(round);
    nostr_filter_free(f);
  }

  // Note: array-of-filters path not available in public API; single-filter fuzz is sufficient.

  free(buf);
  return 0;
}
