#include "ipv6/queue.h"
#include "log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define MOD "ipv6/queue"
struct fipsd_ipv6_queue {
    GoChannel *chan;
    _Atomic uint64_t dropped;
    _Atomic bool closed;
};
fipsd_ipv6_queue *fipsd_ipv6_queue_create(size_t capacity)
{
    fipsd_ipv6_queue *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->chan = go_channel_create(capacity ? capacity : FIPSD_IPV6_HOST_QUEUE_DEFAULT);
    if (!q->chan) { free(q); return NULL; }
    return q;
}
void fipsd_ipv6_queue_destroy(fipsd_ipv6_queue *q)
{
    if (!q) return;
    void *v;
    while (go_channel_try_receive(q->chan, &v) == 0) free(v);
    go_channel_unref(q->chan);
    free(q);
}
int fipsd_ipv6_queue_send(fipsd_ipv6_queue *q, const uint8_t *bytes, size_t len)
{
    if (atomic_load_explicit(&q->closed, memory_order_acquire)) return -1;
    fipsd_ipv6_packet *p = malloc(sizeof *p + len);
    if (!p) return 0; /* as a drop: the reader is still there */
    p->len = len; if (len) memcpy(p->bytes, bytes, len);
    if (go_channel_try_send(q->chan, p) == 0) return 0;
    free(p);
    if (atomic_load_explicit(&q->closed, memory_order_acquire)) return -1;
    uint64_t dropped = atomic_fetch_add_explicit(&q->dropped, 1, memory_order_relaxed) + 1;
    /* Logged at the first drop and at each doubling, so a stall shows in the
     * log without a line per packet. */
    if ((dropped & (dropped - 1)) == 0)
        FIPSD_WARN(MOD, "Host not reading; dropping packets toward it (dropped=%llu)", (unsigned long long)dropped);
    return 0;
}
void fipsd_ipv6_queue_close(fipsd_ipv6_queue *q) { atomic_store_explicit(&q->closed, true, memory_order_release); }
void fipsd_ipv6_queue_open(fipsd_ipv6_queue *q)
{
    void *v;
    while (go_channel_try_receive(q->chan, &v) == 0) free(v);
    atomic_store_explicit(&q->closed, false, memory_order_release);
}
GoChannel *fipsd_ipv6_queue_channel(fipsd_ipv6_queue *q) { return q->chan; }
fipsd_ipv6_packet *fipsd_ipv6_queue_try_receive(fipsd_ipv6_queue *q)
{
    void *v = NULL;
    return go_channel_try_receive(q->chan, &v) == 0 ? v : NULL;
}
uint64_t fipsd_ipv6_queue_dropped(const fipsd_ipv6_queue *q) { return atomic_load_explicit(&q->dropped, memory_order_relaxed); }
const _Atomic uint64_t *fipsd_ipv6_queue_counter(const fipsd_ipv6_queue *q) { return &q->dropped; }
