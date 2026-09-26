#ifndef RELAYD_METRICS_H
#define RELAYD_METRICS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void metrics_on_connect(void);
void metrics_on_disconnect(void);
void metrics_on_sub_start(void);
void metrics_on_sub_end(void);
void metrics_on_event_streamed(size_t n);
void metrics_on_eose(void);
void metrics_on_rate_limit_drop(void);
void metrics_on_backpressure_drop(void);
void metrics_on_duplicate_drop(void);
void metrics_on_skew_reject(void);
void metrics_on_validation_reject(void);
void metrics_on_verification_budget_drop(void);
void metrics_on_oversize_reject(void);

char *metrics_build_json(void);

/* Point-in-time copy of the gauges/counters other threads may display
 * (the session relay's org.nostr.SessionRelay1 D-Bus thread). Counters
 * are updated with relaxed atomics by the event-loop thread, so reading
 * them from another thread is race-free; the fields are individually,
 * not mutually, consistent. */
typedef struct {
  unsigned long connections_current;
  unsigned long connections_total;
  unsigned long subs_current;
  unsigned long events_streamed;
} RelaydMetricsSnapshot;

void metrics_snapshot(RelaydMetricsSnapshot *out);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_METRICS_H */
