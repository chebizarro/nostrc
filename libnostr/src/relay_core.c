#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "nostr-relay-core.h"
#include "nostr-utils.h"

char *nostr_ok_build_json(const char *event_id_hex, int ok, const char *reason) {
  if (!event_id_hex) return NULL;
  char *eid = nostr_escape_string(event_id_hex);
  char *rs = nostr_escape_string(reason ? reason : "");
  if (!eid || !rs) { free(eid); free(rs); return NULL; }
  size_t cap = 32 + strlen(eid) + strlen(rs) + 16;
  char *buf = (char*)malloc(cap);
  if (!buf) { free(eid); free(rs); return NULL; }
  int n = snprintf(buf, cap, "[\"OK\",\"%s\",%s,\"%s\"]",
                   eid, ok?"true":"false", rs);
  free(eid);
  free(rs);
  if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
  return buf;
}

char *nostr_closed_build_json(const char *sub_id, const char *reason) {
  if (!sub_id) return NULL;
  char *sid = nostr_escape_string(sub_id);
  char *rs = nostr_escape_string(reason ? reason : "");
  if (!sid || !rs) { free(sid); free(rs); return NULL; }
  size_t cap = 32 + strlen(sid) + strlen(rs) + 8;
  char *buf = (char*)malloc(cap);
  if (!buf) { free(sid); free(rs); return NULL; }
  int n = snprintf(buf, cap, "[\"CLOSED\",\"%s\",\"%s\"]", sid, rs);
  free(sid);
  free(rs);
  if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
  return buf;
}

char *nostr_eose_build_json(const char *sub_id) {
  if (!sub_id) return NULL;
  char *sid = nostr_escape_string(sub_id);
  if (!sid) return NULL;
  size_t cap = 16 + strlen(sid);
  char *buf = (char*)malloc(cap);
  if (!buf) { free(sid); return NULL; }
  int n = snprintf(buf, cap, "[\"EOSE\",\"%s\"]", sid);
  free(sid);
  if (n < 0 || (size_t)n >= cap) { free(buf); return NULL; }
  return buf;
}
