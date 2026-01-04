/**
 * @file concurrency_testkit.h
 * @brief Concurrency testing harness for GNOSTR/libnostr
 * 
 * Provides event tracing, lifecycle tracking, and deterministic testing
 * for goroutines, channels, threads, and other concurrency primitives.
 * 
 * Usage:
 *   1. Call ctk_init() at test start
 *   2. Use CTK_TRACE() macros to log events
 *   3. Use ctk_register_*() to track resources
 *   4. Call ctk_shutdown() at test end
 *   5. Check ctk_verify_clean() for leaks
 */

#ifndef CONCURRENCY_TESTKIT_H
#define CONCURRENCY_TESTKIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

/** Maximum number of events in trace ring buffer */
#define CTK_TRACE_BUFFER_SIZE 10000

/** Maximum number of tracked resources per type */
#define CTK_MAX_TRACKED_RESOURCES 1000

/* ============================================================================
 * Event Types
 * ============================================================================ */

typedef enum {
    /* Goroutine lifecycle */
    CTK_EVENT_GOROUTINE_START,
    CTK_EVENT_GOROUTINE_EXIT,
    CTK_EVENT_GOROUTINE_YIELD,
    
    /* Thread lifecycle */
    CTK_EVENT_THREAD_CREATE,
    CTK_EVENT_THREAD_JOIN,
    CTK_EVENT_THREAD_EXIT,
    
    /* Channel operations */
    CTK_EVENT_CHANNEL_CREATE,
    CTK_EVENT_CHANNEL_SEND,
    CTK_EVENT_CHANNEL_RECV,
    CTK_EVENT_CHANNEL_CLOSE,
    CTK_EVENT_CHANNEL_FREE,
    
    /* Mutex operations */
    CTK_EVENT_MUTEX_LOCK,
    CTK_EVENT_MUTEX_UNLOCK,
    CTK_EVENT_MUTEX_TRYLOCK,
    
    /* Condition variable operations */
    CTK_EVENT_COND_WAIT,
    CTK_EVENT_COND_SIGNAL,
    CTK_EVENT_COND_BROADCAST,
    
    /* Task/Future operations */
    CTK_EVENT_TASK_SUBMIT,
    CTK_EVENT_TASK_START,
    CTK_EVENT_TASK_COMPLETE,
    CTK_EVENT_TASK_CANCEL,
    
    /* Shutdown/Cancellation */
    CTK_EVENT_SHUTDOWN_INIT,
    CTK_EVENT_SHUTDOWN_COMPLETE,
    CTK_EVENT_CANCEL_REQUEST,
    CTK_EVENT_CANCEL_PROPAGATE,
    
    /* Subscription lifecycle (GNOSTR-specific) */
    CTK_EVENT_SUB_CREATE,
    CTK_EVENT_SUB_FIRE,
    CTK_EVENT_SUB_CLOSE,
    CTK_EVENT_SUB_FREE,
    CTK_EVENT_SUB_CLEANUP_START,
    CTK_EVENT_SUB_CLEANUP_DONE,
    
    /* Errors */
    CTK_EVENT_ERROR,
    CTK_EVENT_ASSERTION_FAIL,
    
    CTK_EVENT_TYPE_COUNT
} ctk_event_type_t;

/* ============================================================================
 * Event Structure
 * ============================================================================ */

typedef struct {
    uint64_t timestamp_ns;      /**< Monotonic timestamp */
    ctk_event_type_t type;      /**< Event type */
    uint64_t goroutine_id;      /**< Goroutine ID (0 if N/A) */
    pthread_t thread_id;        /**< OS thread ID */
    uint64_t object_id;         /**< Object ID (channel, mutex, etc.) */
    uint32_t line;              /**< Source line number */
    const char *file;           /**< Source file name */
    char info[128];             /**< Additional info */
} ctk_event_t;

/* ============================================================================
 * Resource Tracking
 * ============================================================================ */

typedef enum {
    CTK_RESOURCE_GOROUTINE,
    CTK_RESOURCE_THREAD,
    CTK_RESOURCE_CHANNEL,
    CTK_RESOURCE_MUTEX,
    CTK_RESOURCE_CONDVAR,
    CTK_RESOURCE_TASK,
    CTK_RESOURCE_SUBSCRIPTION,
    CTK_RESOURCE_TYPE_COUNT
} ctk_resource_type_t;

typedef struct {
    uint64_t id;
    ctk_resource_type_t type;
    bool active;
    uint64_t create_timestamp_ns;
    uint64_t destroy_timestamp_ns;
    pthread_t owner_thread;
    char debug_name[64];
} ctk_resource_t;

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct {
    uint64_t total_events;
    uint64_t events_by_type[CTK_EVENT_TYPE_COUNT];
    uint64_t resources_created[CTK_RESOURCE_TYPE_COUNT];
    uint64_t resources_destroyed[CTK_RESOURCE_TYPE_COUNT];
    uint64_t resources_leaked[CTK_RESOURCE_TYPE_COUNT];
    uint64_t goroutines_max_concurrent;
    uint64_t threads_max_concurrent;
} ctk_stats_t;

/* ============================================================================
 * Configuration Flags
 * ============================================================================ */

