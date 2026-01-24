/**
 * GnostrZapGoalWidget - NIP-75 Zap Goal Display Widget
 *
 * Displays a zap goal with progress bar, showing funding progress
 * toward a target amount with optional deadline.
 */

#ifndef GNOSTR_ZAP_GOAL_WIDGET_H
#define GNOSTR_ZAP_GOAL_WIDGET_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ZAP_GOAL_WIDGET (gnostr_zap_goal_widget_get_type())

G_DECLARE_FINAL_TYPE(GnostrZapGoalWidget, gnostr_zap_goal_widget, GNOSTR, ZAP_GOAL_WIDGET, GtkWidget)

/**
 * Signals:
 * - "zap-to-goal" (gchar* goal_id, gchar* creator_pubkey, gchar* lud16, gpointer user_data)
 *   Emitted when user clicks to zap toward the goal
 * - "goal-clicked" (gchar* goal_id, gpointer user_data)
 *   Emitted when user clicks on the goal to view details
 * - "creator-clicked" (gchar* pubkey, gpointer user_data)
 *   Emitted when user clicks on the goal creator's name
 */

typedef struct _GnostrZapGoalWidget GnostrZapGoalWidget;

/**
 * gnostr_zap_goal_widget_new:
 *
 * Create a new zap goal widget.
 *
 * Returns: (transfer full): New widget
 */
GnostrZapGoalWidget *gnostr_zap_goal_widget_new(void);

/**
 * gnostr_zap_goal_widget_set_goal_id:
 * @self: the widget
 * @goal_id_hex: Goal event ID (hex)
 *
 * Set the goal event ID.
 */
void gnostr_zap_goal_widget_set_goal_id(GnostrZapGoalWidget *self,
                                         const gchar *goal_id_hex);

/**
 * gnostr_zap_goal_widget_get_goal_id:
 * @self: the widget
 *
 * Get the goal event ID.
 *
 * Returns: (transfer none) (nullable): Goal ID hex string
 */
const gchar *gnostr_zap_goal_widget_get_goal_id(GnostrZapGoalWidget *self);

/**
 * gnostr_zap_goal_widget_set_creator:
 * @self: the widget
 * @pubkey_hex: Creator's pubkey (hex)
 * @display_name: (nullable): Creator's display name
 * @lud16: (nullable): Creator's lightning address for zapping
 *
 * Set the goal creator info.
 */
void gnostr_zap_goal_widget_set_creator(GnostrZapGoalWidget *self,
                                         const gchar *pubkey_hex,
                                         const gchar *display_name,
                                         const gchar *lud16);

/**
 * gnostr_zap_goal_widget_set_description:
 * @self: the widget
 * @description: Goal description text
 *
 * Set the goal description.
 */
void gnostr_zap_goal_widget_set_description(GnostrZapGoalWidget *self,
                                             const gchar *description);

/**
 * gnostr_zap_goal_widget_set_target:
 * @self: the widget
 * @target_msat: Target amount in millisatoshis
 *
 * Set the funding target.
 */
void gnostr_zap_goal_widget_set_target(GnostrZapGoalWidget *self,
                                        gint64 target_msat);

/**
 * gnostr_zap_goal_widget_set_progress:
 * @self: the widget
 * @received_msat: Amount received so far in millisatoshis
 * @zap_count: Number of zaps received
 *
 * Update the progress display.
 */
void gnostr_zap_goal_widget_set_progress(GnostrZapGoalWidget *self,
                                          gint64 received_msat,
                                          guint zap_count);

/**
 * gnostr_zap_goal_widget_set_deadline:
 * @self: the widget
 * @closed_at: Deadline timestamp, or 0 for no deadline
 *
 * Set the goal deadline.
 */
void gnostr_zap_goal_widget_set_deadline(GnostrZapGoalWidget *self,
                                          gint64 closed_at);

/**
 * gnostr_zap_goal_widget_set_complete:
 * @self: the widget
 * @is_complete: Whether the goal has been reached
 *
 * Mark the goal as complete (target reached).
 */
void gnostr_zap_goal_widget_set_complete(GnostrZapGoalWidget *self,
                                          gboolean is_complete);

/**
 * gnostr_zap_goal_widget_set_expired:
 * @self: the widget
 * @is_expired: Whether the deadline has passed
 *
 * Mark the goal as expired.
 */
void gnostr_zap_goal_widget_set_expired(GnostrZapGoalWidget *self,
                                         gboolean is_expired);

/**
 * gnostr_zap_goal_widget_set_linked_event:
 * @self: the widget
 * @event_id: (nullable): Linked event ID (hex)
 *
 * Set a linked event (what the goal is funding).
 */
void gnostr_zap_goal_widget_set_linked_event(GnostrZapGoalWidget *self,
                                              const gchar *event_id);

/**
 * gnostr_zap_goal_widget_set_logged_in:
 * @self: the widget
 * @logged_in: Whether the user is logged in
 *
 * Set whether zap functionality is available.
 */
void gnostr_zap_goal_widget_set_logged_in(GnostrZapGoalWidget *self,
                                           gboolean logged_in);

/**
 * gnostr_zap_goal_widget_get_progress_percent:
 * @self: the widget
 *
 * Get the current progress percentage.
 *
 * Returns: Progress as 0.0-100.0+ percentage
 */
gdouble gnostr_zap_goal_widget_get_progress_percent(GnostrZapGoalWidget *self);

/**
 * gnostr_zap_goal_widget_is_complete:
 * @self: the widget
 *
 * Check if the goal is complete.
 *
 * Returns: TRUE if target reached
 */
gboolean gnostr_zap_goal_widget_is_complete(GnostrZapGoalWidget *self);

/**
 * gnostr_zap_goal_widget_is_expired:
 * @self: the widget
 *
 * Check if the goal has expired.
 *
 * Returns: TRUE if deadline passed
 */
gboolean gnostr_zap_goal_widget_is_expired(GnostrZapGoalWidget *self);

G_END_DECLS

#endif /* GNOSTR_ZAP_GOAL_WIDGET_H */
