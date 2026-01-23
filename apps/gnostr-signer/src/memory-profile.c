/* memory-profile.c - Memory profiling implementation
 *
 * Implements memory tracking and statistics for debug builds.
 * Integrates with GLib and the secure-mem module.
 *
 * SPDX-License-Identifier: MIT
 */
#include "memory-profile.h"
#include "secure-mem.h"
#include <string.h>

/* Module state */
static struct {
  gboolean initialized;
  GMutex lock;
  GnMemStats stats;
  GHashTable *allocations;  /* Track individual allocations for leak detection */
  guint report_timer_id;
  guint gc_timer_id;
} state = { FALSE };

/* Allocation tracking entry */
typedef struct {
  gsize size;
  GnMemComponent component;
  gint64 timestamp;
} AllocEntry;

/* Report interval: 60 seconds for debug builds */
#define REPORT_INTERVAL_SEC 60

/* GC interval: 5 minutes */
#define GC_INTERVAL_SEC 300

/* Component name strings */
static const gchar *component_names[] = {
  "core",
  "accounts",
  "secrets",
  "sessions",
  "policies",
  "ui",
  "cache",
  "secure"
};

const gchar *
gn_mem_component_name(GnMemComponent component)
{
  if (component >= GN_MEM_COMPONENT_MAX) {
    return "unknown";
  }
  return component_names[component];
}

/* Periodic report callback */
static gboolean
periodic_report_cb(gpointer user_data)
{
  (void)user_data;
#ifndef NDEBUG
  gn_mem_profile_log_stats("periodic");
#endif
  return G_SOURCE_CONTINUE;
}

/* Periodic GC callback */
static gboolean
periodic_gc_cb(gpointer user_data)
{
  (void)user_data;
  gn_mem_profile_gc_caches();
  return G_SOURCE_CONTINUE;
}

void
gn_mem_profile_init(void)
{
  if (state.initialized) {
    return;
  }

  g_mutex_init(&state.lock);
  memset(&state.stats, 0, sizeof(state.stats));
  state.stats.tracking_start = g_get_monotonic_time();
  state.stats.last_report = state.stats.tracking_start;

  state.allocations = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                            NULL, g_free);

  state.initialized = TRUE;

  /* Get initial secure memory stats */
  GnostrSecureMemStats secure_stats = gnostr_secure_mem_get_stats();
  state.stats.secure_mlock_available = secure_stats.mlock_available;

#ifndef NDEBUG
  /* Start periodic reporting in debug builds */
  state.report_timer_id = g_timeout_add_seconds(REPORT_INTERVAL_SEC,
                                                 periodic_report_cb, NULL);

  /* Start periodic GC */
  state.gc_timer_id = g_timeout_add_seconds(GC_INTERVAL_SEC,
                                            periodic_gc_cb, NULL);

  g_debug("mem-profile: Memory profiling initialized");
#endif
}

void
gn_mem_profile_shutdown(void)
{
  if (!state.initialized) {
    return;
  }

#ifndef NDEBUG
  /* Final report */
  gn_mem_profile_log_stats("shutdown");

  /* Check for leaks */
  guint leaks = gn_mem_profile_check_leaks(0);
  if (leaks > 0) {
    g_warning("mem-profile: %u potential memory leaks detected at shutdown", leaks);
  }
#endif

  /* Cancel timers */
  if (state.report_timer_id > 0) {
    g_source_remove(state.report_timer_id);
    state.report_timer_id = 0;
  }

  if (state.gc_timer_id > 0) {
    g_source_remove(state.gc_timer_id);
    state.gc_timer_id = 0;
  }

  g_mutex_lock(&state.lock);

  if (state.allocations) {
    g_hash_table_destroy(state.allocations);
    state.allocations = NULL;
  }

  g_mutex_unlock(&state.lock);
  g_mutex_clear(&state.lock);

  state.initialized = FALSE;
}

void
gn_mem_profile_alloc(GnMemComponent component, gsize size)
{
  if (!state.initialized || component >= GN_MEM_COMPONENT_MAX) {
    return;
  }

  g_mutex_lock(&state.lock);

  state.stats.component_bytes[component] += size;
  state.stats.component_count[component]++;
  state.stats.total_allocated += size;
  state.stats.allocation_count++;

  gsize current_usage = state.stats.total_allocated - state.stats.total_freed;
  if (current_usage > state.stats.peak_usage) {
    state.stats.peak_usage = current_usage;
  }

  g_mutex_unlock(&state.lock);
}

void
gn_mem_profile_free(GnMemComponent component, gsize size)
{
  if (!state.initialized || component >= GN_MEM_COMPONENT_MAX) {
    return;
  }

  g_mutex_lock(&state.lock);

  if (state.stats.component_bytes[component] >= size) {
    state.stats.component_bytes[component] -= size;
  }
  if (state.stats.component_count[component] > 0) {
    state.stats.component_count[component]--;
  }
  state.stats.total_freed += size;
  state.stats.free_count++;

  g_mutex_unlock(&state.lock);
}