typedef struct {
    bool enable_tracing;        /**< Enable event tracing (default: true) */
    bool enable_tracking;       /**< Enable resource tracking (default: true) */
    bool enable_stress;         /**< Enable stress mode (yields/delays) */
    bool enable_deterministic;  /**< Use fixed seed (default: true) */
    uint32_t seed;              /**< Random seed (0 = use time) */
    uint32_t stress_yield_prob; /**< Probability of yield in stress mode (0-100) */
    uint32_t stress_delay_us;   /**< Max delay in stress mode (microseconds) */
    size_t trace_buffer_size;   /**< Trace buffer size (default: CTK_TRACE_BUFFER_SIZE) */
} ctk_config_t;

/* ============================================================================
 * API Functions
 * ============================================================================ */

/**
 * Initialize the concurrency test kit.
 * Must be called before any other CTK functions.
 * 
 * @param config Configuration (NULL for defaults)
 * @return 0 on success, -1 on error
 */
int ctk_init(const ctk_config_t *config);

/**
 * Shutdown the test kit and free resources.
 * Call this at the end of each test.
 */
void ctk_shutdown(void);

/**
 * Reset all state (useful for running multiple tests in one process).
 */
void ctk_reset(void);

/**
 * Record a trace event.
 * Use CTK_TRACE() macro instead of calling directly.
 */
void ctk_trace_event(ctk_event_type_t type, uint64_t goroutine_id, 
                     uint64_t object_id, const char *file, uint32_t line,
                     const char *fmt, ...);

/**
 * Register a resource for lifecycle tracking.
 * 
 * @param type Resource type
 * @param id Unique ID for this resource
 * @param debug_name Optional debug name
 * @return 0 on success, -1 on error
 */
int ctk_register_resource(ctk_resource_type_t type, uint64_t id, const char *debug_name);

/**
 * Unregister a resource (marks it as destroyed).
 * 
 * @param type Resource type
 * @param id Resource ID
 * @return 0 on success, -1 if not found
 */
int ctk_unregister_resource(ctk_resource_type_t type, uint64_t id);

/**
 * Check if a resource is still active.
 */
bool ctk_is_resource_active(ctk_resource_type_t type, uint64_t id);

/**
 * Verify that all resources have been cleaned up.
 * 
 * @return true if clean, false if leaks detected
 */
bool ctk_verify_clean(void);

/**
 * Get current statistics.
 */
const ctk_stats_t *ctk_get_stats(void);

/**
 * Dump trace buffer to file or stderr.
 * 
 * @param filename File to write to (NULL for stderr)
 * @param last_n Number of events to dump (0 for all)
 */
void ctk_dump_trace(const char *filename, size_t last_n);

/**
 * Dump resource tracking state.
 */
void ctk_dump_resources(const char *filename);

/**
 * Get a deterministic random number (uses test seed).
 */
uint32_t ctk_rand(void);

/**
 * Inject a stress perturbation (yield or delay).
 * Only has effect if stress mode is enabled.
 */
void ctk_stress_point(void);

/**
 * Get current monotonic timestamp in nanoseconds.
 */
uint64_t ctk_timestamp_ns(void);

/**
 * Get current thread ID.
 */
pthread_t ctk_thread_id(void);

/**
 * Get event type name as string.
 */
const char *ctk_event_type_name(ctk_event_type_t type);

/**
 * Get resource type name as string.
 */
const char *ctk_resource_type_name(ctk_resource_type_t type);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#define CTK_TRACE(type, gid, oid, ...) \
    ctk_trace_event((type), (gid), (oid), __FILE__, __LINE__, __VA_ARGS__)

#define CTK_TRACE_GOROUTINE_START(gid) \
    CTK_TRACE(CTK_EVENT_GOROUTINE_START, (gid), 0, "goroutine %lu started", (unsigned long)(gid))

#define CTK_TRACE_GOROUTINE_EXIT(gid) \
    CTK_TRACE(CTK_EVENT_GOROUTINE_EXIT, (gid), 0, "goroutine %lu exited", (unsigned long)(gid))

#define CTK_TRACE_CHANNEL_SEND(gid, ch) \
    CTK_TRACE(CTK_EVENT_CHANNEL_SEND, (gid), (uint64_t)(uintptr_t)(ch), "send")

#define CTK_TRACE_CHANNEL_RECV(gid, ch) \
    CTK_TRACE(CTK_EVENT_CHANNEL_RECV, (gid), (uint64_t)(uintptr_t)(ch), "recv")

#define CTK_TRACE_SUB_CREATE(sub) \
    CTK_TRACE(CTK_EVENT_SUB_CREATE, 0, (uint64_t)(uintptr_t)(sub), "subscription created")

#define CTK_TRACE_SUB_FREE(sub) \
    CTK_TRACE(CTK_EVENT_SUB_FREE, 0, (uint64_t)(uintptr_t)(sub), "subscription freed")

#define CTK_ASSERT(cond, ...) \
    do { \
        if (!(cond)) { \
            CTK_TRACE(CTK_EVENT_ASSERTION_FAIL, 0, 0, __VA_ARGS__); \
            ctk_dump_trace(NULL, 100); \
            assert(cond); \
        } \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* CONCURRENCY_TESTKIT_H */
