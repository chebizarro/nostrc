#ifndef GO_CHANNEL_H
#define GO_CHANNEL_H

#include "context.h"
#include "refptr.h"
#include <nsync.h>
#include <stdatomic.h>
#include <stddef.h> // offsetof
#include <stdint.h> // uint32_t

#ifndef NOSTR_CACHELINE
#define NOSTR_CACHELINE 64
#endif

// Enforce power-of-two capacity for channels by default to enable fast mask-based wraparound
#ifndef NOSTR_CHANNEL_ENFORCE_POW2_CAP
#define NOSTR_CHANNEL_ENFORCE_POW2_CAP 1
#endif

// Magic number to detect valid vs freed/garbage channel pointers
#define GO_CHANNEL_MAGIC 0xC4A77E10  // "CHANNEL0"

// Magic numbers for sync state lifecycle
#define GO_SYNC_MAGIC_ALIVE  0x5CA11FE0  // "SYNC ALIVE" (valid hex)
#define GO_SYNC_MAGIC_FREED  0xDEAD5C00  // "DEAD SYNC" (valid hex)

/* Forward declarations for select waiters */
struct GoSelectWaiter;
struct GoSelectWaiterNode;

/* Forward declaration for fiber waiter (used by fiber-aware CV_WAIT) */
struct GoFiberWaiter;

/**
 * Separately-allocated sync state for channels.
 * 
 * This struct holds the mutex and condition variables that waiters touch.
 * By allocating it separately from the channel, we can:
 * 1. Free the channel while sync state remains valid for late waiters
 * 2. Implement proper refcounting so sync state lives until all waiters exit
 * 3. Diagnose UAF by making sync state immortal (NOSTR_CHAN_SYNC_NEVER_FREE=1)
 */
typedef struct GoChanSyncState {
    uint32_t magic;                    // Validation magic
    _Atomic int refcnt;                // Reference count (channel + active waiters)
    _Atomic int waiter_count;          // Number of threads currently blocked in wait
    nsync_mu mu;                       // Mutex for channel operations
    nsync_cv cv_full;                  // Condition: buffer has space (senders wait here)
    nsync_cv cv_empty;                 // Condition: buffer has data (receivers wait here)
} GoChanSyncState;

typedef struct GoChannel {
    // Magic number for validation (must be first field)
    uint32_t magic;
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
    
    // Pointer to separately-allocated sync state (survives channel free)
    _Alignas(NOSTR_CACHELINE) GoChanSyncState *sync;
    
    // Reference count for shared ownership (hq-e3ach). Starts at 1.
    // When refs drops to 0 the channel is destroyed.
    _Atomic int refs;
    // Active waiters count (nostrc-waiter-count). Tracks threads currently
    // blocked inside go_channel_send/receive. Channel cannot be freed until
    // this reaches 0, preventing use-after-free in nsync_cv_wait.
    _Atomic int active_waiters;
    // Linked list of select waiter registrations (protected by sync->mu)
    struct GoSelectWaiterNode *select_waiters;
    // Linked list of fiber waiters on cv_full (protected by sync->mu)
    struct GoFiberWaiter *fiber_waiters_full;
    // Linked list of fiber waiters on cv_empty (protected by sync->mu)
    struct GoFiberWaiter *fiber_waiters_empty;
} __attribute__((aligned(NOSTR_CACHELINE))) GoChannel;

/* Sync state lifecycle functions */
GoChanSyncState *go_chan_sync_create(void);
GoChanSyncState *go_chan_sync_ref(GoChanSyncState *sync);
void go_chan_sync_unref(GoChanSyncState *sync);

/* Waiter count helpers - call before/after blocking */
static inline void go_chan_sync_waiter_enter(GoChanSyncState *sync) {
    atomic_fetch_add_explicit(&sync->waiter_count, 1, memory_order_acq_rel);
}
static inline void go_chan_sync_waiter_exit(GoChanSyncState *sync) {
    atomic_fetch_sub_explicit(&sync->waiter_count, 1, memory_order_acq_rel);
}

// Compile-time checks to ensure hot fields start at cacheline boundaries.
// This helps avoid false sharing between producers/consumers and sync vars.
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert((offsetof(GoChannel, in)   % NOSTR_CACHELINE) == 0,  "GoChannel.in not cacheline-aligned");
_Static_assert((offsetof(GoChannel, out)  % NOSTR_CACHELINE) == 0,  "GoChannel.out not cacheline-aligned");
_Static_assert((offsetof(GoChannel, size) % NOSTR_CACHELINE) == 0,  "GoChannel.size not cacheline-aligned");
_Static_assert((offsetof(GoChannel, sync) % NOSTR_CACHELINE) == 0,  "GoChannel.sync not cacheline-aligned");
#endif

GoChannel *go_channel_create(size_t capacity);
GoChannel *go_channel_ref(GoChannel *chan);
void go_channel_unref(GoChannel *chan);
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
// Get current channel depth (number of items in buffer)
size_t go_channel_get_depth(GoChannel *chan);

#endif // GO_CHANNEL_H
