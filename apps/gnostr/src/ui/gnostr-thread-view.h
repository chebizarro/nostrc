#ifndef GNOSTR_THREAD_VIEW_H
#define GNOSTR_THREAD_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_THREAD_VIEW (gnostr_thread_view_get_type())
G_DECLARE_FINAL_TYPE(GnostrThreadView, gnostr_thread_view, GNOSTR, THREAD_VIEW, GtkWidget)

/**
 * GnostrThreadView:
 *
 * A widget that displays a full threaded conversation.
 * Shows parent notes above the focus note and replies below it,
 * with visual hierarchy using indentation and connecting lines.
 *
 * Signals:
 * - "close-requested" - emitted when user wants to close the thread view
 * - "note-activated" - emitted when user clicks on a note (const char *event_id)
 * - "open-profile" - emitted when user clicks on an author (const char *pubkey_hex)
 */

GtkWidget *gnostr_thread_view_new(void);

/**
 * gnostr_thread_view_set_focus_event:
 * @self: a #GnostrThreadView
 * @event_id_hex: the 64-character hex event ID to focus on
 *
 * Sets the focus event for the thread view. This triggers loading of
 * the full thread context: parent notes above and replies below.
 */
void gnostr_thread_view_set_focus_event(GnostrThreadView *self, const char *event_id_hex);

/**
 * gnostr_thread_view_set_thread_root:
 * @self: a #GnostrThreadView
 * @root_event_id_hex: the 64-character hex event ID of the thread root
 *
 * Sets the thread root event. All notes in the thread share this root.
 * If different from focus_event, the root will be shown at the top.
 */
void gnostr_thread_view_set_thread_root(GnostrThreadView *self, const char *root_event_id_hex);

/**
 * gnostr_thread_view_clear:
 * @self: a #GnostrThreadView
 *
 * Clears the thread view and cancels any pending network requests.
 */
void gnostr_thread_view_clear(GnostrThreadView *self);

/**
 * gnostr_thread_view_refresh:
 * @self: a #GnostrThreadView
 *
 * Refreshes the thread by re-querying nostrdb and relays.
 */
void gnostr_thread_view_refresh(GnostrThreadView *self);

/**
 * gnostr_thread_view_get_focus_event_id:
 * @self: a #GnostrThreadView
 *
 * Returns: (transfer none): the hex event ID of the focus note, or NULL
 */
const char *gnostr_thread_view_get_focus_event_id(GnostrThreadView *self);

/**
 * gnostr_thread_view_get_thread_root_id:
 * @self: a #GnostrThreadView
 *
 * Returns: (transfer none): the hex event ID of the thread root, or NULL
 */
const char *gnostr_thread_view_get_thread_root_id(GnostrThreadView *self);

G_END_DECLS

#endif /* GNOSTR_THREAD_VIEW_H */
