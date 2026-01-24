#ifndef GNOSTR_POLL_WIDGET_H
#define GNOSTR_POLL_WIDGET_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_POLL_WIDGET (gnostr_poll_widget_get_type())

G_DECLARE_FINAL_TYPE(GnostrPollWidget, gnostr_poll_widget, GNOSTR, POLL_WIDGET, GtkWidget)

/**
 * NIP-88: Poll Widget for displaying and voting on polls
 *
 * Polls are kind 1068 events with "poll_option" tags for choices.
 * Responses are kind 1018 events with "response" tag containing option index(es).
 *
 * Signals:
 * - "vote-requested" (gchar* poll_id, GArray* selected_indices, gpointer user_data)
 *   Emitted when user clicks vote button with selected option indices.
 */

typedef struct _GnostrPollWidget GnostrPollWidget;

/**
 * Poll option structure for setting up poll display
 */
typedef struct {
  int index;           /* Option index (0-based) */
  char *text;          /* Option text */
  guint vote_count;    /* Number of votes */
} GnostrPollOption;

/**
 * Create a new poll widget
 */
GnostrPollWidget *gnostr_poll_widget_new(void);

/**
 * Set the poll event ID (hex)
 */
void gnostr_poll_widget_set_poll_id(GnostrPollWidget *self, const char *poll_id_hex);

/**
 * Get the poll event ID
 */
const char *gnostr_poll_widget_get_poll_id(GnostrPollWidget *self);

/**
 * Set poll options
 * @param options: Array of GnostrPollOption structures
 * @param count: Number of options
 */
void gnostr_poll_widget_set_options(GnostrPollWidget *self,
                                     GnostrPollOption *options,
                                     gsize count);

/**
 * Set whether this is a multiple choice poll
 * If FALSE, only one option can be selected (radio buttons)
 * If TRUE, multiple options can be selected (checkboxes)
 */
void gnostr_poll_widget_set_multiple_choice(GnostrPollWidget *self, gboolean multiple);

/**
 * Check if this is a multiple choice poll
 */
gboolean gnostr_poll_widget_is_multiple_choice(GnostrPollWidget *self);

/**
 * Set the closing time for the poll
 * @param closed_at: Unix timestamp when poll closes, or 0 for no closing time
 */
void gnostr_poll_widget_set_closed_at(GnostrPollWidget *self, gint64 closed_at);

/**
 * Get the closing time for the poll
 */
gint64 gnostr_poll_widget_get_closed_at(GnostrPollWidget *self);

/**
 * Check if the poll is currently closed
 */
gboolean gnostr_poll_widget_is_closed(GnostrPollWidget *self);

/**
 * Set the total vote count for the poll
 */
void gnostr_poll_widget_set_total_votes(GnostrPollWidget *self, guint total);

/**
 * Get the total vote count
 */
guint gnostr_poll_widget_get_total_votes(GnostrPollWidget *self);

/**
 * Update vote counts for options
 * @param vote_counts: Array of vote counts (indexed by option index)
 * @param count: Number of entries in the array
 */
void gnostr_poll_widget_update_vote_counts(GnostrPollWidget *self,
                                            guint *vote_counts,
                                            gsize count);

/**
 * Set whether the current user has already voted
 * When TRUE, shows results instead of voting UI
 */
void gnostr_poll_widget_set_has_voted(GnostrPollWidget *self, gboolean has_voted);

/**
 * Check if the current user has voted
 */
gboolean gnostr_poll_widget_has_voted(GnostrPollWidget *self);

/**
 * Set the indices the current user voted for (for highlighting)
 */
void gnostr_poll_widget_set_user_votes(GnostrPollWidget *self,
                                        int *indices,
                                        gsize count);

/**
 * Set whether the user is logged in (affects vote button sensitivity)
 */
void gnostr_poll_widget_set_logged_in(GnostrPollWidget *self, gboolean logged_in);

/**
 * Get the currently selected option indices
 * Returns a GArray of ints. Caller must free with g_array_unref().
 */
GArray *gnostr_poll_widget_get_selected(GnostrPollWidget *self);

G_END_DECLS

#endif /* GNOSTR_POLL_WIDGET_H */
