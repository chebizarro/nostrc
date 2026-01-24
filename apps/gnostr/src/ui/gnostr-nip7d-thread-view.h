/*
 * gnostr-nip7d-thread-view.h - NIP-7D Forum Thread Full View Widget
 *
 * Displays a complete NIP-7D thread with:
 * - Thread title and full content at top
 * - Threaded replies with indentation (kind 1111)
 * - Reply composer at bottom
 * - Collapse/expand for long threads
 * - Load more replies pagination
 *
 * This is distinct from gnostr-thread-view.h which handles NIP-10
 * reply chains for regular notes. This widget is specifically for
 * forum-style threaded discussions using kind 11 and 1111 events.
 *
 * Signals:
 * - "close-requested" - User wants to close the view
 * - "author-clicked" (gchar* pubkey_hex) - User clicked an author
 * - "reply-submitted" (gchar* content, gchar* parent_id) - User submitted a reply
 * - "hashtag-clicked" (gchar* hashtag) - User clicked a hashtag
 */

#ifndef GNOSTR_NIP7D_THREAD_VIEW_H
#define GNOSTR_NIP7D_THREAD_VIEW_H

#include <gtk/gtk.h>
#include "../util/nip7d_threads.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_NIP7D_THREAD_VIEW (gnostr_nip7d_thread_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrNip7dThreadView, gnostr_nip7d_thread_view, GNOSTR, NIP7D_THREAD_VIEW, GtkWidget)

/*
 * Signals:
 *
 * "close-requested" (gpointer user_data)
 *    - Emitted when user clicks the close/back button
 *
 * "author-clicked" (gchar* pubkey_hex, gpointer user_data)
 *    - Emitted when user clicks an author's name or avatar
 *
 * "reply-submitted" (gchar* content, gchar* parent_id, gpointer user_data)
 *    - Emitted when user submits a reply via the composer
 *    - parent_id is the thread root ID for top-level replies,
 *      or a reply ID for nested replies
 *
 * "hashtag-clicked" (gchar* hashtag, gpointer user_data)
 *    - Emitted when user clicks a hashtag
 *
 * "need-profile" (gchar* pubkey_hex, gpointer user_data)
 *    - Emitted when a profile needs to be fetched from relays
 */

typedef struct _GnostrNip7dThreadView GnostrNip7dThreadView;

/*
 * gnostr_nip7d_thread_view_new:
 *
 * Creates a new thread view widget.
 *
 * Returns: (transfer floating): A new #GnostrNip7dThreadView
 */
GtkWidget *gnostr_nip7d_thread_view_new(void);

/*
 * gnostr_nip7d_thread_view_set_thread:
 * @self: The thread view
 * @thread: (transfer none): The thread data to display
 *
 * Sets the thread to display. This clears any existing replies
 * and shows the thread root content.
 */
void gnostr_nip7d_thread_view_set_thread(GnostrNip7dThreadView *self,
                                          GnostrThread *thread);

/*
 * gnostr_nip7d_thread_view_load_thread:
 * @self: The thread view
 * @event_id_hex: Thread root event ID (hex)
 *
 * Loads a thread by its root event ID. Fetches from local DB
 * first, then relays.
 */
void gnostr_nip7d_thread_view_load_thread(GnostrNip7dThreadView *self,
                                           const char *event_id_hex);

/*
 * gnostr_nip7d_thread_view_add_reply:
 * @self: The thread view
 * @reply: (transfer none): Reply to add
 *
 * Adds a single reply to the thread view in the correct position.
 */
void gnostr_nip7d_thread_view_add_reply(GnostrNip7dThreadView *self,
                                         GnostrThreadReply *reply);

/*
 * gnostr_nip7d_thread_view_add_replies:
 * @self: The thread view
 * @replies: (element-type GnostrThreadReply*): Array of replies to add
 *
 * Adds multiple replies at once, rebuilding the tree structure.
 */
void gnostr_nip7d_thread_view_add_replies(GnostrNip7dThreadView *self,
                                           GPtrArray *replies);

/*
 * gnostr_nip7d_thread_view_clear:
 * @self: The thread view
 *
 * Clears the thread view and any pending operations.
 */
void gnostr_nip7d_thread_view_clear(GnostrNip7dThreadView *self);

/*
 * gnostr_nip7d_thread_view_refresh:
 * @self: The thread view
 *
 * Refreshes the thread by re-fetching from DB and relays.
 */
void gnostr_nip7d_thread_view_refresh(GnostrNip7dThreadView *self);

/*
 * gnostr_nip7d_thread_view_get_thread_id:
 * @self: The thread view
 *
 * Gets the current thread's event ID.
 *
 * Returns: (transfer none) (nullable): The thread event ID
 */
const char *gnostr_nip7d_thread_view_get_thread_id(GnostrNip7dThreadView *self);

/*
 * gnostr_nip7d_thread_view_get_reply_count:
 * @self: The thread view
 *
 * Gets the number of replies currently displayed.
 *
 * Returns: Number of replies
 */
guint gnostr_nip7d_thread_view_get_reply_count(GnostrNip7dThreadView *self);

/*
 * gnostr_nip7d_thread_view_set_reply_parent:
 * @self: The thread view
 * @parent_id: (nullable): Event ID to reply to, or NULL for thread root
 *
 * Sets the parent for the next reply. Called when user clicks
 * "reply" on a specific comment.
 */
void gnostr_nip7d_thread_view_set_reply_parent(GnostrNip7dThreadView *self,
                                                const char *parent_id);

/*
 * gnostr_nip7d_thread_view_set_logged_in:
 * @self: The thread view
 * @logged_in: TRUE if user is logged in
 *
 * Sets the login state (affects composer visibility/sensitivity).
 */
void gnostr_nip7d_thread_view_set_logged_in(GnostrNip7dThreadView *self,
                                             gboolean logged_in);

/*
 * gnostr_nip7d_thread_view_update_profiles:
 * @self: The thread view
 *
 * Updates profile information for displayed thread and replies
 * from the profile cache. Call after profiles have been fetched.
 */
void gnostr_nip7d_thread_view_update_profiles(GnostrNip7dThreadView *self);

/*
 * gnostr_nip7d_thread_view_collapse_reply:
 * @self: The thread view
 * @reply_id: Event ID of the reply to collapse
 * @collapsed: TRUE to collapse, FALSE to expand
 *
 * Collapses or expands a reply and its children.
 */
void gnostr_nip7d_thread_view_collapse_reply(GnostrNip7dThreadView *self,
                                              const char *reply_id,
                                              gboolean collapsed);

/*
 * gnostr_nip7d_thread_view_load_more_replies:
 * @self: The thread view
 * @limit: Maximum number of additional replies to load
 *
 * Loads more replies from relays for pagination.
 */
void gnostr_nip7d_thread_view_load_more_replies(GnostrNip7dThreadView *self,
                                                 guint limit);

/*
 * gnostr_nip7d_thread_view_scroll_to_reply:
 * @self: The thread view
 * @reply_id: Event ID of the reply to scroll to
 *
 * Scrolls the view to show the specified reply.
 */
void gnostr_nip7d_thread_view_scroll_to_reply(GnostrNip7dThreadView *self,
                                               const char *reply_id);

G_END_DECLS

#endif /* GNOSTR_NIP7D_THREAD_VIEW_H */
