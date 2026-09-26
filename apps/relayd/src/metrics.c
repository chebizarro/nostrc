#include "metrics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
  unsigned long connections_current;
  unsigned long connections_total;
  unsigned long connections_closed;
  unsigned long subs_current;
  unsigned long subs_started;
  unsigned long subs_ended;
  unsigned long events_streamed;
  unsigned long eose_sent;
  unsigned long rate_limit_drops;
  unsigned long backpressure_drops;
  unsigned long duplicate_drops;
  unsigned long skew_rejects;
  unsigned long validation_rejects;
  unsigned long verification_budget_drops;
  unsigned long oversize_rejects;
} M;

/* Relaxed atomics: one writer (the lws event-loop thread) and readers on
 * other threads (metrics_snapshot). No ordering is implied between
 * counters. */
#define M_INC(f) __atomic_fetch_add(&M.f, 1UL, __ATOMIC_RELAXED)
#define M_ADD(f, n) __atomic_fetch_add(&M.f, (unsigned long)(n), __ATOMIC_RELAXED)
#define M_DEC_SAT(f)                                                     \
  do {                                                                   \
    unsigned long _v = __atomic_load_n(&M.f, __ATOMIC_RELAXED);          \
    while (_v > 0 && !__atomic_compare_exchange_n(&M.f, &_v, _v - 1, 0,  \
                                                  __ATOMIC_RELAXED,      \
                                                  __ATOMIC_RELAXED)) {   \
    }                                                                    \
  } while (0)
#define M_LOAD(f) __atomic_load_n(&M.f, __ATOMIC_RELAXED)

void metrics_on_connect(void) { M_INC(connections_current); M_INC(connections_total); }
void metrics_on_disconnect(void) {
  M_DEC_SAT(connections_current);
  M_INC(connections_closed);
}
void metrics_on_sub_start(void) { M_INC(subs_current); M_INC(subs_started); }
void metrics_on_sub_end(void) {
  M_DEC_SAT(subs_current);
  M_INC(subs_ended);
}
void metrics_on_event_streamed(size_t n) { M_ADD(events_streamed, n); }
void metrics_on_eose(void) { M_INC(eose_sent); }
void metrics_on_rate_limit_drop(void) { M_INC(rate_limit_drops); }
void metrics_on_backpressure_drop(void) { M_INC(backpressure_drops); }
void metrics_on_duplicate_drop(void) { M_INC(duplicate_drops); }
void metrics_on_skew_reject(void) { M_INC(skew_rejects); }
void metrics_on_validation_reject(void) { M_INC(validation_rejects); }
void metrics_on_verification_budget_drop(void) {
  M_INC(verification_budget_drops);
}
void metrics_on_oversize_reject(void) { M_INC(oversize_rejects); }

void metrics_snapshot(RelaydMetricsSnapshot *out) {
  if (!out) return;
  out->connections_current = M_LOAD(connections_current);
  out->connections_total = M_LOAD(connections_total);
  out->subs_current = M_LOAD(subs_current);
  out->events_streamed = M_LOAD(events_streamed);
}

char *metrics_build_json(void) {
  char buf[1400];
  int n = snprintf(
      buf, sizeof(buf),
      "{\"connections\":{\"current\":%lu,\"total\":%lu,\"closed\":%lu},"
      "\"subs\":{\"current\":%lu,\"started\":%lu,\"ended\":%lu},"
      "\"stream\":{\"events\":%lu,\"eose\":%lu},"
      "\"drops\":{\"rate_limit\":%lu,\"backpressure\":%lu,"
      "\"duplicate\":%lu,\"skew\":%lu,\"validation\":%lu,"
      "\"verification_budget\":%lu,\"oversize\":%lu}}",
      M_LOAD(connections_current), M_LOAD(connections_total),
      M_LOAD(connections_closed), M_LOAD(subs_current), M_LOAD(subs_started),
      M_LOAD(subs_ended), M_LOAD(events_streamed), M_LOAD(eose_sent),
      M_LOAD(rate_limit_drops), M_LOAD(backpressure_drops),
      M_LOAD(duplicate_drops), M_LOAD(skew_rejects),
      M_LOAD(validation_rejects), M_LOAD(verification_budget_drops),
      M_LOAD(oversize_rejects));
  if (n <= 0 || (size_t)n >= sizeof(buf)) return NULL;
  char *out = malloc((size_t)n + 1);
  if (!out) return NULL;
  memcpy(out, buf, (size_t)n + 1);
  return out;
}
