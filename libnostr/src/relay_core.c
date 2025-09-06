#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "nostr-relay-core.h"

char *nostr_ok_build_json(const char *event_id_hex, int ok, const char *reason) {
  if (!event_id_hex) return NULL;
  const char *rs = reason ? reason : "";
  size_t cap = 32 + strlen(event_id_hex) + strlen(rs) + 16;
  char *buf = (char*)malloc(cap);
  if (!buf) return NULL;
  int n = snprintf(buf, cap, "[\"OK\",\"%s\",%s,\"%s\"]",
                   event_id_hex, ok?"true":"false", rs);
  if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
  return buf;
}

char *nostr_closed_build_json(const char *sub_id, const char *reason) {
  if (!sub_id) return NULL;
  const char *rs = reason ? reason : "";
  size_t cap = 32 + strlen(sub_id) + strlen(rs) + 8;
  char *buf = (char*)malloc(cap);
  if (!buf) return NULL;
  int n = snprintf(buf, cap, "[\"CLOSED\",\"%s\",\"%s\"]", sub_id, rs);
  if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
  return buf;
}

char *nostr_eose_build_json(const char *sub_id) {
  if (!sub_id) return NULL;
  size_t cap = 16 + strlen(sub_id);
  char *buf = (char*)malloc(cap);
  if (!buf) return NULL;
  int n = snprintf(buf, cap, "[\"EOSE\",\"%s\"]", sub_id);
  if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
  return buf;
}
