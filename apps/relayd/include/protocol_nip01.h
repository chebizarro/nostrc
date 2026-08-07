#ifndef RELAYD_PROTOCOL_NIP01_H
#define RELAYD_PROTOCOL_NIP01_H

#include <stddef.h>
#include <stdint.h>
#include <libwebsockets.h>

#include "rate_limit.h"
#include "relay_policy.h"
#include "relayd_conn.h"
#include "relayd_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

void relayd_nip01_on_writable(struct lws *wsi, ConnState *cs,
                              const RelaydCtx *ctx);
void relayd_nip01_on_receive(struct lws *wsi, ConnState *cs,
                             const RelaydCtx *ctx, const void *in, size_t len);

int relayd_nip01_ingress_decide_json(
    RelayPolicy *policy, VerificationBudget *verification_budget,
    RateLimitBucket *connection_verification_bucket, const char *peer_ip,
    const char *event_json, size_t event_json_len, size_t max_event_bytes,
    uint32_t verification_cost, int64_t now_wall, uint64_t now_mono_ms,
    char canonical_id_out[65], const char **out_reason);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_PROTOCOL_NIP01_H */
