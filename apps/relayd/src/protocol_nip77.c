#include <string.h>
#include <stdlib.h>
#include <libwebsockets.h>
#include "protocol_nip77.h"
#include "nostr-relay-core.h"

static void ws_send_text(struct lws *wsi, const char *s) {
  if (!wsi || !s) return;
  size_t blen = strlen(s);
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + blen);
  if (!buf) return;
  memcpy(&buf[LWS_PRE], s, blen);
  lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
  free(buf);
}

int relayd_nip77_handle_frame(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const char *msg, size_t len) {
  (void)cs;
  if (!msg || len < 14) return 0;
  if (!(len >= 14 && memcmp(msg, "[\"NEGENTROPY\"", 14) == 0)) return 0;
  /* If disabled by config, immediately return CLOSED */
  if (!ctx || !ctx->cfg.negentropy_enabled) {
    const char *p = strchr(msg, ',');
    char subbuf[64]; subbuf[0]='\0';
    if (p) {
      const char *q1 = strchr(p+1, '"'); if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2 && (size_t)(q2-(q1+1))<sizeof(subbuf)) { memcpy(subbuf, q1+1, (size_t)(q2-(q1+1))); subbuf[(size_t)(q2-(q1+1))]='\0'; } }
    }
    char *closed = nostr_closed_build_json(subbuf[0]?subbuf:"sub1", "unsupported: negentropy");
    if (closed) { ws_send_text(wsi, closed); free(closed);}        
    return 1;
  }
  /* TODO: implement when storage backend exposes NIP-77 APIs; for now CLOSED */
  char *closed = nostr_closed_build_json("sub1", "unsupported: negentropy");
  if (closed) { ws_send_text(wsi, closed); free(closed);}        
  return 1;
}
