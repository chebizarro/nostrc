#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "nostr-event.h"
#include "json.h"

// Simple fuzz harness for the compact event deserializer
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!data || size == 0) return 0;
  // Ensure input is NUL-terminated (copy to buffer)
  char *buf = (char*)malloc(size + 1);
  if (!buf) return 0;
  memcpy(buf, data, size);
  buf[size] = '\0';

  NostrEvent *ev = nostr_event_new();
  if (ev) {
    // Try compact fast-path first; fall back to public API parse
    if (!nostr_event_deserialize_compact(ev, buf, NULL)) {
      (void)nostr_event_deserialize(ev, buf);
    }
    nostr_event_free(ev);
  }
  free(buf);
  return 0;
}
