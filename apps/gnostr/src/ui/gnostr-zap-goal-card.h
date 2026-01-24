/**
 * GnostrZapGoalCard - NIP-75 Zap Goal Card Widget
 *
 * A card-style GTK4 widget for displaying zap goals with:
 * - Goal title and description
 * - Progress bar showing current vs target amount
 * - Percentage and formatted amounts
 * - Time remaining countdown (if deadline set)
 * - Large Zap button for contributing
 * - Author info with avatar
 * - Celebration animation when goal is reached
 *
 * Signals:
 * - "zap-clicked": Emitted when the zap button is clicked
 * - "open-profile": Emitted when the author avatar/name is clicked
 * - "goal-reached": Emitted when progress hits 100%
 */

#ifndef GNOSTR_ZAP_GOAL_CARD_H
#define GNOSTR_ZAP_GOAL_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_ZAP_GOAL_CARD (gnostr_zap_goal_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrZapGoalCard, gnostr_zap_goal_card, GNOSTR, ZAP_GOAL_CARD, GtkWidget)

/**
 * GnostrZapGoalCard:
 *
 * A card widget for displaying NIP-75 zap goals.
 *
 * Signals:
 *
 * "zap-clicked":
 *   void user_function(GnostrZapGoalCard *card,
 *                      gchar *goal_id,
 *                      gchar *pubkey,
 *                      gchar *lud16,
 *                      gpointer user_data)
 *   Emitted when the user clicks the Zap button.
 *
 * "open-profile":
 *   void user_function(GnostrZapGoalCard *card,
 *                      gchar *pubkey,
 *                      gpointer user_data)
 *   Emitted when the user clicks on the goal creator.
 *
 * "goal-reached":
 *   void user_function(GnostrZapGoalCard *card,
 *                      gchar *goal_id,
 *                      gpointer user_data)
 *   Emitted when the goal reaches 100% completion.
 */
typedef struct _GnostrZapGoalCard GnostrZapGoalCard;

/* ============== Construction ============== */

/**
 * gnostr_zap_goal_card_new:
 *
 * Create a new zap goal card widget.
 *
 * Returns: (transfer full): A new #GnostrZapGoalCard
 */
GnostrZapGoalCard *gnostr_zap_goal_card_new(void);

/* ============== Goal Data ============== */

/**
 * gnostr_zap_goal_card_set_goal_id:
 * @self: A #GnostrZapGoalCard
 * @goal_id_hex: Goal event ID in hex format (64 chars)
 *
 * Set the goal event ID for reference and signals.
 */
void gnostr_zap_goal_card_set_goal_id(GnostrZapGoalCard *self,
                                       const gchar *goal_id_hex);

/**
 * gnostr_zap_goal_card_get_goal_id:
 * @self: A #GnostrZapGoalCard
 *
 * Get the goal event ID.
 *
 * Returns: (transfer none) (nullable): The goal ID hex string
 */
const gchar *gnostr_zap_goal_card_get_goal_id(GnostrZapGoalCard *self);

/**
 * gnostr_zap_goal_card_set_title:
 * @self: A #GnostrZapGoalCard
 * @title: Goal title text
 *
 * Set the goal title/description displayed in the card.
 */
void gnostr_zap_goal_card_set_title(GnostrZapGoalCard *self,
                                     const gchar *title);

/**
 * gnostr_zap_goal_card_set_target:
 * @self: A #GnostrZapGoalCard
 * @target_msats: Target amount in millisatoshis
 *
 * Set the funding target amount.
 */
void gnostr_zap_goal_card_set_target(GnostrZapGoalCard *self,
                                      gint64 target_msats);

/**
 * gnostr_zap_goal_card_set_progress:
 * @self: A #GnostrZapGoalCard
 * @current_msats: Current amount received in millisatoshis
 * @zap_count: Number of zaps received
 *
 * Update the progress display with current funding amount.
 * Will trigger "goal-reached" signal if progress >= 100%.
 */
void gnostr_zap_goal_card_set_progress(GnostrZapGoalCard *self,
                                        gint64 current_msats,
                                        guint zap_count);

/**
 * gnostr_zap_goal_card_set_deadline:
 * @self: A #GnostrZapGoalCard
 * @end_time: Unix timestamp of deadline, or 0 for no deadline
 *
 * Set the goal deadline. The card will show remaining time.
 */
