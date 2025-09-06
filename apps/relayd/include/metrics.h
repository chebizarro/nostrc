#ifndef RELAYD_METRICS_H
#define RELAYD_METRICS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

void metrics_on_connect(void);
void metrics_on_disconnect(void);

void metrics_on_sub_start(void);
void metrics_on_sub_end(void);

void metrics_on_event_streamed(size_t n);
void metrics_on_eose(void);

void metrics_on_rate_limit_drop(void);
void metrics_on_backpressure_drop(void);

/* Returns a newly allocated JSON string with metrics snapshot; caller must free. */
char *metrics_build_json(void);

#ifdef __cplusplus
}
#endif

#endif /* RELAYD_METRICS_H */
