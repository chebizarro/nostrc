#ifndef GNOSTR_POLL_COMPOSER_H
#define GNOSTR_POLL_COMPOSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_POLL_COMPOSER (gnostr_poll_composer_get_type())

G_DECLARE_FINAL_TYPE(GnostrPollComposer, gnostr_poll_composer, GNOSTR, POLL_COMPOSER, GtkWidget)

/**
 * NIP-88: Poll Composer Widget
 *
 * UI for creating new poll events (kind 1068).
 * Features:
 * - Poll question text entry
 * - Dynamic option list (2-10 options)
 * - Single/multiple choice toggle
 * - Optional closing time picker
 *
 * Signals:
 * - "poll-created" (gchar* question, GPtrArray* options, gboolean multiple, gint64 closed_at, gpointer user_data)
 *   Emitted when user clicks "Create Poll" button with valid poll data.
 * - "cancelled" ()
 *   Emitted when user cancels poll creation.
 */

typedef struct _GnostrPollComposer GnostrPollComposer;

/**
 * Create a new poll composer widget
 */
GnostrPollComposer *gnostr_poll_composer_new(void);

/**
 * Get the poll question text
 */
const char *gnostr_poll_composer_get_question(GnostrPollComposer *self);

/**
 * Set the poll question text
 */
void gnostr_poll_composer_set_question(GnostrPollComposer *self, const char *question);

/**
 * Get the poll options
 * Returns a GPtrArray of strings (option texts). Caller must free with g_ptr_array_unref().
 * Empty options are filtered out.
 */
GPtrArray *gnostr_poll_composer_get_options(GnostrPollComposer *self);

/**
 * Get whether this is a multiple choice poll
 */
gboolean gnostr_poll_composer_is_multiple_choice(GnostrPollComposer *self);

/**
 * Set whether this is a multiple choice poll
 */
void gnostr_poll_composer_set_multiple_choice(GnostrPollComposer *self, gboolean multiple);

/**
 * Get the closing time (Unix timestamp, 0 = no closing time)
 */
gint64 gnostr_poll_composer_get_closed_at(GnostrPollComposer *self);

/**
 * Set the closing time (Unix timestamp, 0 = no closing time)
 */
void gnostr_poll_composer_set_closed_at(GnostrPollComposer *self, gint64 closed_at);

/**
 * Clear all fields and reset to default state
 */
void gnostr_poll_composer_clear(GnostrPollComposer *self);

/**
 * Check if the poll is valid (has question and at least 2 options)
 */
gboolean gnostr_poll_composer_is_valid(GnostrPollComposer *self);

G_END_DECLS

#endif /* GNOSTR_POLL_COMPOSER_H */
