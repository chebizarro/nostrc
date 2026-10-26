/**
 * @file fiber.h
 * @brief Public API for the libgo fiber runtime (lightweight cooperative threads).
 *
 * The fiber scheduler provides cooperative multitasking with a work-stealing runtime,
 * lightweight synchronization primitives, and an integrated I/O netpoller. See
 * docs in `docs/fiber/` for architecture details and usage examples.
 */
#ifndef LIBGO_FIBER_LIBGO_FIBER_H
#define LIBGO_FIBER_LIBGO_FIBER_H

#include "libgo/fiber-export.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#if defined(_WIN32) || defined(__MINGW32__)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h> /* for struct sockaddr, socklen_t */
#endif
#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to a fiber. */
typedef struct gof_fiber gof_fiber_t;

/** Function signature for a fiber entrypoint. */
typedef void (*gof_fn)(void *arg);

/**
 * @brief Initialize the fiber runtime and worker threads.
 * @param default_stack_bytes Default stack size (bytes) for fibers spawned with stack_bytes=0.
 */
GO_FIBER_API void        gof_init(size_t default_stack_bytes);

/**
 * @brief Spawn a new fiber.
 * @param fn Entry function to run on the new fiber.
 * @param arg User argument passed to the entry function.
 * @param stack_bytes Stack size in bytes; if 0, uses the default configured in gof_init.
 * @return Pointer to the newly created fiber handle, or NULL on failure.
 */
GO_FIBER_API gof_fiber_t*gof_spawn(gof_fn fn, void *arg, size_t stack_bytes);

/**
 * @brief Cooperatively yield the CPU to allow other fibers to run.
 */
GO_FIBER_API void        gof_yield(void);

/**
 * @brief Sleep the current fiber for at least the specified duration in milliseconds.
 * @param ms Milliseconds to sleep.
 */
GO_FIBER_API void        gof_sleep_ms(uint64_t ms);

/**
 * @brief Enter the scheduler loop. Typically called once on the main thread.
 * @note This blocks the calling thread until all fibers complete.
 *       For applications with their own event loop (e.g., GTK/GLib),
 *       use gof_start_background() instead.
 */
GO_FIBER_API void        gof_run(void);

/**
 * @brief Start the fiber scheduler on background threads and return immediately.
 *
 * Unlike gof_run() which takes over the calling thread, this spawns the
 * scheduler worker threads in the background, allowing the calling thread
 * to continue (e.g., to run a GTK main loop).
 *
 * The scheduler stays alive until gof_request_stop() is called or all fibers
 * complete and no I/O waiters remain.
 *
 * @param default_stack_bytes Default stack size for fibers (0 = use default 256KB).
 * @return 0 on success, -1 on error (e.g., already running).
 */
GO_FIBER_API int         gof_start_background(size_t default_stack_bytes);

/**
 * @brief Request the fiber scheduler to stop.
 *
 * Wakes all workers so they can observe the stop flag and exit.
 * Does not wait for completion — use gof_join_background() to wait.
 * Safe to call from any thread (including GTK main thread).
 */
GO_FIBER_API void        gof_request_stop(void);

/**
 * @brief Wait for all background scheduler threads to finish.
 *
 * Blocks until all worker threads have exited. Call after gof_request_stop()
 * or after all fibers have naturally completed.
 *
 * @return 0 on success, -1 if not running in background mode.
 */
GO_FIBER_API int         gof_join_background(void);

/**
 * @brief Check whether the current thread is a fiber scheduler worker.
 * @return 1 if called from within a fiber or worker thread, 0 otherwise.
 */
GO_FIBER_API int         gof_in_fiber(void);

/**
 * @brief Get the current fiber handle, if running inside a fiber.
 * @return Current fiber handle, or NULL if not in a fiber.
 */
GO_FIBER_API gof_fiber_t*gof_current(void);

/** Fiber-friendly I/O wrappers that integrate with the netpoller. */
GO_FIBER_API ssize_t     gof_read(int fd, void *buf, size_t n);
GO_FIBER_API ssize_t     gof_write(int fd, const void *buf, size_t n);
GO_FIBER_API int         gof_connect(int fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms);
GO_FIBER_API int         gof_accept(int fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms);

