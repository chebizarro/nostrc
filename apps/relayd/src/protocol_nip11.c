#include <string.h>
#include <stdlib.h>
#include <libwebsockets.h>
#include "relayd_ctx.h"
#include "protocol_nip11.h"
#include "nip11.h"

static void ws_http_send_json(struct lws *wsi, const char *json) {
  if (!wsi || !json) return;
  size_t blen = strlen(json);
  /* headers + body */
  unsigned char *buf = (unsigned char*)malloc(LWS_PRE + blen + 512);
  if (!buf) return;
  unsigned char *p = &buf[LWS_PRE];
  (void)lws_add_http_common_headers(wsi, HTTP_STATUS_OK, "application/json", blen, &p, buf + LWS_PRE + blen + 512);
  (void)lws_finalize_http_header(wsi, &p, buf + LWS_PRE + blen + 512);
  memcpy(p, json, blen);
  lws_write(wsi, &buf[LWS_PRE], (size_t)(p - &buf[LWS_PRE]) + blen, LWS_WRITE_HTTP);
  free(buf);
}

int relayd_handle_nip11_root(struct lws *wsi, const RelaydCtx *ctx) {
  char *body = NULL; int need_free = 0;
#ifdef HAVE_NIP11
  RelayInformationDocument doc = {0};
  if (ctx) {
    doc.name = ctx->cfg.name[0] ? ctx->cfg.name : (char*)"nostrc-relayd";
    doc.software = ctx->cfg.software[0] ? ctx->cfg.software : (char*)"nostrc";
    doc.version = ctx->cfg.version[0] ? ctx->cfg.version : (char*)"0.1";
    if (ctx->cfg.description[0]) doc.description = ctx->cfg.description;
    if (ctx->cfg.contact[0]) doc.contact = ctx->cfg.contact;
    doc.supported_nips = (int*)ctx->cfg.supported_nips;
    doc.supported_nips_count = ctx->cfg.supported_nips_count;
    RelayLimitationDocument L = {0};
    L.max_filters = ctx->cfg.max_filters;
    L.max_limit = ctx->cfg.max_limit;
    doc.limitation = &L;
    body = nostr_nip11_build_info_json(&doc);
    /* Note: L is on stack; builder must copy the fields it uses. */
  } else {
    doc.name = (char*)"nostrc-relayd";
    doc.software = (char*)"nostrc";
    doc.version = (char*)"0.1";
    body = nostr_nip11_build_info_json(&doc);
  }
  need_free = (body != NULL);
#else
  const char *name = (ctx && ctx->cfg.name[0]) ? ctx->cfg.name : "nostrc-relayd";
  const char *software = (ctx && ctx->cfg.software[0]) ? ctx->cfg.software : "nostrc";
  const char *version = (ctx && ctx->cfg.version[0]) ? ctx->cfg.version : "0.1";
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "{\"name\":\"%s\",\"software\":\"%s\",\"version\":\"%s\"}", name, software, version);
  body = strdup(tmp);
  need_free = 1;
#endif
  if (!body) body = (char*)"{\"name\":\"nostrc-relayd\"}";
  ws_http_send_json(wsi, body);
  if (need_free && body && body[0] == '{') free(body);
  return 0;
}
