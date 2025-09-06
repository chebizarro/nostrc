#include <stdint.h>
#include <time.h>
#include "relayd_conn.h"
#include "relayd_ctx.h"
#include "rate_limit.h"

/* Default token bucket: 20 ops/sec, burst 40; can be overridden by config. */
#define RL_DEFAULT_OPS_PER_SEC 20
#define RL_DEFAULT_BURST 40

uint64_t rate_limit_now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

void rate_limit_init_conn(ConnState *cs, const RelaydCtx *ctx) {
  if (!cs) return;
  unsigned int def_ops = RL_DEFAULT_OPS_PER_SEC;
  unsigned int def_burst = RL_DEFAULT_BURST;
  if (ctx) {
    if (ctx->cfg.rate_ops_per_sec > 0) def_ops = (unsigned int)ctx->cfg.rate_ops_per_sec;
    if (ctx->cfg.rate_burst > 0) def_burst = (unsigned int)ctx->cfg.rate_burst;
  }
  cs->rl_ops_per_sec = def_ops;
  cs->rl_burst = def_burst;
  cs->rl_tokens = def_burst;
  cs->rl_last_ms = rate_limit_now_ms();
}

int rate_limit_allow(ConnState *cs, uint64_t now_ms) {
  if (!cs) return 0;
  /* Use per-connection cached parameters */
  unsigned int ops_per_sec = cs->rl_ops_per_sec ? cs->rl_ops_per_sec : RL_DEFAULT_OPS_PER_SEC;
  unsigned int burst = cs->rl_burst ? cs->rl_burst : RL_DEFAULT_BURST;
  unsigned int refill = 0;
  if (ops_per_sec > 0) {
    unsigned int refill_ms = (unsigned int)(1000u / (unsigned int)ops_per_sec);
    uint64_t elapsed = now_ms > cs->rl_last_ms ? now_ms - cs->rl_last_ms : 0;
    refill = (unsigned int)(elapsed / refill_ms);
    if (refill > 0) {
      if (cs->rl_tokens + refill > burst) cs->rl_tokens = burst;
      else cs->rl_tokens += refill;
      cs->rl_last_ms += (uint64_t)refill * refill_ms;
    }
  }
  if (cs->rl_tokens == 0) return 0;
  cs->rl_tokens--;
  return 1;
}
