#ifndef RELAYD_PROTOCOL_NIP11_H
#define RELAYD_PROTOCOL_NIP11_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libwebsockets.h>
#include "relayd_ctx.h"

/* Serve NIP-11 relay information document for GET /. */
int relayd_handle_nip11_root(struct lws *wsi, const RelaydCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP11_H */
