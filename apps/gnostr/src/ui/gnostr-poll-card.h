/*
 * gnostr-poll-card.h - NIP-88 Poll Card Widget
 *
 * Displays kind 1018 poll events with voting interface:
 * - Poll question as header
 * - Radio buttons (single choice) or checkboxes (multiple choice) for options
 * - Vote button (disabled if already voted or poll ended)
 * - Results bar chart showing vote percentages
 * - Poll end time display with countdown
 * - Author info with avatar
 *
 * Poll responses are kind 1019 events.
 */

#ifndef GNOSTR_POLL_CARD_H
#define GNOSTR_POLL_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_POLL_CARD (gnostr_poll_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrPollCard, gnostr_poll_card, GNOSTR, POLL_CARD, GtkWidget)

/*
 * Signals:
 *
 * "vote-clicked" (gchar* poll_id, GArray* selected_indices, gpointer user_data)
 *   - Emitted when user clicks the vote button
 *   - poll_id: Poll event ID (hex string)
 *   - selected_indices: GArray of ints with selected option indices
 *
 * "results-requested" (gchar* poll_id, gpointer user_data)
 *   - Emitted when user wants to refresh vote results
 *   - poll_id: Poll event ID (hex string)
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks the poll author's profile
 *   - pubkey_hex: Author's public key (hex string)
 */

typedef struct _GnostrPollCard GnostrPollCard;

/*
 * Poll option data structure
 */
typedef struct {
  int index;           /* Option index (0-based) */
  char *text;          /* Option text */
  guint vote_count;    /* Number of votes for this option */
} GnostrPollCardOption;

/*
 * gnostr_poll_card_new:
 *
 * Creates a new poll card widget.
 *
 * Returns: (transfer full): A new #GnostrPollCard
 */
GnostrPollCard *gnostr_poll_card_new(void);

/*
 * gnostr_poll_card_set_poll:
 * @self: The poll card
 * @event_id: Poll event ID (hex)
 * @question: Poll question text (from content)
 * @created_at: Poll creation timestamp
 *
 * Sets the main poll data from the kind 1018 event.
 */
void gnostr_poll_card_set_poll(GnostrPollCard *self,
                                const char *event_id,
                                const char *question,
                                gint64 created_at);

/*
 * gnostr_poll_card_set_author:
 * @self: The poll card
 * @pubkey_hex: Author's public key (hex)
 * @display_name: (nullable): Author's display name
 * @avatar_url: (nullable): Author's avatar URL
 *
 * Sets the poll author information.
 */
void gnostr_poll_card_set_author(GnostrPollCard *self,
                                  const char *pubkey_hex,
                                  const char *display_name,
                                  const char *avatar_url);

/*
 * gnostr_poll_card_set_options:
 * @self: The poll card
 * @options: Array of GnostrPollCardOption structures
 * @count: Number of options
 *
 * Sets the poll options from "poll_option" tags.
 */
void gnostr_poll_card_set_options(GnostrPollCard *self,
                                   GnostrPollCardOption *options,
                                   gsize count);

/*
 * gnostr_poll_card_set_multiple_choice:
 * @self: The poll card
 * @multiple: TRUE for multi-select, FALSE for single select
 *
 * Sets whether this is a multiple choice poll.
 * Derived from value_maximum tag (1 = single, >1 = multiple).
 */
void gnostr_poll_card_set_multiple_choice(GnostrPollCard *self,
                                           gboolean multiple);

/*
 * gnostr_poll_card_is_multiple_choice:
 * @self: The poll card
 *
 * Returns: TRUE if multiple selections allowed
 */
gboolean gnostr_poll_card_is_multiple_choice(GnostrPollCard *self);

/*
 * gnostr_poll_card_set_end_time:
 * @self: The poll card
 * @end_time: Unix timestamp when poll closes (0 = no end time)
 *
 * Sets the poll end time from "expiration" or "closed_at" tag.
 */
void gnostr_poll_card_set_end_time(GnostrPollCard *self, gint64 end_time);

/*
 * gnostr_poll_card_get_end_time:
 * @self: The poll card
 *
 * Returns: The poll end time, or 0 if no end time
 */
gint64 gnostr_poll_card_get_end_time(GnostrPollCard *self);

/*
 * gnostr_poll_card_is_closed:
 * @self: The poll card
 *
 * Returns: TRUE if the poll has ended
 */
gboolean gnostr_poll_card_is_closed(GnostrPollCard *self);

/*
 * gnostr_poll_card_set_vote_counts:
 * @self: The poll card
 * @vote_counts: Array of vote counts indexed by option index
 * @count: Number of entries
 * @total_votes: Total number of unique voters
 *
 * Updates the vote counts for all options.
 */
void gnostr_poll_card_set_vote_counts(GnostrPollCard *self,
                                       guint *vote_counts,
                                       gsize count,
                                       guint total_votes);

/*
 * gnostr_poll_card_set_has_voted:
 * @self: The poll card
 * @has_voted: TRUE if current user has voted
 *
 * Sets whether the current user has already voted.
 * When TRUE, shows results and disables voting.
 */
void gnostr_poll_card_set_has_voted(GnostrPollCard *self, gboolean has_voted);

/*
 * gnostr_poll_card_has_voted:
 * @self: The poll card
 *
 * Returns: TRUE if user has voted
 */
gboolean gnostr_poll_card_has_voted(GnostrPollCard *self);

/*
 * gnostr_poll_card_set_user_votes:
 * @self: The poll card
 * @indices: Array of option indices the user voted for
 * @count: Number of indices
 *
 * Sets which options the current user voted for (for highlighting).
 */
void gnostr_poll_card_set_user_votes(GnostrPollCard *self,
                                      int *indices,
                                      gsize count);

/*
 * gnostr_poll_card_set_logged_in:
 * @self: The poll card
 * @logged_in: TRUE if user is logged in
 *
 * Sets login state (affects vote button sensitivity).
 */
void gnostr_poll_card_set_logged_in(GnostrPollCard *self, gboolean logged_in);

/*
 * gnostr_poll_card_get_selected:
 * @self: The poll card
 *
 * Gets the currently selected option indices.
 *
 * Returns: (transfer full): GArray of ints. Caller must free with g_array_unref().
 */
GArray *gnostr_poll_card_get_selected(GnostrPollCard *self);

/*
 * gnostr_poll_card_get_poll_id:
 * @self: The poll card
 *
 * Returns: (transfer none) (nullable): The poll event ID
 */
const char *gnostr_poll_card_get_poll_id(GnostrPollCard *self);

/*
 * gnostr_poll_card_get_author_pubkey:
 * @self: The poll card
 *
 * Returns: (transfer none) (nullable): The author's pubkey hex
 */
const char *gnostr_poll_card_get_author_pubkey(GnostrPollCard *self);

G_END_DECLS

#endif /* GNOSTR_POLL_CARD_H */
