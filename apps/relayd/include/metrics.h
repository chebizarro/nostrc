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

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_METRICS_H */
