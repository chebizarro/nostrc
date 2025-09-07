// No-op stubs to satisfy libgo linkage when libnostr is not linked.
#include <stddef.h>

typedef struct nostr_metric_histogram nostr_metric_histogram;
typedef struct { char opaque[16]; } nostr_metric_timer;

void nostr_metric_counter_add(const char *name, long delta) { (void)name; (void)delta; }
nostr_metric_histogram *nostr_metric_histogram_get(const char *name) { (void)name; return NULL; }
void nostr_metric_timer_start(nostr_metric_timer *t) { (void)t; }
void nostr_metric_timer_stop(nostr_metric_timer *t, nostr_metric_histogram *h) { (void)t; (void)h; }
