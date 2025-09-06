#ifndef RELAYD_RETENTION_H
#define RELAYD_RETENTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "relayd_ctx.h"

/* Periodic retention maintenance. Safe to call in main loop. */
void retention_tick(const RelaydCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_RETENTION_H */