void gnostr_zap_goal_card_set_deadline(GnostrZapGoalCard *self,
                                        gint64 end_time);

/* ============== Author Info ============== */

/**
 * gnostr_zap_goal_card_set_author:
 * @self: A #GnostrZapGoalCard
 * @pubkey_hex: Author's pubkey in hex format (64 chars)
 * @display_name: (nullable): Author's display name
 * @lud16: (nullable): Author's lightning address for zapping
 *
 * Set the goal creator's information.
 */
void gnostr_zap_goal_card_set_author(GnostrZapGoalCard *self,
                                      const gchar *pubkey_hex,
                                      const gchar *display_name,
                                      const gchar *lud16);

/**
 * gnostr_zap_goal_card_set_avatar:
 * @self: A #GnostrZapGoalCard
 * @avatar_url: (nullable): URL of the author's avatar image
 *
 * Set the author's avatar image URL. Uses avatar cache.
 */
void gnostr_zap_goal_card_set_avatar(GnostrZapGoalCard *self,
                                      const gchar *avatar_url);

/* ============== State ============== */

/**
 * gnostr_zap_goal_card_set_logged_in:
 * @self: A #GnostrZapGoalCard
 * @logged_in: Whether the user is logged in
 *
 * Set whether zap functionality is available (requires login).
 */
void gnostr_zap_goal_card_set_logged_in(GnostrZapGoalCard *self,
                                         gboolean logged_in);

/**
 * gnostr_zap_goal_card_set_complete:
 * @self: A #GnostrZapGoalCard
 * @is_complete: Whether the goal is complete
 *
 * Manually set the completion state. Triggers celebration animation.
 */
void gnostr_zap_goal_card_set_complete(GnostrZapGoalCard *self,
                                        gboolean is_complete);

/**
 * gnostr_zap_goal_card_set_expired:
 * @self: A #GnostrZapGoalCard
 * @is_expired: Whether the goal has expired
 *
 * Set the expired state. Disables zap button when expired.
 */
void gnostr_zap_goal_card_set_expired(GnostrZapGoalCard *self,
                                       gboolean is_expired);

/* ============== Queries ============== */

/**
 * gnostr_zap_goal_card_get_progress_percent:
 * @self: A #GnostrZapGoalCard
 *
 * Get the current progress percentage.
 *
 * Returns: Progress as 0.0-100.0+ percentage
 */
gdouble gnostr_zap_goal_card_get_progress_percent(GnostrZapGoalCard *self);

/**
 * gnostr_zap_goal_card_is_complete:
 * @self: A #GnostrZapGoalCard
 *
 * Check if the goal is complete (target reached).
 *
 * Returns: TRUE if target has been reached
 */
gboolean gnostr_zap_goal_card_is_complete(GnostrZapGoalCard *self);

/**
 * gnostr_zap_goal_card_is_expired:
 * @self: A #GnostrZapGoalCard
 *
 * Check if the goal has expired.
 *
 * Returns: TRUE if deadline has passed
 */
gboolean gnostr_zap_goal_card_is_expired(GnostrZapGoalCard *self);

/* ============== Animation Control ============== */

/**
 * gnostr_zap_goal_card_trigger_celebration:
 * @self: A #GnostrZapGoalCard
 *
 * Manually trigger the celebration animation.
 * Normally called automatically when goal reaches 100%.
 */
void gnostr_zap_goal_card_trigger_celebration(GnostrZapGoalCard *self);

/**
 * gnostr_zap_goal_card_start_deadline_timer:
 * @self: A #GnostrZapGoalCard
 *
 * Start the countdown timer for the deadline display.
 * Updates every minute until deadline passes.
 */
void gnostr_zap_goal_card_start_deadline_timer(GnostrZapGoalCard *self);

/**
 * gnostr_zap_goal_card_stop_deadline_timer:
 * @self: A #GnostrZapGoalCard
 *
 * Stop the deadline countdown timer.
 */
void gnostr_zap_goal_card_stop_deadline_timer(GnostrZapGoalCard *self);

G_END_DECLS

#endif /* GNOSTR_ZAP_GOAL_CARD_H */
