/*
 * gnostr-thread-card.h - NIP-7D Forum Thread Card Widget
 *
 * Displays a kind 11 thread root event in a card format suitable for
 * thread listing views. Shows:
 * - Thread subject/title
 * - Preview of thread content
 * - Reply count badge
 * - Last activity timestamp
 * - Author info with avatar
 * - Category/hashtag pills
 *
 * This widget is used in forum/thread listing views to display
 * thread summaries. Clicking opens the full thread view.
 */

#ifndef GNOSTR_THREAD_CARD_H
#define GNOSTR_THREAD_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_THREAD_CARD (gnostr_thread_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrThreadCard, gnostr_thread_card, GNOSTR, THREAD_CARD, GtkWidget)

/*
 * Signals:
 *
 * "thread-clicked" (gchar* event_id_hex, gpointer user_data)
 *    - Emitted when the user clicks the thread card to view it
 *    - event_id_hex is the thread root event ID
 *
 * "author-clicked" (gchar* pubkey_hex, gpointer user_data)
 *    - Emitted when the user clicks the author's name or avatar
 *
 * "reply-clicked" (gchar* event_id_hex, gpointer user_data)
 *    - Emitted when the user clicks the reply button
 *
 * "hashtag-clicked" (gchar* hashtag, gpointer user_data)
 *    - Emitted when the user clicks a hashtag pill
 */

typedef struct _GnostrThreadCard GnostrThreadCard;

/*
 * gnostr_thread_card_new:
 *
 * Creates a new thread card widget.
 *
 * Returns: (transfer full): A new #GnostrThreadCard
 */
GnostrThreadCard *gnostr_thread_card_new(void);

/*
 * gnostr_thread_card_set_thread:
 * @self: The thread card
 * @event_id: Thread event ID (hex, 64 chars)
 * @subject: Thread subject/title
 * @content_preview: Preview text (first ~200 chars of content)
 * @created_at: Thread creation timestamp
 *
 * Sets the main thread information.
 */
void gnostr_thread_card_set_thread(GnostrThreadCard *self,
                                    const char *event_id,
                                    const char *subject,
                                    const char *content_preview,
                                    gint64 created_at);

/*
 * gnostr_thread_card_set_author:
 * @self: The thread card
 * @pubkey_hex: Author's public key (hex, 64 chars)
 * @display_name: (nullable): Display name to show
 * @handle: (nullable): @ handle (e.g., "@alice")
 * @avatar_url: (nullable): Avatar image URL
 *
 * Sets the thread author information.
 */
void gnostr_thread_card_set_author(GnostrThreadCard *self,
                                    const char *pubkey_hex,
                                    const char *display_name,
                                    const char *handle,
                                    const char *avatar_url);

/*
 * gnostr_thread_card_set_reply_count:
 * @self: The thread card
 * @count: Number of replies
 *
 * Sets the reply count badge.
 */
void gnostr_thread_card_set_reply_count(GnostrThreadCard *self,
                                         guint count);

/*
 * gnostr_thread_card_set_last_activity:
 * @self: The thread card
 * @timestamp: Unix timestamp of last reply or activity
 *
 * Sets the last activity timestamp for display.
 */
void gnostr_thread_card_set_last_activity(GnostrThreadCard *self,
                                           gint64 timestamp);

/*
 * gnostr_thread_card_set_hashtags:
 * @self: The thread card
 * @hashtags: (nullable): NULL-terminated array of hashtag strings
 *
 * Sets the hashtag pills to display below the content preview.
 */
void gnostr_thread_card_set_hashtags(GnostrThreadCard *self,
                                      const char * const *hashtags);

/*
 * gnostr_thread_card_add_hashtag:
 * @self: The thread card
 * @hashtag: Hashtag string (without # prefix)
 *
 * Adds a single hashtag pill.
 */
void gnostr_thread_card_add_hashtag(GnostrThreadCard *self,
                                     const char *hashtag);

/*
 * gnostr_thread_card_clear_hashtags:
 * @self: The thread card
 *
 * Clears all hashtag pills.
 */
void gnostr_thread_card_clear_hashtags(GnostrThreadCard *self);

/*
 * gnostr_thread_card_set_nip05:
 * @self: The thread card
 * @nip05: (nullable): NIP-05 identifier (e.g., "user@domain.com")
 * @pubkey_hex: Public key for verification
 *
 * Sets and initiates NIP-05 verification for the author.
 */
void gnostr_thread_card_set_nip05(GnostrThreadCard *self,
                                   const char *nip05,
                                   const char *pubkey_hex);

/*
 * gnostr_thread_card_set_logged_in:
 * @self: The thread card
 * @logged_in: TRUE if user is logged in
 *
 * Sets the login state (affects reply button sensitivity).
 */
void gnostr_thread_card_set_logged_in(GnostrThreadCard *self,
                                       gboolean logged_in);

/*
 * gnostr_thread_card_get_event_id:
 * @self: The thread card
 *
 * Gets the thread event ID.
 *
 * Returns: (transfer none) (nullable): The event ID hex string
 */
const char *gnostr_thread_card_get_event_id(GnostrThreadCard *self);

/*
 * gnostr_thread_card_get_author_pubkey:
 * @self: The thread card
 *
 * Gets the thread author's public key.
 *
 * Returns: (transfer none) (nullable): The pubkey hex string
 */
const char *gnostr_thread_card_get_author_pubkey(GnostrThreadCard *self);

/*
 * gnostr_thread_card_get_subject:
 * @self: The thread card
 *
 * Gets the thread subject.
 *
 * Returns: (transfer none) (nullable): The subject string
 */
const char *gnostr_thread_card_get_subject(GnostrThreadCard *self);

G_END_DECLS

#endif /* GNOSTR_THREAD_CARD_H */
