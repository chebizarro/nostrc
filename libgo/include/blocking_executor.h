/**
 * @file blocking_executor.h
 * @brief Bounded thread pool for offloading blocking I/O from fibers.
 *
 * When code running on a fiber needs to perform a blocking operation
 * (e.g., NDB/LMDB transactions, synchronous file I/O), calling the
 * blocking function directly would stall the fiber scheduler's worker
 * thread, reducing parallelism. The blocking executor solves this by:
 *
 *   1. Submitting the blocking work to a dedicated OS thread pool
 *   2. Parking the calling fiber (freeing the worker for other fibers)
 *   3. Waking the fiber when the blocking work completes
 *
 * When called from a non-fiber context (regular pthread), the function
 * is executed synchronously in the calling thread (zero overhead).
 *
 * Usage:
 *   // Initialize once at startup (e.g., 4 blocking threads)
 *   go_blocking_executor_init(4);
 *
 *   // From a fiber:
 *   void *result = go_blocking_submit(my_ndb_query, query_arg);
 *
 *   // Shutdown at cleanup
 *   go_blocking_executor_shutdown();
 *
 * Inspired by Tokio's spawn_blocking() and Go's runtime.LockOSThread().
 */
#ifndef LIBGO_BLOCKING_EXECUTOR_H
#define LIBGO_BLOCKING_EXECUTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the blocking executor thread pool.
 *
 * @param num_threads Number of OS threads in the pool (0 = default: 4).
 * @return 0 on success, -1 on failure.
 *
 * Safe to call multiple times (subsequent calls are no-ops).
 * Thread-safe.
 */
int go_blocking_executor_init(size_t num_threads);

/**
 * @brief Submit a blocking function to the executor.
 *
 * If called from a fiber context:
 *   - Enqueues the work to the thread pool
 *   - Parks the calling fiber
 *   - Wakes the fiber when work completes
 *   - Returns the result of fn(arg)
 *
 * If called from a non-fiber context (regular pthread):
 *   - Executes fn(arg) synchronously in the calling thread
 *   - Returns the result directly
 *
 * @param fn  Blocking function to execute.
 * @param arg Argument passed to fn.
 * @return The return value of fn(arg).
 */
void *go_blocking_submit(void *(*fn)(void *arg), void *arg);

/**
 * @brief Shut down the blocking executor.
 *
 * Signals all worker threads to exit and joins them. Any work items
 * still in the queue will be completed before shutdown.
 *
 * Safe to call multiple times.
 */
void go_blocking_executor_shutdown(void);

/**
 * @brief Get the number of currently active (busy) executor threads.
 *
 * Useful for metrics and debugging.
 */
int go_blocking_executor_active_count(void);

/**
 * @brief Get the number of pending (queued but not started) work items.
 */
int go_blocking_executor_pending_count(void);

#ifdef __cplusplus
}
#endif
#endif /* LIBGO_BLOCKING_EXECUTOR_H */
