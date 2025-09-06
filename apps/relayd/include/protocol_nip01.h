#ifndef RELAYD_PROTOCOL_NIP01_H
#define RELAYD_PROTOCOL_NIP01_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <libwebsockets.h>
#include "relayd_ctx.h"
#include "relayd_conn.h"

/* Handle writeable callback: stream iterator results, AUTH challenge, EOSE */
void relayd_nip01_on_writable(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx);

/* Handle inbound WS frame for NIP-01: EVENT/REQ/CLOSE */
void relayd_nip01_on_receive(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const void *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP01_H */
