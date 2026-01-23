/* memory-profile.h - Memory profiling and monitoring for gnostr-signer
 *
 * Provides memory usage tracking, leak detection, and statistics
 * for debug builds. Uses GLib memory profiling infrastructure.
 *
 * Features:
 * - Track allocations by component (accounts, sessions, UI, etc.)
 * - Log memory statistics periodically
 * - Detect potential leaks via allocation aging
 * - Integration with secure-mem for sensitive memory tracking
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GNOSTR_MEMORY_PROFILE_H
#define GNOSTR_MEMORY_PROFILE_H

#include <glib.h>
#include <glib-object.h>

G_BEGIN_DECLS

/* Memory component identifiers for tracking */
typedef enum {
  GN_MEM_COMPONENT_CORE,        /* Core application */
  GN_MEM_COMPONENT_ACCOUNTS,    /* Account storage */
  GN_MEM_COMPONENT_SECRETS,     /* Secret/key storage */
  GN_MEM_COMPONENT_SESSIONS,    /* Client sessions */
  GN_MEM_COMPONENT_POLICIES,    /* Permission policies */
  GN_MEM_COMPONENT_UI,          /* UI components */
  GN_MEM_COMPONENT_CACHE,       /* Caches (relay, profile, etc.) */
  GN_MEM_COMPONENT_SECURE,      /* Secure memory allocations */
  GN_MEM_COMPONENT_MAX
} GnMemComponent;

/* Memory statistics structure */
typedef struct {
  /* Per-component tracking */
  gsize component_bytes[GN_MEM_COMPONENT_MAX];
  guint component_count[GN_MEM_COMPONENT_MAX];

  /* Overall statistics */
  gsize total_allocated;
  gsize total_freed;
  gsize peak_usage;
  guint allocation_count;
  guint free_count;

  /* Secure memory stats */
  gsize secure_allocated;
  gsize secure_peak;
  gboolean secure_mlock_available;

  /* GObject tracking */
  guint gobjects_alive;
  guint gobjects_peak;

  /* Cache statistics */
  gsize cache_bytes;
  guint cache_entries;
  guint cache_hits;
  guint cache_misses;

  /* Timing */
  gint64 tracking_start;
  gint64 last_report;
} GnMemStats;

/* Initialize memory profiling (debug builds only) */
void gn_mem_profile_init(void);

/* Shutdown and report final statistics */
void gn_mem_profile_shutdown(void);

/* Track allocation for a specific component */
void gn_mem_profile_alloc(GnMemComponent component, gsize size);

/* Track deallocation for a specific component */
void gn_mem_profile_free(GnMemComponent component, gsize size);

/* Record a cache hit/miss */
void gn_mem_profile_cache_hit(void);
void gn_mem_profile_cache_miss(void);

/* Record cache entry add/remove */
void gn_mem_profile_cache_add(gsize bytes);
void gn_mem_profile_cache_remove(gsize bytes);

/* Get current memory statistics */
GnMemStats gn_mem_profile_get_stats(void);

/* Log current memory statistics (debug builds) */
void gn_mem_profile_log_stats(const gchar *context);

/* Check for potential leaks (allocations older than threshold) */
guint gn_mem_profile_check_leaks(guint age_threshold_seconds);

/* Force garbage collection on caches */
void gn_mem_profile_gc_caches(void);

/* Get component name string */
const gchar *gn_mem_component_name(GnMemComponent component);

/* Convenience macros for debug builds */
#ifndef NDEBUG
#define GN_MEM_ALLOC(component, size) gn_mem_profile_alloc(component, size)
#define GN_MEM_FREE(component, size) gn_mem_profile_free(component, size)
#define GN_MEM_LOG(context) gn_mem_profile_log_stats(context)
#define GN_CACHE_HIT() gn_mem_profile_cache_hit()
#define GN_CACHE_MISS() gn_mem_profile_cache_miss()
#else
#define GN_MEM_ALLOC(component, size) ((void)0)
#define GN_MEM_FREE(component, size) ((void)0)
#define GN_MEM_LOG(context) ((void)0)
#define GN_CACHE_HIT() ((void)0)
#define GN_CACHE_MISS() ((void)0)
#endif

G_END_DECLS

#endif /* GNOSTR_MEMORY_PROFILE_H */
