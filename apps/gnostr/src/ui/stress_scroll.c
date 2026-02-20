/* stress_scroll.c - Deterministic scroll stress test for crash reproduction
 *
 * Enable with: GNOSTR_STRESS_SCROLL=1 ./gnostr
 *
 * This creates a high-frequency scroll loop that rapidly adjusts the
 * timeline scroll position, triggering model invalidations, widget
 * disposal, and signal emission at ~200Hz.
 */

#include "stress_scroll.h"
#include <glib.h>

/* ── Configuration ─────────────────────────────────────────────────── */

/* Scroll interval in milliseconds (16ms = 60Hz, realistic but harsh) */
#define STRESS_SCROLL_INTERVAL_MS 16

/* Scroll step: use page_size multiplier instead of fixed pixels */
#define STRESS_SCROLL_PAGE_MULTIPLIER 1.5

/* Idle gap: pause every N ticks for M milliseconds */
#define STRESS_SCROLL_IDLE_EVERY_N_TICKS 50
#define STRESS_SCROLL_IDLE_DURATION_MS 200

/* Delay before starting stress scroll (0 = immediate) */
#define STRESS_SCROLL_DELAY_MS 0

/* ── Global State ──────────────────────────────────────────────────── */

static gboolean g_stress_enabled = FALSE;
static gboolean g_stress_checked = FALSE;
static gdouble g_scroll_direction = 1.0;  /* 1.0 = down, -1.0 = up */
static guint g_active_source_id = 0;

static GnostrStressScrollStats g_stats = {0};

/* ── Implementation ────────────────────────────────────────────────── */

gboolean gnostr_stress_scroll_enabled(void) {
    if (!g_stress_checked) {
        const char *env = g_getenv("GNOSTR_STRESS_SCROLL");
        g_stress_enabled = (env && *env && g_strcmp0(env, "0") != 0);
        g_stress_checked = TRUE;
        
        if (g_stress_enabled) {
            g_warning("[STRESS_SCROLL] Stress scroll mode ENABLED");
            g_warning("[STRESS_SCROLL] Interval: %dms, PageMult: %.1fx, IdleEvery: %d ticks",
                      STRESS_SCROLL_INTERVAL_MS, STRESS_SCROLL_PAGE_MULTIPLIER,
                      STRESS_SCROLL_IDLE_EVERY_N_TICKS);
        }
    }
    return g_stress_enabled;
}

