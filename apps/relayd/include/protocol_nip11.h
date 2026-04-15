#ifndef RELAYD_PROTOCOL_NIP11_H
#define RELAYD_PROTOCOL_NIP11_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libwebsockets.h>
#include "relayd_ctx.h"

/* Serve NIP-11 relay information document for GET /. */
int relayd_handle_nip11_root(struct lws *wsi, const RelaydCtx *ctx);

/* Respond to CORS preflight (OPTIONS) for NIP-11 endpoint. */
int relayd_handle_nip11_options(struct lws *wsi);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP11_H */
