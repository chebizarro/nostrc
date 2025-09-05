/**
 * @file fiber.h
 * @brief Public API for the libgo fiber runtime (lightweight cooperative threads).
 *
 * The fiber scheduler provides cooperative multitasking with a work-stealing runtime,
 * lightweight synchronization primitives, and an integrated I/O netpoller. See
 * docs in `docs/fiber/` for architecture details and usage examples.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h> /* for struct sockaddr, socklen_t */
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
void        gof_init(size_t default_stack_bytes);

/**
 * @brief Spawn a new fiber.
 * @param fn Entry function to run on the new fiber.
 * @param arg User argument passed to the entry function.
 * @param stack_bytes Stack size in bytes; if 0, uses the default configured in gof_init.
 * @return Pointer to the newly created fiber handle, or NULL on failure.
 */
gof_fiber_t*gof_spawn(gof_fn fn, void *arg, size_t stack_bytes);

/**
 * @brief Cooperatively yield the CPU to allow other fibers to run.
 */
void        gof_yield(void);

/**
 * @brief Sleep the current fiber for at least the specified duration in milliseconds.
 * @param ms Milliseconds to sleep.
 */
void        gof_sleep_ms(uint64_t ms);

/**
 * @brief Enter the scheduler loop. Typically called once on the main thread.
 */
void        gof_run(void);

/** Fiber-friendly I/O wrappers that integrate with the netpoller. */
ssize_t     gof_read(int fd, void *buf, size_t n);
ssize_t     gof_write(int fd, const void *buf, size_t n);
int         gof_connect(int fd, const struct sockaddr *sa, socklen_t slen, int timeout_ms);
int         gof_accept(int fd, struct sockaddr *sa, socklen_t *slen, int timeout_ms);

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
size_t      gof_list(gof_info *out, size_t max);

/** Dump stack traces of all fibers to the given file descriptor (best-effort). */
void        gof_dump_stacks(int fd);

/** Set a human-readable name for the current fiber (for diagnostics and tracing). */
void        gof_set_name(const char *name);

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
void        gof_get_stats(gof_sched_stats *out);

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
void gof_set_steal_params(const gof_steal_params *p);
void gof_get_steal_params(gof_steal_params *out);

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
void gof_set_rebalance_params(const gof_rebalance_params *p);
void gof_get_rebalance_params(gof_rebalance_params *out);

/** Toggle affinity routing (sticky connections) at runtime. */
void gof_set_affinity_enabled(int enable);
int  gof_get_affinity_enabled(void);

/**
 * @brief Runtime configuration of netpollers (must be set before scheduler init).
 * @return 0 on success, -1 if called after scheduler initialization or invalid n.
 */
int  gof_set_npollers(int n);
/** Returns the configured number of pollers (>=1). */
int  gof_get_npollers(void);

/** Weak trace hooks (no-op by default); attributes best-effort across compilers. */
#if defined(__GNUC__) || defined(__clang__)
void __attribute__((weak)) gof_trace_on_switch(uint64_t old_id, uint64_t new_id);
void __attribute__((weak)) gof_trace_on_block(int fd, int ev);
void __attribute__((weak)) gof_trace_on_unblock(int fd, int ev);
#else
void gof_trace_on_switch(uint64_t old_id, uint64_t new_id);
void gof_trace_on_block(int fd, int ev);
void gof_trace_on_unblock(int fd, int ev);
#endif

#ifdef __cplusplus
}
#endif
