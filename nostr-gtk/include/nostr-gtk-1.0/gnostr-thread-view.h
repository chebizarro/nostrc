#ifndef NOSTR_GTK_THREAD_VIEW_H
#define NOSTR_GTK_THREAD_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define NOSTR_GTK_TYPE_THREAD_VIEW (nostr_gtk_thread_view_get_type())
G_DECLARE_FINAL_TYPE(NostrGtkThreadView, nostr_gtk_thread_view, NOSTR_GTK, THREAD_VIEW, GtkWidget)

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
 * NostrGtkThreadView:
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

GtkWidget *nostr_gtk_thread_view_new(void);

/**
 * nostr_gtk_thread_view_set_focus_event:
 * @self: a #NostrGtkThreadView
 * @event_id_hex: the 64-character hex event ID to focus on
 *
 * Sets the focus event for the thread view. This triggers loading of
 * the full thread context: parent notes above and replies below.
 */
void nostr_gtk_thread_view_set_focus_event(NostrGtkThreadView *self, const char *event_id_hex);

/**
 * nostr_gtk_thread_view_set_focus_event_with_json:
 * @self: a #NostrGtkThreadView
 * @event_id_hex: the 64-character hex event ID to focus on
 * @event_json: (nullable): optional JSON of the focus event to avoid nostrdb lookup
 *
 * Sets the focus event with optional JSON data. When event_json is provided,
 * the event is added directly to the view without requiring nostrdb lookup.
 * This is useful when the event is already in memory (e.g., from timeline).
 */
void nostr_gtk_thread_view_set_focus_event_with_json(NostrGtkThreadView *self,
                                                   const char *event_id_hex,
                                                   const char *event_json);

/**
 * nostr_gtk_thread_view_set_thread_root:
 * @self: a #NostrGtkThreadView
 * @root_event_id_hex: the 64-character hex event ID of the thread root
 *
 * Sets the thread root event. All notes in the thread share this root.
 * If different from focus_event, the root will be shown at the top.
 */
void nostr_gtk_thread_view_set_thread_root(NostrGtkThreadView *self, const char *root_event_id_hex);

/**
 * nostr_gtk_thread_view_set_thread_root_with_json:
 * @self: a #NostrGtkThreadView
 * @root_event_id_hex: the 64-character hex event ID of the thread root
 * @event_json: (nullable): optional JSON of the root event to avoid nostrdb lookup
 *
 * Sets the thread root with optional JSON data. When event_json is provided,
 * the event is added directly to the view without requiring nostrdb lookup.
 */
void nostr_gtk_thread_view_set_thread_root_with_json(NostrGtkThreadView *self,
                                                   const char *root_event_id_hex,
                                                   const char *event_json);

/**
 * nostr_gtk_thread_view_clear:
 * @self: a #NostrGtkThreadView
 *
 * Clears the thread view and cancels any pending network requests.
 */
void nostr_gtk_thread_view_clear(NostrGtkThreadView *self);

/**
 * nostr_gtk_thread_view_refresh:
 * @self: a #NostrGtkThreadView
 *
 * Refreshes the thread by re-querying nostrdb and relays.
 */
void nostr_gtk_thread_view_refresh(NostrGtkThreadView *self);

/**
 * nostr_gtk_thread_view_get_focus_event_id:
 * @self: a #NostrGtkThreadView
 *
 * Returns: (transfer none): the hex event ID of the focus note, or NULL
 */
const char *nostr_gtk_thread_view_get_focus_event_id(NostrGtkThreadView *self);

/**
 * nostr_gtk_thread_view_get_thread_root_id:
 * @self: a #NostrGtkThreadView
 *
 * Returns: (transfer none): the hex event ID of the thread root, or NULL
 */
const char *nostr_gtk_thread_view_get_thread_root_id(NostrGtkThreadView *self);

/**
 * nostr_gtk_thread_view_update_profiles:
 * @self: a #NostrGtkThreadView
 *
 * Updates profile information for displayed notes by re-checking
 * the profile provider cache. Call this after profiles have been
 * fetched from relays.
 */
void nostr_gtk_thread_view_update_profiles(NostrGtkThreadView *self);

/**
 * nostr_gtk_thread_view_toggle_branch:
 * @self: a #NostrGtkThreadView
 * @event_id_hex: the event ID of the branch root to toggle
 *
 * Toggles the collapsed state of a thread branch.
 * When collapsed, child replies are hidden and a count indicator is shown.
 */
void nostr_gtk_thread_view_toggle_branch(NostrGtkThreadView *self, const char *event_id_hex);

/**
 * nostr_gtk_thread_view_expand_all:
 * @self: a #NostrGtkThreadView
 *
 * Expands all collapsed branches in the thread view.
 */
void nostr_gtk_thread_view_expand_all(NostrGtkThreadView *self);

/**
 * nostr_gtk_thread_view_collapse_non_focus:
 * @self: a #NostrGtkThreadView
 *
 * Collapses all branches not on the focus path (path from focus event to root).
 * The focus path remains expanded for easy reading.
 */
void nostr_gtk_thread_view_collapse_non_focus(NostrGtkThreadView *self);

G_END_DECLS

#endif /* NOSTR_GTK_THREAD_VIEW_H */
