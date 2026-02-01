#ifndef GNOSTR_THREAD_VIEW_H
#define GNOSTR_THREAD_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_THREAD_VIEW (gnostr_thread_view_get_type())
G_DECLARE_FINAL_TYPE(GnostrThreadView, gnostr_thread_view, GNOSTR, THREAD_VIEW, GtkWidget)

/**
 * ThreadNode:
 *
 * A node in the thread graph representing a single event and its
 * relationships. Used internally for bidirectional graph traversal.
 */
typedef struct _ThreadNode ThreadNode;

/**
 * ThreadGraph:
 *
 * Complete graph representation of a thread conversation.
 * Enables bidirectional traversal (parents, children, siblings).
 */
typedef struct _ThreadGraph ThreadGraph;

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

/**
 * gnostr_thread_view_update_profiles:
 * @self: a #GnostrThreadView
 *
 * Updates profile information for displayed notes by re-checking
 * the profile provider cache. Call this after profiles have been
 * fetched from relays.
 */
void gnostr_thread_view_update_profiles(GnostrThreadView *self);

/**
 * gnostr_thread_view_toggle_branch:
 * @self: a #GnostrThreadView
 * @event_id_hex: the event ID of the branch root to toggle
 *
 * Toggles the collapsed state of a thread branch.
 * When collapsed, child replies are hidden and a count indicator is shown.
 */
void gnostr_thread_view_toggle_branch(GnostrThreadView *self, const char *event_id_hex);

/**
 * gnostr_thread_view_expand_all:
 * @self: a #GnostrThreadView
 *
 * Expands all collapsed branches in the thread view.
 */
void gnostr_thread_view_expand_all(GnostrThreadView *self);

/**
 * gnostr_thread_view_collapse_non_focus:
 * @self: a #GnostrThreadView
 *
 * Collapses all branches not on the focus path (path from focus event to root).
 * The focus path remains expanded for easy reading.
 */
void gnostr_thread_view_collapse_non_focus(GnostrThreadView *self);

G_END_DECLS

#endif /* GNOSTR_THREAD_VIEW_H */
