/**
 * gnostr-live-card.h - NIP-53 Live Activity Card Widget
 *
 * Displays a live activity (stream, broadcast, event) in a card format:
 * - Cover image/thumbnail
 * - Title and summary
 * - Live status indicator badge
 * - Host/speaker avatars
 * - Viewer count
 * - "Watch Live" or "Set Reminder" action button
 *
 * Signals:
 * - "watch-live" - Emitted when Watch Live button is clicked
 * - "set-reminder" - Emitted for planned events
 * - "profile-clicked" (pubkey_hex) - When participant avatar is clicked
 */

#ifndef GNOSTR_LIVE_CARD_H
#define GNOSTR_LIVE_CARD_H

#include <gtk/gtk.h>
#include "../util/nip53_live.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_LIVE_CARD (gnostr_live_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrLiveCard, gnostr_live_card, GNOSTR, LIVE_CARD, GtkWidget)

typedef struct _GnostrLiveCard GnostrLiveCard;

/**
 * gnostr_live_card_new:
 *
 * Creates a new live activity card widget.
 *
 * Returns: (transfer full): A new #GnostrLiveCard
 */
GnostrLiveCard *gnostr_live_card_new(void);

/**
 * gnostr_live_card_set_activity:
 * @self: A #GnostrLiveCard
 * @activity: The live activity to display
 *
 * Sets the live activity to display in this card.
 * The activity data is copied internally.
 */
void gnostr_live_card_set_activity(GnostrLiveCard *self,
                                    const GnostrLiveActivity *activity);

/**
 * gnostr_live_card_get_activity:
 * @self: A #GnostrLiveCard
 *
 * Gets the current live activity.
 *
 * Returns: (transfer none) (nullable): The current activity or NULL
 */
const GnostrLiveActivity *gnostr_live_card_get_activity(GnostrLiveCard *self);

/**
 * gnostr_live_card_set_loading:
 * @self: A #GnostrLiveCard
 * @loading: Whether to show loading state
 *
 * Shows or hides the loading spinner.
 */
void gnostr_live_card_set_loading(GnostrLiveCard *self, gboolean loading);

/**
 * gnostr_live_card_set_error:
 * @self: A #GnostrLiveCard
 * @error_message: (nullable): Error message to display, or NULL to clear
 *
 * Shows an error state with the given message.
 */
void gnostr_live_card_set_error(GnostrLiveCard *self, const char *error_message);

/**
 * gnostr_live_card_set_compact:
 * @self: A #GnostrLiveCard
 * @compact: Whether to use compact layout
 *
 * Enables compact mode for use in lists (smaller image, less padding).
 */
void gnostr_live_card_set_compact(GnostrLiveCard *self, gboolean compact);

/**
 * gnostr_live_card_update_participant_info:
 * @self: A #GnostrLiveCard
 * @pubkey_hex: Participant pubkey
 * @display_name: (nullable): Display name
 * @avatar_url: (nullable): Avatar URL
 *
 * Updates cached profile info for a participant (for lazy loading profiles).
 */
void gnostr_live_card_update_participant_info(GnostrLiveCard *self,
                                               const char *pubkey_hex,
                                               const char *display_name,
                                               const char *avatar_url);

/**
 * gnostr_live_card_get_event_id:
 * @self: A #GnostrLiveCard
 *
 * Gets the event ID of the displayed activity.
 *
 * Returns: (transfer none) (nullable): Event ID hex string or NULL
 */
const char *gnostr_live_card_get_event_id(GnostrLiveCard *self);

/**
 * gnostr_live_card_get_streaming_url:
 * @self: A #GnostrLiveCard
 *
 * Gets the primary streaming URL.
 *
 * Returns: (transfer none) (nullable): Streaming URL or NULL
 */
const char *gnostr_live_card_get_streaming_url(GnostrLiveCard *self);

G_END_DECLS

#endif /* GNOSTR_LIVE_CARD_H */
