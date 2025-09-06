#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <libwebsockets.h>
#include "nostr-json.h"
#include "nostr-filter.h"
#include "nostr-storage.h"
#include "nostr-relay-limits.h"
#include "nostr-relay-core.h"
#include "relayd_ctx.h"
#include "protocol_nip45.h"

static void ws_send_text(struct lws *wsi, const char *s) {
  if (!wsi || !s) return;
  size_t blen = strlen(s);
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + blen);
  if (!buf) return;
  memcpy(&buf[LWS_PRE], s, blen);
  lws_write(wsi, &buf[LWS_PRE], blen, LWS_WRITE_TEXT);
  free(buf);
}

int relayd_handle_count(struct lws *wsi, const RelaydCtx *ctx, const char *msg, size_t len) {
  if (!wsi || !ctx || !msg || len < 8) return -EINVAL;
  const char *p = strchr(msg, ',');
  const char *subid = NULL; size_t sublen = 0;
  if (p) {
    const char *q1 = strchr(p+1, '"'); if (q1) { const char *q2 = strchr(q1+1, '"'); if (q2) { subid = q1+1; sublen = (size_t)(q2 - (q1+1)); p = strchr(q2, ','); } }
  }
  const char *filters_json = p ? p+1 : NULL;
  const char *sub = subid ? subid : "sub1";
  NostrStorage *st = ctx->storage;
  if (!st || !st->vt || !st->vt->count || !filters_json) {
    char *closed = nostr_closed_build_json(sub, "invalid: count");
    if (closed) { ws_send_text(wsi, closed); free(closed);}          
    return -EINVAL;
  }
  size_t flen = (size_t)len - (filters_json - msg);
  while (flen > 0 && (filters_json[flen-1] == '\n' || filters_json[flen-1] == '\r' || filters_json[flen-1] == ' ')) flen--;
  if (flen > 0 && filters_json[flen-1] == ']') flen--;
  char *fbuf = (char*)malloc(flen + 1);
  if (!fbuf) return -ENOMEM;
  memcpy(fbuf, filters_json, flen); fbuf[flen] = '\0';
  /* For simplicity, expect one filter object; if array, take first element. */
  const char *fb = fbuf; while (*fb && *fb != '{' && *fb != '[') fb++;
  NostrFilter ftmp; memset(&ftmp, 0, sizeof(ftmp)); NostrFilter *farr = &ftmp; size_t fn = 1;
  int ok = -1;
  if (*fb == '{') {
    ok = nostr_filter_deserialize(&ftmp, fb);
  } else if (*fb == '[') {
    const char *q = strchr(fb, '{');
    if (q) ok = nostr_filter_deserialize(&ftmp, q);
  }
  if (ok != 0) {
    char *closed = nostr_closed_build_json(sub, nostr_limits_reason_invalid_filter());
    if (closed) { ws_send_text(wsi, closed); free(closed);}          
    free(fbuf);
    return -EINVAL;
  }
  if (ctx && ftmp.limit > ctx->cfg.max_limit) ftmp.limit = ctx->cfg.max_limit;
  uint64_t cval = 0; int rc = st->vt->count(st, farr, fn, &cval);
  if (rc == 0) {
    char body[128]; snprintf(body, sizeof(body), "[\"COUNT\",\"%.*s\",{\"count\":%llu}]", (int)(sublen ? sublen : strlen(sub)), sub, (unsigned long long)cval);
    ws_send_text(wsi, body);
  } else {
    char *closed = nostr_closed_build_json(sub, "count-failed");
    if (closed) { ws_send_text(wsi, closed); free(closed);}          
  }
  nostr_filter_free(&ftmp);
  free(fbuf);
  return 0;
}