void
gn_mem_profile_cache_hit(void)
{
  if (!state.initialized) return;
  g_atomic_int_inc((gint*)&state.stats.cache_hits);
}

void
gn_mem_profile_cache_miss(void)
{
  if (!state.initialized) return;
  g_atomic_int_inc((gint*)&state.stats.cache_misses);
}

void
gn_mem_profile_cache_add(gsize bytes)
{
  if (!state.initialized) return;

  g_mutex_lock(&state.lock);
  state.stats.cache_bytes += bytes;
  state.stats.cache_entries++;
  g_mutex_unlock(&state.lock);

  gn_mem_profile_alloc(GN_MEM_COMPONENT_CACHE, bytes);
}

void
gn_mem_profile_cache_remove(gsize bytes)
{
  if (!state.initialized) return;

  g_mutex_lock(&state.lock);
  if (state.stats.cache_bytes >= bytes) {
    state.stats.cache_bytes -= bytes;
  }
  if (state.stats.cache_entries > 0) {
    state.stats.cache_entries--;
  }
  g_mutex_unlock(&state.lock);

  gn_mem_profile_free(GN_MEM_COMPONENT_CACHE, bytes);
}

GnMemStats
gn_mem_profile_get_stats(void)
{
  GnMemStats stats;

  if (!state.initialized) {
    memset(&stats, 0, sizeof(stats));
    return stats;
  }

  g_mutex_lock(&state.lock);
  stats = state.stats;

  /* Update secure memory stats */
  GnostrSecureMemStats secure_stats = gnostr_secure_mem_get_stats();
  stats.secure_allocated = secure_stats.total_allocated;
  stats.secure_peak = secure_stats.peak_allocated;

  g_mutex_unlock(&state.lock);

  return stats;
}

void
gn_mem_profile_log_stats(const gchar *context)
{
#ifndef NDEBUG
  if (!state.initialized) return;

  GnMemStats stats = gn_mem_profile_get_stats();

  gsize current_usage = stats.total_allocated - stats.total_freed;
  gint64 elapsed_sec = (g_get_monotonic_time() - stats.tracking_start) / G_USEC_PER_SEC;

  g_message("mem-profile [%s]: current=%zu peak=%zu allocs=%u frees=%u (elapsed: %llds)",
            context ? context : "unknown",
            current_usage,
            stats.peak_usage,
            stats.allocation_count,
            stats.free_count,
            (long long)elapsed_sec);

  /* Log per-component breakdown */
  g_debug("mem-profile: Component breakdown:");
  for (int i = 0; i < GN_MEM_COMPONENT_MAX; i++) {
    if (stats.component_bytes[i] > 0 || stats.component_count[i] > 0) {
      g_debug("  %s: %zu bytes, %u allocations",
              gn_mem_component_name(i),
              stats.component_bytes[i],
              stats.component_count[i]);
    }
  }

  /* Cache statistics */
  guint total_cache_ops = stats.cache_hits + stats.cache_misses;
  gdouble hit_rate = total_cache_ops > 0 ?
                     (gdouble)stats.cache_hits / total_cache_ops * 100.0 : 0.0;
  g_debug("mem-profile: Cache: %zu bytes, %u entries, %.1f%% hit rate (%u/%u)",
          stats.cache_bytes, stats.cache_entries,
          hit_rate, stats.cache_hits, total_cache_ops);

  /* Secure memory stats */
  g_debug("mem-profile: Secure memory: %zu bytes (peak %zu), mlock %s",
          stats.secure_allocated, stats.secure_peak,
          stats.secure_mlock_available ? "available" : "unavailable");

  state.stats.last_report = g_get_monotonic_time();
#else
  (void)context;
#endif
}

guint
gn_mem_profile_check_leaks(guint age_threshold_seconds)
{
  if (!state.initialized) return 0;

  /* For now, just report based on allocation balance */
  GnMemStats stats = gn_mem_profile_get_stats();

  guint potential_leaks = 0;
  for (int i = 0; i < GN_MEM_COMPONENT_MAX; i++) {
    if (stats.component_count[i] > 0) {
      potential_leaks += stats.component_count[i];
    }
  }

  if (age_threshold_seconds == 0) {
    /* At shutdown, any remaining allocations are potential leaks */
    gsize unfreed = stats.total_allocated - stats.total_freed;
    if (unfreed > 0) {
      g_debug("mem-profile: %zu bytes still allocated at check", unfreed);
    }
  }

  return potential_leaks;
}

void
gn_mem_profile_gc_caches(void)
{
  if (!state.initialized) return;

  g_debug("mem-profile: Running cache garbage collection");

  /* This is a hook point for modules to register their cache cleanup.
   * For now, we just emit a signal/callback that interested parties can use.
   * The actual cleanup happens in the respective modules (relay_store, etc.)
   */

  /* Future: Add callback registration for cache cleanup */
}
