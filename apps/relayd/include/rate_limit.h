#ifndef RELAYD_RATE_LIMIT_H
#define RELAYD_RATE_LIMIT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "relayd_conn.h"
#include "relayd_ctx.h"

/* Initialize per-connection rate limiting state */
void rate_limit_init_conn(ConnState *cs, const RelaydCtx *ctx);

/* Return 1 if the operation is allowed at current time, 0 if it should be rate-limited. */
int rate_limit_allow(ConnState *cs, uint64_t now_ms);

/* Get current monotonic-ish ms time (coarse) */
uint64_t rate_limit_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_RATE_LIMIT_H */
