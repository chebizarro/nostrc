// No-op stubs to satisfy libgo linkage when libnostr is not linked.
//
// They are weak so libnostr's metrics.c wins whenever both archives are
// linked. With strong stubs GNU ld fails with "multiple definition" once it
// has pulled this object for libgo's own references and later pulls
// metrics.c.o for another libnostr metrics symbol (gnostr, gnostr-live-log);
// Apple's ld64 does not report that, so only Linux builds broke.
#include <stddef.h>
#include "libgo-export.h"
/* These weak definitions implement the public metrics hook ABI. Keep them
 * preemptible by libnostr's strong definitions in shared builds too. */
#include "nostr/metrics.h"

#if defined(__GNUC__) || defined(__clang__)
#define GO_METRICS_STUB __attribute__((weak))
#else
#define GO_METRICS_STUB
#endif

GO_METRICS_STUB void nostr_metric_counter_add(const char *name, uint64_t delta) { (void)name; (void)delta; }
GO_METRICS_STUB nostr_metric_histogram *nostr_metric_histogram_get(const char *name) { (void)name; return NULL; }
GO_METRICS_STUB void nostr_metric_timer_start(nostr_metric_timer *t) { (void)t; }
GO_METRICS_STUB void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h) { (void)t; (void)h; }
