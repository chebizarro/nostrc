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

/* Runtime-configurable mitigations */
void nostr_relay_set_replay_ttl(int seconds);
void nostr_relay_set_skew(int future_seconds, int past_seconds);
int  nostr_relay_get_replay_ttl(void);
void nostr_relay_get_skew(int *future_seconds, int *past_seconds);

/* Testable ingress decision function (no websockets) */
int relayd_nip01_ingress_decide_json(const char *event_json, int64_t now_override, const char **out_reason);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP01_H */
