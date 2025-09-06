#ifndef RELAYD_PROTOCOL_NIP77_H
#define RELAYD_PROTOCOL_NIP77_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <libwebsockets.h>
#include "relayd_ctx.h"
#include "relayd_conn.h"

/* Handle NIP-77 NEGENTROPY frame if present. Returns 1 if handled. */
int relayd_nip77_handle_frame(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const char *msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP77_H */
