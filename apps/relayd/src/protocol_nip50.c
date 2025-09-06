#include <string.h>
#include <stdlib.h>
#include <libwebsockets.h>
#include "nostr-filter.h"
#include "nostr-storage.h"
#include "nostr-relay-core.h"
#include "relayd_ctx.h"
#include "relayd_conn.h"
#include "protocol_nip50.h"

static void ws_send_text(struct lws *wsi, const char *s) {
  if (!wsi || !s) return;
  size_t blen = strlen(s);
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + blen);
  if (!buf) return;
  memcpy(&buf[LWS_PRE], s, blen);
  lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
  free(buf);
}

int relayd_nip50_maybe_start_search(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx,
                                    const char *sub, size_t sub_len,
                                    NostrFilter **arr, size_t n) {
  if (!wsi || !ctx || !cs) return 0;
  NostrStorage *st = ctx->storage;
  const char *qsearch = NULL; const NostrFilter *scope = NULL;
  for (size_t i=0;i<n;i++) {
    const char *q = nostr_filter_get_search(arr[i]);
    if (q && *q) { qsearch = q; scope = arr[i]; break; }
  }
  if (!qsearch) return 0; /* no search keys present */
  if (!st || !st->vt || !st->vt->search) {
    char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
    char *closed = nostr_closed_build_json(subtmp, "unsupported: search");
    if (closed) { ws_send_text(wsi, closed); free(closed);}                
    return 1;
  }
  void *it = NULL; int rc = st->vt->search(st, qsearch, scope, 0, &it);
  char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
  if (rc == -ENOTSUP || !it) {
    char *closed = nostr_closed_build_json(subtmp, "unsupported: search");
    if (closed) { ws_send_text(wsi, closed); free(closed);}                
    return 1;
  }
  cs->it = it; memcpy(cs->subid, subtmp, strlen(subtmp)+1);
  lws_callback_on_writable(wsi);
  return 1;
}
