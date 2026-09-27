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
  /* NUL-terminated copy: `subid` points into the frame, and the CLOSED
   * builders below would otherwise take the rest of the frame as the id. */
  char sub[128];
  if (subid) {
    size_t cplen = sublen < sizeof(sub) - 1 ? sublen : sizeof(sub) - 1;
    memcpy(sub, subid, cplen);
    sub[cplen] = '\0';
  } else {
    snprintf(sub, sizeof sub, "%s", "sub1");
  }
  NostrStorage *st = ctx->storage;
  if (!st || !st->vt || !st->vt->count || !filters_json) {
    char *closed = nostr_closed_build_json(
        sub, !filters_json ? "invalid: COUNT needs a filter"
                           : "unsupported: count requires a storage backend");
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
  /* nostr_filter_new(), not a zeroed struct: the deserializer appends to
   * the filter's arrays, which need their initial capacity. */
  NostrFilter *farr = nostr_filter_new(); size_t fn = 1;
  int ok = -1;
  if (farr && *fb == '{') {
    ok = nostr_filter_deserialize(farr, fb);
  } else if (farr && *fb == '[') {
    const char *q = strchr(fb, '{');
    if (q) ok = nostr_filter_deserialize(farr, q);
  }
  if (ok != 0) {
    nostr_filter_free(farr);
    char *closed = nostr_closed_build_json(sub, nostr_limits_reason_invalid_filter());
    if (closed) { ws_send_text(wsi, closed); free(closed);}          
    free(fbuf);
    return -EINVAL;
  }
  if (ctx && farr->limit > ctx->cfg.max_limit) farr->limit = ctx->cfg.max_limit;
  uint64_t cval = 0; int rc = st->vt->count(st, farr, fn, &cval);
  if (rc == 0) {
    char body[192]; snprintf(body, sizeof(body), "[\"COUNT\",\"%s\",{\"count\":%llu}]", sub, (unsigned long long)cval);
    ws_send_text(wsi, body);
  } else {
    char *closed = nostr_closed_build_json(sub, "count-failed");
    if (closed) { ws_send_text(wsi, closed); free(closed);}          
  }
  nostr_filter_free(farr);
  free(fbuf);
  return 0;
}