/**
 * @brief Snapshot of fiber diagnostic information.
 */
typedef struct {
  uint64_t   id;
  const char*name;
  size_t     stack_size;
  size_t     stack_used;
  int        state;
  uint64_t   last_run_ns;
} gof_info;

/**
 * @brief List currently known fibers.
 * @param out Buffer to receive up to @p max entries. May be NULL to query count.
 * @param max Maximum number of entries that fit in @p out.
 * @return Number of entries written if @p out != NULL, otherwise total number of fibers.
 */
GO_FIBER_API size_t      gof_list(gof_info *out, size_t max);

/** Dump stack traces of all fibers to the given file descriptor (best-effort). */
GO_FIBER_API void        gof_dump_stacks(int fd);

/** Set a human-readable name for the current fiber (for diagnostics and tracing). */
GO_FIBER_API void        gof_set_name(const char *name);

/* Scheduler statistics */
/**
 * @brief Scheduler statistics (monotonic counters and current configuration).
 */
typedef struct {
  int        nworkers;
  int        enable_steal;
  int        affinity_enable;
  int        steal_min_live;
  int        steal_min_victim;
  uint64_t   live_fibers;
  uint64_t   steals_attempted;
  uint64_t   steals_success;
  uint64_t   inject_enqueues;
  uint64_t   inject_drains;
  /* Rebalancer */
  int        rebalance_enable;
  int        rebalance_threshold;
  int        rebalance_interval_ms;
  uint64_t   rebalances_attempted;
  uint64_t   rebalances_migrated;
} gof_sched_stats;

/** Retrieve current scheduler statistics (thread-safe snapshot). */
GO_FIBER_API void        gof_get_stats(gof_sched_stats *out);

/* Runtime configuration for work-stealing */
/**
 * @brief Work-stealing runtime parameters.
 */
typedef struct {
  int enable_steal;    /* 0/1 */
  int steal_min_live;  /* >=0 */
  int steal_min_victim;/* >=2 */
} gof_steal_params;

/** Set and get work-stealing parameters at runtime. Environment values are used as defaults on init. */
GO_FIBER_API void gof_set_steal_params(const gof_steal_params *p);
GO_FIBER_API void gof_get_steal_params(gof_steal_params *out);

/* Runtime configuration for periodic rebalancing */
/**
 * @brief Periodic rebalancer parameters.
 */
typedef struct {
  int enable;          /* 0/1 */
  int threshold;       /* migrate when (max_len - min_len) >= threshold */
  int interval_ms;     /* how often to attempt rebalancing */
} gof_rebalance_params;

/** Set and get rebalancer parameters at runtime. Env values are used as defaults on init. */
GO_FIBER_API void gof_set_rebalance_params(const gof_rebalance_params *p);
GO_FIBER_API void gof_get_rebalance_params(gof_rebalance_params *out);

/** Toggle affinity routing (sticky connections) at runtime. */
GO_FIBER_API void gof_set_affinity_enabled(int enable);
GO_FIBER_API int  gof_get_affinity_enabled(void);

/**
 * @brief Runtime configuration of netpollers (must be set before scheduler init).
 * @return 0 on success, -1 if called after scheduler initialization or invalid n.
 */
GO_FIBER_API int  gof_set_npollers(int n);
/** Returns the configured number of pollers (>=1). */
GO_FIBER_API int  gof_get_npollers(void);

/** Weak trace hooks (no-op by default); attributes best-effort across compilers. */
#if defined(__GNUC__) || defined(__clang__)
GO_FIBER_API void __attribute__((weak)) gof_trace_on_switch(uint64_t old_id, uint64_t new_id);
GO_FIBER_API void __attribute__((weak)) gof_trace_on_block(int fd, int ev);
GO_FIBER_API void __attribute__((weak)) gof_trace_on_unblock(int fd, int ev);
#else
GO_FIBER_API void gof_trace_on_switch(uint64_t old_id, uint64_t new_id);
GO_FIBER_API void gof_trace_on_block(int fd, int ev);
GO_FIBER_API void gof_trace_on_unblock(int fd, int ev);
#endif

#ifdef __cplusplus
}
#endif
#endif /* LIBGO_FIBER_LIBGO_FIBER_H */
