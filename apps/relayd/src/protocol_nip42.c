#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <libwebsockets.h>
#include "nostr-json.h"
#include "nostr-event.h"
#include "nostr-relay-core.h"
#include "relayd_ctx.h"
#include "relayd_conn.h"
#include "protocol_nip42.h"

int relayd_nip42_maybe_send_challenge_on_writable(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx) {
  (void)ctx;
  if (!wsi || !cs) return 0;
  if (!cs->need_auth_chal) return 0;
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + 64 + strlen(cs->auth_chal));
  if (buf) {
    int m = snprintf((char*)&buf[LWS_PRE], 64 + (int)strlen(cs->auth_chal), "[\"AUTH\",\"%s\"]", cs->auth_chal);
    if (m > 0) lws_write(wsi, &buf[LWS_PRE], (size_t)m, LWS_WRITE_TEXT);
    free(buf);
  }
  cs->need_auth_chal = 0;
  return 1;
}

int relayd_nip42_handle_auth_frame(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const char *msg, size_t len) {
  (void)wsi; (void)ctx;
  if (!cs || !msg || len < 7) return 0;
  if (!(len >= 7 && memcmp(msg, "[\"AUTH\"", 7) == 0)) return 0;
  const char *payload = strchr(msg, ',');
  if (!payload) return 1; /* malformed AUTH; treat as handled */
  payload++;
  size_t plen = (size_t)len - (payload - msg);
  while (plen > 0 && (payload[plen-1] == '\n' || payload[plen-1] == '\r' || payload[plen-1] == ' ')) plen--;
  if (plen > 0 && payload[plen-1] == ']') plen--;
  if (plen == 0 || payload[0] != '{') return 1;
  char *pbuf = (char*)malloc(plen + 1);
  if (!pbuf) return 1;
  memcpy(pbuf, payload, plen); pbuf[plen] = '\0';
  NostrEvent *ev = nostr_event_new();
  bool ok_parse = false;
  if (ev) {
    if (nostr_event_deserialize_compact(ev, pbuf, NULL)) ok_parse = true;
    else ok_parse = (nostr_event_deserialize(ev, pbuf) == 0);
  }
  if (ok_parse && nostr_event_check_signature(ev)) {
    /* Verify challenge tag */
    NostrTags *tags = (NostrTags*)nostr_event_get_tags(ev);
    const char *chal = NULL;
    if (tags) {
      size_t tcount = nostr_tags_size(tags);
      for (size_t i = 0; i < tcount; i++) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *k = t ? nostr_tag_get_key(t) : NULL;
        if (k && strcmp(k, "challenge") == 0) { chal = nostr_tag_get_value(t); break; }
      }
    }
    if (chal && cs->auth_chal[0] && strcmp(chal, cs->auth_chal) == 0) {
      const char *pk = nostr_event_get_pubkey(ev);
      if (pk && *pk) {
        snprintf(cs->authed_pubkey, sizeof(cs->authed_pubkey), "%s", pk);
        cs->authed = 1;
        fprintf(stderr, "relayd: AUTH verified pubkey=%s\n", cs->authed_pubkey);
      }
    } else {
      fprintf(stderr, "relayd: AUTH rejected (challenge mismatch)\n");
    }
  } else {
    fprintf(stderr, "relayd: AUTH rejected (bad signature or parse)\n");
  }
  if (ev) nostr_event_free(ev);
  free(pbuf);
  return 1;
}
