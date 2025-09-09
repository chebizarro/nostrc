#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "metrics.h"

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
} M;

void metrics_on_connect(void){ M.connections_current++; M.connections_total++; }
void metrics_on_disconnect(void){ if (M.connections_current>0) M.connections_current--; M.connections_closed++; }

void metrics_on_sub_start(void){ M.subs_current++; M.subs_started++; }
void metrics_on_sub_end(void){ if (M.subs_current>0) M.subs_current--; M.subs_ended++; }

void metrics_on_event_streamed(size_t n){ M.events_streamed += (unsigned long)n; }
void metrics_on_eose(void){ M.eose_sent++; }

void metrics_on_rate_limit_drop(void){ M.rate_limit_drops++; }
void metrics_on_backpressure_drop(void){ M.backpressure_drops++; }
void metrics_on_duplicate_drop(void){ M.duplicate_drops++; }
void metrics_on_skew_reject(void){ M.skew_rejects++; }

char *metrics_build_json(void){
  char buf[1024];
  int n = snprintf(buf, sizeof(buf),
    "{\"connections\":{\"current\":%lu,\"total\":%lu,\"closed\":%lu},"
    "\"subs\":{\"current\":%lu,\"started\":%lu,\"ended\":%lu},"
    "\"stream\":{\"events\":%lu,\"eose\":%lu},"
    "\"drops\":{\"rate_limit\":%lu,\"backpressure\":%lu,\"duplicate\":%lu,\"skew\":%lu}}",
    M.connections_current, M.connections_total, M.connections_closed,
    M.subs_current, M.subs_started, M.subs_ended,
    M.events_streamed, M.eose_sent,
    M.rate_limit_drops, M.backpressure_drops, M.duplicate_drops, M.skew_rejects);
  if (n <= 0) return NULL;
  char *out = (char*)malloc((size_t)n + 1);
  if (!out) return NULL;
  memcpy(out, buf, (size_t)n+1);
  return out;
}
