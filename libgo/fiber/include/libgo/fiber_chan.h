/**
 * @file fiber_chan.h
 * @brief Bounded MPMC channels for pointer-sized messages between fibers.
 */
#ifndef LIBGO_FIBER_LIBGO_FIBER_CHAN_H
#define LIBGO_FIBER_LIBGO_FIBER_CHAN_H

#include "libgo/fiber-export.h"
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
GO_FIBER_API gof_chan_t* gof_chan_make(size_t capacity);

/**
 * @brief Close a channel, waking any blocked senders/receivers.
 * Closing is idempotent. Subsequent send/recv act as on a closed channel.
 */
GO_FIBER_API void        gof_chan_close(gof_chan_t* c);

/** Blocking cooperative send/recv of a single pointer-sized value. */
GO_FIBER_API int         gof_chan_send(gof_chan_t* c, void* value);
GO_FIBER_API int         gof_chan_recv(gof_chan_t* c, void** out_value);

/** Non-blocking try variants: return 1 on success, 0 if would block, <0 on closed. */
GO_FIBER_API int         gof_chan_try_send(gof_chan_t* c, void* value);
GO_FIBER_API int         gof_chan_try_recv(gof_chan_t* c, void** out_value);

#ifdef __cplusplus
}
#endif
#endif /* LIBGO_FIBER_LIBGO_FIBER_CHAN_H */
