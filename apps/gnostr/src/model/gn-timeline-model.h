/**
 * GnTimelineModel - Lazy view on NostrDB for timeline display
 *
 * A GListModel implementation that queries NostrDB on-demand instead of
 * maintaining a manual list of items. Supports cursor-based pagination
 * and efficient handling of new notes.
 *
 * Part of the Timeline Architecture Refactor (nostrc-e03f)
 */

#ifndef GN_TIMELINE_MODEL_H
#define GN_TIMELINE_MODEL_H

#include <gio/gio.h>
#include <gtk/gtk.h>
#include "gn-timeline-query.h"
#include "gn-nostr-event-item.h"

G_BEGIN_DECLS

#define GN_TYPE_TIMELINE_MODEL (gn_timeline_model_get_type())
G_DECLARE_FINAL_TYPE(GnTimelineModel, gn_timeline_model, GN, TIMELINE_MODEL, GObject)

/**
 * gn_timeline_model_new:
 * @query: (transfer none): The query filter for this timeline
 *
 * Create a new timeline model with the given query.
 *
 * Returns: (transfer full): A new timeline model
 */
GnTimelineModel *gn_timeline_model_new(GnTimelineQuery *query);

/**
 * gn_timeline_model_new_global:
 *
 * Create a new timeline model for the global timeline.
 *
 * Returns: (transfer full): A new timeline model
 */
GnTimelineModel *gn_timeline_model_new_global(void);

/* ============== Query Management ============== */

/**
 * gn_timeline_model_set_query:
 * @self: The model
 * @query: (transfer none): New query filter
 *
 * Change the query filter. This clears the model and reloads.
 */
void gn_timeline_model_set_query(GnTimelineModel *self, GnTimelineQuery *query);

/**
 * gn_timeline_model_get_query:
 * @self: The model
 *
 * Get the current query filter.
 *
 * Returns: (transfer none): The query (owned by model)
 */
GnTimelineQuery *gn_timeline_model_get_query(GnTimelineModel *self);

/**
 * gn_timeline_model_refresh:
 * @self: The model
 *
 * Clear cache and reload from NostrDB.
 */
void gn_timeline_model_refresh(GnTimelineModel *self);

/**
 * gn_timeline_model_clear:
 * @self: The model
 *
 * Clear all items from the model.
 */
void gn_timeline_model_clear(GnTimelineModel *self);

/* ============== Pagination ============== */

/**
 * gn_timeline_model_load_older:
 * @self: The model
 * @count: Number of items to load
 *
 * Load older items (for infinite scroll).
 *
 * Returns: Number of items actually loaded
 */
guint gn_timeline_model_load_older(GnTimelineModel *self, guint count);

/**
 * gn_timeline_model_get_oldest_timestamp:
 * @self: The model
 *
 * Get the timestamp of the oldest loaded item.
 *
 * Returns: Unix timestamp, or 0 if empty
 */
gint64 gn_timeline_model_get_oldest_timestamp(GnTimelineModel *self);

/**
 * gn_timeline_model_get_newest_timestamp:
 * @self: The model
 *
 * Get the timestamp of the newest loaded item.
 *
 * Returns: Unix timestamp, or 0 if empty
 */
gint64 gn_timeline_model_get_newest_timestamp(GnTimelineModel *self);

/* ============== Scroll Position Awareness ============== */

/**
 * gn_timeline_model_set_user_at_top:
 * @self: The model
 * @at_top: Whether user is scrolled to top
 *
 * Set scroll position state. When user is at top, new items are
 * added immediately. When scrolled down, items are deferred.
 */
void gn_timeline_model_set_user_at_top(GnTimelineModel *self, gboolean at_top);

/**
 * gn_timeline_model_get_pending_count:
 * @self: The model
 *
 * Get number of pending new items (when user is scrolled down).
 *
 * Returns: Number of pending items
 */
guint gn_timeline_model_get_pending_count(GnTimelineModel *self);

/**
 * gn_timeline_model_flush_pending:
 * @self: The model
 *
 * Flush pending items into the visible timeline.
 * Called when user clicks "N new notes" indicator.
 */
void gn_timeline_model_flush_pending(GnTimelineModel *self);

/* ============== Phase 3: Animated Reveal (nostrc-0hp) ============== */

/**
 * gn_timeline_model_flush_pending_animated:
 * @self: The model
 * @complete_cb: (nullable): Callback when reveal finishes
 * @complete_data: (nullable): User data for completion callback
 *
 * Flush pending items with a smooth staggered reveal animation.
 *
 * Instead of inserting all pending items at once, this function animates
 * items in one-by-one with a 50ms stagger between each batch. This provides
 * a graceful UX when clicking the "New Notes" button.
 *
 * The completion callback is invoked AFTER all items are revealed, allowing
 * the caller to scroll to top after the animation completes.
 *
 * Callback signature: void (*)(gpointer model, gpointer user_data)
 */
