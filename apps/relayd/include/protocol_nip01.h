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
void relayd_nip01_reconcile_all(const RelaydCtx *ctx);
void relayd_nip01_on_timer(struct lws *wsi, ConnState *cs, const RelaydCtx *ctx);
void relayd_conn_acks_free_all(ConnState *cs, const RelaydCtx *ctx);
void relayd_nip01_on_receive(struct lws *wsi, ConnState *cs,
                             const RelaydCtx *ctx, const void *in, size_t len);

/* Storage that can serve REQs: query + query_next + query_free. Iterators
 * from both query() and search() are released with query_free(). */
int relayd_storage_can_query(const NostrStorage *st);

/* Queue a stored-result iterator for `sub` (caller checked capacity and
 * that `sub` is not already pending) and schedule streaming. Takes
 * ownership of `it`. */
void relayd_conn_sub_push(struct lws *wsi, ConnState *cs, const char *sub,
                          size_t sub_len, void *it);

/* Release every pending iterator (connection teardown). */
void relayd_conn_subs_free_all(ConnState *cs, const RelaydCtx *ctx);

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
