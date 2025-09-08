#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "nostr-event.h"
#include "json.h"

// Fuzz harness for signature verify path
// Strategy: try to deserialize input as an event JSON, then call verify.
// If it fails to deserialize, attempt to wrap input as a minimal event envelope.
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (!data || size == 0) return 0;
  char *buf = (char*)malloc(size + 1);
  if (!buf) return 0;
  memcpy(buf, data, size);
  buf[size] = '\0';

  NostrEvent *ev = nostr_event_new();
  if (!ev) { free(buf); return 0; }

  int parsed = nostr_event_deserialize(ev, buf);
  if (parsed != 0) {
    // Try to craft a minimal event using the blob as content to exercise the verify path
    nostr_event_set_kind(ev, 1);
    nostr_event_set_created_at(ev, (int64_t)time(NULL));
    nostr_event_set_content(ev, buf);
  }

  // Attempt verify (will handle missing/invalid fields internally)
  (void)nostr_event_check_signature(ev);

  nostr_event_free(ev);
  free(buf);
  return 0;
}
