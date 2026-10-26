#include <string.h>
#include <stdlib.h>
#include <libwebsockets.h>
#include "nostr-filter.h"
#include "nostr-storage.h"
#include "nostr-relay-core.h"
#include "relayd_ctx.h"
#include "relayd_conn.h"
#include "protocol_nip50.h"
#include "protocol_nip01.h"

#define ws_send_text relayd_conn_send_text

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
  if (scope->limit_zero) {
    relayd_conn_sub_push(wsi, cs, sub, sub_len, NULL);
    return 1;
  }
  void *it = NULL; int rc = st->vt->search(st, qsearch, scope, 0, &it);
  char subtmp[128]; size_t cplen = sub_len < sizeof(subtmp)-1 ? sub_len : sizeof(subtmp)-1; memcpy(subtmp, sub, cplen); subtmp[cplen]='\0';
  if (rc == -ENOTSUP || !it) {
    char *closed = nostr_closed_build_json(subtmp, "unsupported: search");
    if (closed) { ws_send_text(wsi, closed); free(closed);}                
    return 1;
  }
  relayd_conn_sub_push(wsi, cs, sub, sub_len, it);
  return 1;
}