void gn_timeline_model_flush_pending_animated(GnTimelineModel *self,
                                               GFunc            complete_cb,
                                               gpointer         complete_data);

/**
 * gn_timeline_model_is_reveal_in_progress:
 * @self: The model
 *
 * Check if an animated reveal is currently in progress.
 *
 * Returns: TRUE if reveal animation is active
 */
gboolean gn_timeline_model_is_reveal_in_progress(GnTimelineModel *self);

/**
 * gn_timeline_model_cancel_reveal:
 * @self: The model
 *
 * Cancel any in-progress reveal animation.
 * Items already revealed will remain, but remaining items are discarded.
 */
void gn_timeline_model_cancel_reveal(GnTimelineModel *self);

/* ============== Visible Range ============== */

/**
 * gn_timeline_model_set_visible_range:
 * @self: The model
 * @start: First visible position
 * @end: Last visible position
 *
 * Update the visible range for prefetching optimization.
 */
void gn_timeline_model_set_visible_range(GnTimelineModel *self, guint start, guint end);

/* ============== Profile Updates ============== */

/**
 * gn_timeline_model_update_profile:
 * @self: The model
 * @pubkey_hex: Public key of the profile
 *
 * Notify that a profile has been updated. Items for this author
 * will be refreshed.
 */
void gn_timeline_model_update_profile(GnTimelineModel *self, const char *pubkey_hex);

/* ============== Batch Mode ============== */

/**
 * gn_timeline_model_begin_batch:
 * @self: The model
 *
 * Begin batch mode. UI updates are suppressed until end_batch() is called.
 * Use this during initial load to prevent widget recycling storms.
 */
void gn_timeline_model_begin_batch(GnTimelineModel *self);

/**
 * gn_timeline_model_end_batch:
 * @self: The model
 *
 * End batch mode and emit a single items_changed signal for all
 * accumulated changes since begin_batch() was called.
 */
void gn_timeline_model_end_batch(GnTimelineModel *self);

/* ============== Frame-Aware Batching (nostrc-0hp) ============== */

/**
 * gn_timeline_model_set_view_widget:
 * @self: The model
 * @widget: (nullable): The view widget to use for frame clock, or NULL to disable
 *
 * Set the widget used for frame-synchronized updates. When set, new
 * items are staged and processed at most N items per frame, preventing
 * UI freezes during heavy traffic.
 *
 * The model holds a weak reference to the widget. Call with NULL or
 * let the widget be destroyed to disable frame-aware batching.
 */
void gn_timeline_model_set_view_widget(GnTimelineModel *self, GtkWidget *widget);

/**
 * gn_timeline_model_get_staged_count:
 * @self: The model
 *
 * Get the number of items currently in the staging buffer awaiting
 * frame-synchronized insertion.
 *
 * Returns: Number of staged items
 */
guint gn_timeline_model_get_staged_count(GnTimelineModel *self);

/* ============== Phase 2: Staging Pipeline (nostrc-0hp) ============== */

/**
 * gn_timeline_model_get_incoming_count:
 * @self: The model
 *
 * DEPRECATED (hq-a11by): Always returns 0.  The incoming queue has been
 * removed; items go directly into the staging buffer.
 *
 * Returns: 0
 */
guint gn_timeline_model_get_incoming_count(GnTimelineModel *self);

/**
 * gn_timeline_model_get_total_queued_count:
 * @self: The model
 *
 * Get the total number of items queued in the staging buffer.
 * This represents all items waiting to be inserted into the visible model.
 *
 * Returns: Total queued items
 */
guint gn_timeline_model_get_total_queued_count(GnTimelineModel *self);

/**
 * gn_timeline_model_get_peak_queue_depth:
 * @self: The model
 *
 * Get the peak staging buffer depth (high-water mark) since last reset.
 * Useful for monitoring and diagnostics.
 *
 * Returns: Peak staging depth
 */
guint gn_timeline_model_get_peak_queue_depth(GnTimelineModel *self);

/**
 * gn_timeline_model_is_backpressure_active:
 * @self: The model
 *
 * Check if backpressure is currently being applied due to high staging depth.
 * When backpressure is active, oldest items may be dropped to prevent
 * unbounded buffer growth.
 *
 * Returns: TRUE if backpressure is active
 */
gboolean gn_timeline_model_is_backpressure_active(GnTimelineModel *self);

/**
 * gn_timeline_model_get_insertion_rate:
 * @self: The model
 *
 * DEPRECATED (hq-a11by): Always returns 0.0.  Rate tracking was removed
 * along with the throttle timer.
 *
 * Returns: 0.0
 */
gdouble gn_timeline_model_get_insertion_rate(GnTimelineModel *self);

/**
 * gn_timeline_model_reset_peak_queue_depth:
 * @self: The model
 *
 * Reset the peak staging depth counter.
 */
void gn_timeline_model_reset_peak_queue_depth(GnTimelineModel *self);

G_END_DECLS

#endif /* GN_TIMELINE_MODEL_H */