static gboolean stress_scroll_cb(gpointer user_data) {
    GtkAdjustment *adj = GTK_ADJUSTMENT(user_data);
    
    if (!GTK_IS_ADJUSTMENT(adj)) {
        g_warning("[STRESS_SCROLL] Adjustment destroyed, stopping");
        g_active_source_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    gdouble value = gtk_adjustment_get_value(adj);
    gdouble lower = gtk_adjustment_get_lower(adj);
    gdouble upper = gtk_adjustment_get_upper(adj);
    gdouble page_size = gtk_adjustment_get_page_size(adj);
    gdouble max_value = upper - page_size;
    
    /* Log every 50 iterations to track progress without flooding */
    if (g_stats.iterations % 50 == 0) {
        g_warning("[STRESS_SCROLL] tick=%llu value=%.0f/%.0f dir=%.0f page=%.0f",
                  (unsigned long long)g_stats.iterations, value, max_value, g_scroll_direction, page_size);
    }
    
    /* Idle gap: pause every N ticks to let UI catch up */
    if (g_stats.iterations > 0 && (g_stats.iterations % STRESS_SCROLL_IDLE_EVERY_N_TICKS) == 0) {
        /* Schedule resume after idle duration */
        g_stats.iterations++;
        return G_SOURCE_CONTINUE;  /* Skip this tick as idle */
    }
    
    /* Calculate new value using page-sized steps */
    gdouble step = page_size * STRESS_SCROLL_PAGE_MULTIPLIER * g_scroll_direction;
    gdouble new_value = value + step;
    
    /* Bounce at boundaries */
    if (new_value >= max_value) {
        new_value = max_value;
        g_scroll_direction = -1.0;
        g_stats.direction_changes++;
    } else if (new_value <= lower) {
        new_value = lower;
        g_scroll_direction = 1.0;
        g_stats.direction_changes++;
    }
    
    /* Apply scroll */
    gtk_adjustment_set_value(adj, new_value);
    
    /* Update stats */
    g_stats.iterations++;
    g_stats.current_velocity = step / (gdouble)STRESS_SCROLL_INTERVAL_MS;
    
    /* Log progress every 200 iterations */
    if (g_stats.iterations > 0 && g_stats.iterations % 200 == 0) {
        gint64 elapsed_us = g_get_monotonic_time() - g_stats.start_time_us;
        gdouble elapsed_sec = (gdouble)elapsed_us / 1000000.0;
        gdouble rate = (gdouble)g_stats.iterations / elapsed_sec;
        
        g_warning("[STRESS_SCROLL] progress: iter=%llu bounces=%llu rate=%.1f/s pos=%.0f",
                  (unsigned long long)g_stats.iterations,
                  (unsigned long long)g_stats.direction_changes,
                  rate, new_value);
    }
    
    return G_SOURCE_CONTINUE;
}

/* Callback to start the actual stress scroll after delay */
static gboolean stress_scroll_delayed_start_cb(gpointer user_data) {
    GtkAdjustment *adj = GTK_ADJUSTMENT(user_data);
    
    if (!GTK_IS_ADJUSTMENT(adj)) {
        g_warning("[STRESS_SCROLL] Adjustment destroyed before start");
        return G_SOURCE_REMOVE;
    }
    
    gdouble upper = gtk_adjustment_get_upper(adj);
    gdouble page_size = gtk_adjustment_get_page_size(adj);
    
    g_warning("[STRESS_SCROLL] Delayed start: upper=%.0f page=%.0f scrollable=%.0f",
              upper, page_size, upper - page_size);
    
    /* Initialize stats */
    g_stats.iterations = 0;
    g_stats.direction_changes = 0;
    g_stats.start_time_us = g_get_monotonic_time();
    g_stats.current_velocity = 0.0;
    g_scroll_direction = 1.0;
    
    g_warning("[STRESS_SCROLL] === STARTING STRESS SCROLL NOW ===");
    
    /* Start the scroll timer */
    g_active_source_id = g_timeout_add(STRESS_SCROLL_INTERVAL_MS, stress_scroll_cb, adj);
    
    return G_SOURCE_REMOVE; /* One-shot */
}

guint gnostr_stress_scroll_start(GtkAdjustment *adj) {
    if (!gnostr_stress_scroll_enabled()) {
        return 0;
    }
    
    if (!GTK_IS_ADJUSTMENT(adj)) {
        g_warning("[STRESS_SCROLL] Invalid adjustment, cannot start");
        return 0;
    }
    
    if (STRESS_SCROLL_DELAY_MS > 0) {
        g_warning("[STRESS_SCROLL] Will start in %dms", STRESS_SCROLL_DELAY_MS);
        return g_timeout_add(STRESS_SCROLL_DELAY_MS, stress_scroll_delayed_start_cb, adj);
    }
    
    /* Start immediately */
    g_warning("[STRESS_SCROLL] Starting immediately (no delay)");
    
    /* Initialize stats */
    g_stats.iterations = 0;
    g_stats.direction_changes = 0;
    g_stats.start_time_us = g_get_monotonic_time();
    g_stats.current_velocity = 0.0;
    g_scroll_direction = 1.0;
    
    g_active_source_id = g_timeout_add(STRESS_SCROLL_INTERVAL_MS, stress_scroll_cb, adj);
    guint source_id = g_active_source_id;
    
    return source_id;
}

void gnostr_stress_scroll_stop(guint source_id) {
    if (g_active_source_id > 0) {
        g_source_remove(g_active_source_id);
        g_active_source_id = 0;
    }
    if (source_id > 0 && source_id != g_active_source_id) {
        g_source_remove(source_id);
        
        gint64 elapsed_us = g_get_monotonic_time() - g_stats.start_time_us;
        gdouble elapsed_sec = (gdouble)elapsed_us / 1000000.0;
        
        g_warning("[STRESS_SCROLL] Stopped after %.1fs", elapsed_sec);
        g_warning("[STRESS_SCROLL] Final stats: iterations=%llu bounces=%llu",
                  (unsigned long long)g_stats.iterations,
                  (unsigned long long)g_stats.direction_changes);
    }
    g_warning("[STRESS_SCROLL] Stress scroll stopped");
}

void gnostr_stress_scroll_get_stats(GnostrStressScrollStats *stats) {
    if (stats) {
        *stats = g_stats;
    }
}
