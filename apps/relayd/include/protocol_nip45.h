#ifndef RELAYD_PROTOCOL_NIP45_H
#define RELAYD_PROTOCOL_NIP45_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <libwebsockets.h>
#include "relayd_ctx.h"

/* Handle NIP-45 COUNT frame: ["COUNT","subid", <filters...>] */
int relayd_handle_count(struct lws *wsi, const RelaydCtx *ctx, const char *msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP45_H */
