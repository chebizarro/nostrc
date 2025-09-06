#ifndef RELAYD_PROTOCOL_NIP42_H
#define RELAYD_PROTOCOL_NIP42_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <libwebsockets.h>
#include "relayd_ctx.h"
#include "relayd_conn.h"

/* If an AUTH challenge needs to be sent, send it and clear need_auth_chal.
 * Returns 1 if a challenge was sent (and no further writable work should run), 0 otherwise.
 */
int relayd_nip42_maybe_send_challenge_on_writable(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx);

/* Handle an incoming AUTH frame: ["AUTH", {event}]. Returns 1 if handled, 0 otherwise. */
int relayd_nip42_handle_auth_frame(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx, const char *msg, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP42_H */
