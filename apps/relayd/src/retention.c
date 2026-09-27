#include "relayd_ctx.h"
#include "retention.h"

/* Retention hook, called every ~5 s from the server loop. Deliberately
 * empty: eviction needs a delete primitive the nostrdb driver does not have
 * (nostrdb has no delete API -- nostrc-8rxk Phase 1), so nothing is evicted
 * and the session relay reports SessionRelay1.RetentionSupported = false
 * (nostrc-prqu.17). This used to log "retention tick" as if it did work. */
void retention_tick(const RelaydCtx *ctx) {
  (void)ctx;
}
