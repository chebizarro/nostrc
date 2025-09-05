#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "nostr-json.h"
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
    (void)nostr_filter_from_json(f, buf);
    // Optionally serialize back (exercise serializer)
    char *round = nostr_filter_to_json(f);
    if (round) free(round);
    nostr_filter_free(f);
  }

  // Attempt to parse an array of filters as well
  NostrFilter **arr = NULL; size_t n = 0;
  (void)nostr_filters_from_json(buf, &arr, &n);
  if (arr) {
    for (size_t i = 0; i < n; ++i) if (arr[i]) nostr_filter_free(arr[i]);
    free(arr);
  }

  free(buf);
  return 0;
}
