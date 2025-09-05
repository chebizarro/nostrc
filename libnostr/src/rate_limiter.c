#include "../include/rate_limiter.h"
#include <time.h>

static double now_seconds(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

void tb_init(nostr_token_bucket *tb, double rate, double burst){
  if (!tb) return;
  tb->tokens = burst;
  tb->rate = rate;
  tb->burst = burst;
  tb->last_ts = now_seconds();
}

void tb_set_now(nostr_token_bucket *tb, double now){
  if (!tb) return;
  tb->last_ts = now;
}

bool tb_allow(nostr_token_bucket *tb, double cost){
  if (!tb) return false;
  double now = now_seconds();
  double elapsed = now - tb->last_ts;
  if (elapsed < 0) elapsed = 0;
  tb->last_ts = now;
  tb->tokens += elapsed * tb->rate;
  if (tb->tokens > tb->burst) tb->tokens = tb->burst;
  if (tb->tokens >= cost){
    tb->tokens -= cost;
    return true;
  }
  return false;
}
