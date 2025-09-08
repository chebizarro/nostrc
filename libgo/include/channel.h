#ifndef GO_CHANNEL_H
#define GO_CHANNEL_H

#include "context.h"
#include "refptr.h"
#include <nsync.h>
#include <stdatomic.h>
#include <stddef.h> // offsetof

#ifndef NOSTR_CACHELINE
#define NOSTR_CACHELINE 64
#endif

// Enforce power-of-two capacity for channels by default to enable fast mask-based wraparound
#ifndef NOSTR_CHANNEL_ENFORCE_POW2_CAP
#define NOSTR_CHANNEL_ENFORCE_POW2_CAP 1
#endif

typedef struct GoChannel {
    // Immutable after create
    _Atomic(void*) *buffer;
    size_t capacity;
    // Optional per-slot sequence numbers for MPMC lock-free try paths
    _Atomic size_t *slot_seq;
    // Mask for fast wrap (capacity is enforced to power-of-two)
    size_t mask;

    // Separate hot fields across cache lines to reduce false sharing.
    // Use C11 alignment to ensure cacheline boundaries irrespective of prior fields.
    _Alignas(NOSTR_CACHELINE) _Atomic size_t in;   // Producer index (writers: senders)
    _Alignas(NOSTR_CACHELINE) _Atomic size_t out;  // Consumer index (writers: receivers)
    _Alignas(NOSTR_CACHELINE) size_t size;         // Occupancy (if maintained)
    _Alignas(NOSTR_CACHELINE) _Atomic int closed;  // Closed flag (atomic)
    _Alignas(NOSTR_CACHELINE) nsync_mu mutex;      // Mutex separated from hot counters
    nsync_cv cond_full;
    nsync_cv cond_empty;
    // Double-free guard: set to 1 when freed. Placed at end to avoid shifting aligned fields.
    _Atomic int freed;
} __attribute__((aligned(NOSTR_CACHELINE))) GoChannel;

// Compile-time checks to ensure hot fields start at cacheline boundaries.
// This helps avoid false sharing between producers/consumers and sync vars.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert((offsetof(GoChannel, in)   % NOSTR_CACHELINE) == 0,  "GoChannel.in not cacheline-aligned");
_Static_assert((offsetof(GoChannel, out)  % NOSTR_CACHELINE) == 0,  "GoChannel.out not cacheline-aligned");
_Static_assert((offsetof(GoChannel, size) % NOSTR_CACHELINE) == 0,  "GoChannel.size not cacheline-aligned");
_Static_assert((offsetof(GoChannel, mutex) % NOSTR_CACHELINE) == 0, "GoChannel.mutex not cacheline-aligned");
#endif

GoChannel *go_channel_create(size_t capacity);
void go_channel_free(GoChannel *chan);
int go_channel_send(GoChannel *chan, void *data);
int go_channel_has_space(const void *chan);
int go_channel_has_data(const void *chan);
int go_channel_receive(GoChannel *chan, void **data);
// Non-blocking operations: return 0 on success, -1 if would block/closed
int go_channel_try_send(GoChannel *chan, void *data);
int go_channel_try_receive(GoChannel *chan, void **data);
int go_channel_is_closed(GoChannel *chan);
int go_channel_send_with_context(GoChannel *chan, void *data, GoContext *ctx);
int go_channel_receive_with_context(GoChannel *chan, void **data, GoContext *ctx);
void go_channel_close(GoChannel *chan);

#endif // GO_CHANNEL_H
