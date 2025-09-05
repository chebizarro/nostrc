/**
 * @file fiber_chan.h
 * @brief Bounded MPMC channels for pointer-sized messages between fibers.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/** Opaque channel handle. */
typedef struct gof_chan gof_chan_t;

/**
 * @brief Create a channel with the given capacity.
 * @param capacity Number of buffered slots (0 for unbuffered rendezvous).
 * @return New channel handle or NULL on allocation failure.
 */
gof_chan_t* gof_chan_make(size_t capacity);

/**
 * @brief Close a channel, waking any blocked senders/receivers.
 * Closing is idempotent. Subsequent send/recv act as on a closed channel.
 */
void        gof_chan_close(gof_chan_t* c);

/** Blocking cooperative send/recv of a single pointer-sized value. */
int         gof_chan_send(gof_chan_t* c, void* value);
int         gof_chan_recv(gof_chan_t* c, void** out_value);

/** Non-blocking try variants: return 1 on success, 0 if would block, <0 on closed. */
int         gof_chan_try_send(gof_chan_t* c, void* value);
int         gof_chan_try_recv(gof_chan_t* c, void** out_value);

#ifdef __cplusplus
}
#endif
