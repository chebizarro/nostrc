#ifndef FIPSD_NOSTR_SIGNAL_ROUTE_H
#define FIPSD_NOSTR_SIGNAL_ROUTE_H
#include "nostr/events.h"
#include "nostr/signal_gate.h"
#include <channel.h>
#include <pthread.h>
#define FIPSD_SIGNAL_BACKLOG 4096u
/* Middleware owns route(); actor receives owned NostrEvent pointers. Separate
 * replay memory and mailbox mean advert volume cannot evict queued signals. */
typedef struct {
    pthread_mutex_t mutex;
    GoChannel *signals;
    fipsd_signal_gate gate;
    fipsd_nostr_seen seen;
    uint64_t *expires;
    uint64_t window_ms, shed, duplicates, full;
    char recipient[65];
    bool awaiting;
} fipsd_signal_router;
bool fipsd_signal_router_init(fipsd_signal_router *, const char *recipient, uint64_t now_ms, uint64_t replay_ms);
void fipsd_signal_router_clear(fipsd_signal_router *); /* after producer/consumer join */
void fipsd_signal_router_awaiting(fipsd_signal_router *, bool);
/* Consumes event on ALL paths. Signature, size, expiry and target admission
 * precede queuing. No decryption under the lock. */
bool fipsd_signal_router_route(fipsd_signal_router *, NostrEvent *, uint64_t mono_ms, uint64_t wall_ms);
#endif
