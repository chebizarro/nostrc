/* stress_scroll.h - Deterministic scroll stress test for crash reproduction
 *
 * Enable with: GNOSTR_STRESS_SCROLL=1 ./gnostr
 *
 * This creates a high-frequency scroll loop that rapidly adjusts the
 * timeline scroll position, triggering model invalidations, widget
 * disposal, and signal emission at ~200Hz.
 *
 * Purpose: Turn "scrolling triggers crash" into a deterministic,
 * reproducible test that doesn't require human interaction.
 */

#ifndef GNOSTR_STRESS_SCROLL_H
#define GNOSTR_STRESS_SCROLL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Start stress scroll test on the given adjustment.
 * Returns the source ID (can be used to stop with g_source_remove).
 * Returns 0 if stress scroll is disabled via env. */
guint gnostr_stress_scroll_start(GtkAdjustment *adj);

/* Stop stress scroll test */
void gnostr_stress_scroll_stop(guint source_id);

/* Check if stress scroll is enabled via environment */
gboolean gnostr_stress_scroll_enabled(void);

/* Get stress scroll statistics */
typedef struct {
    guint64 iterations;
    guint64 direction_changes;
    gint64 start_time_us;
    gdouble current_velocity;
} GnostrStressScrollStats;

void gnostr_stress_scroll_get_stats(GnostrStressScrollStats *stats);

G_END_DECLS

#endif /* GNOSTR_STRESS_SCROLL_H */
