// No-op stubs to satisfy libgo linkage when libnostr is not linked.
//
// They are weak so libnostr's metrics.c wins whenever both archives are
// linked. With strong stubs GNU ld fails with "multiple definition" once it
// has pulled this object for libgo's own references and later pulls
// metrics.c.o for another libnostr metrics symbol (gnostr, gnostr-live-log);
// Apple's ld64 does not report that, so only Linux builds broke.
#include <stddef.h>

#if defined(__GNUC__) || defined(__clang__)
#define GO_METRICS_STUB __attribute__((weak))
#else
#define GO_METRICS_STUB
#endif

typedef struct nostr_metric_histogram nostr_metric_histogram;
typedef struct { char opaque[16]; } nostr_metric_timer;

GO_METRICS_STUB void nostr_metric_counter_add(const char *name, long delta) { (void)name; (void)delta; }
GO_METRICS_STUB nostr_metric_histogram *nostr_metric_histogram_get(const char *name) { (void)name; return NULL; }
GO_METRICS_STUB void nostr_metric_timer_start(nostr_metric_timer *t) { (void)t; }
GO_METRICS_STUB void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h) { (void)t; (void)h; }
