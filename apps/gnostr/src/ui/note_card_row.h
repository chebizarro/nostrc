#ifndef GNOSTR_NOTE_CARD_ROW_H
#define GNOSTR_NOTE_CARD_ROW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NOTE_CARD_ROW (gnostr_note_card_row_get_type())

G_DECLARE_FINAL_TYPE(GnostrNoteCardRow, gnostr_note_card_row, GNOSTR, NOTE_CARD_ROW, GtkWidget)

/* Signals
 * "open-nostr-target" (gchar* target, gpointer user_data)
 * "open-url" (gchar* url, gpointer user_data)
 * "request-embed" (gchar* target, gpointer user_data)
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 * "reply-requested" (gchar* id_hex, gchar* root_id, gchar* pubkey_hex, gpointer user_data)
 * "repost-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data)
 * "quote-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data)
 * "like-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data)
 * "zap-requested" (gchar* id_hex, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 * "view-thread-requested" (gchar* root_event_id, gpointer user_data)
 * "mute-user-requested" (gchar* pubkey_hex, gpointer user_data)
 * "mute-thread-requested" (gchar* event_id_hex, gpointer user_data) - mutes the thread root event
 * "show-toast" (gchar* message, gpointer user_data) - requests toast notification display
 * "bookmark-toggled" (gchar* id_hex, gboolean is_bookmarked, gpointer user_data)
 * "report-note-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data) - NIP-56 report request
 * "share-note-requested" (gchar* nostr_uri, gpointer user_data) - share note URI
 * "search-hashtag" (gchar* hashtag, gpointer user_data) - search for hashtag (without # prefix)
 * "navigate-to-note" (gchar* event_id_hex, gpointer user_data) - navigate to specific note (e.g., parent note)
 * "delete-note-requested" (gchar* id_hex, gchar* pubkey_hex, gpointer user_data) - NIP-09 deletion request
 * "comment-requested" (gchar* id_hex, gint kind, gchar* pubkey_hex, gpointer user_data) - NIP-22 comment request
 */

typedef struct _GnostrNoteCardRow GnostrNoteCardRow;

GnostrNoteCardRow *gnostr_note_card_row_new(void);

void gnostr_note_card_row_set_author(GnostrNoteCardRow *self, const char *display_name, const char *handle, const char *avatar_url);
void gnostr_note_card_row_set_timestamp(GnostrNoteCardRow *self, gint64 created_at, const char *fallback_ts);
void gnostr_note_card_row_set_content(GnostrNoteCardRow *self, const char *content);

/**
 * gnostr_note_card_row_set_content_with_imeta:
 * @self: note card row
 * @content: text content of the note
 * @tags_json: (nullable): JSON array string of event tags for NIP-92 imeta parsing
 *
 * Sets the note content and parses imeta tags for enhanced media display.
 * When tags_json is provided, media URLs in content will use metadata from
 * matching imeta tags (dimensions, alt text, blurhash placeholder, etc.).
 */
void gnostr_note_card_row_set_content_with_imeta(GnostrNoteCardRow *self, const char *content, const char *tags_json);

void gnostr_note_card_row_set_depth(GnostrNoteCardRow *self, guint depth);
void gnostr_note_card_row_set_ids(GnostrNoteCardRow *self, const char *id_hex, const char *root_id, const char *pubkey_hex);
void gnostr_note_card_row_set_embed(GnostrNoteCardRow *self, const char *title, const char *snippet);
/* Rich embed variant: title (e.g., Note), meta (e.g., author · time), snippet (content excerpt) */
void gnostr_note_card_row_set_embed_rich(GnostrNoteCardRow *self, const char *title, const char *meta, const char *snippet);

/* NIP-05 verification: set identifier and trigger async verification */
void gnostr_note_card_row_set_nip05(GnostrNoteCardRow *self, const char *nip05, const char *pubkey_hex);

/* NIP-10 threading: set thread info and update reply indicator */
void gnostr_note_card_row_set_thread_info(GnostrNoteCardRow *self,
                                           const char *root_id,
                                           const char *parent_id,
                                           const char *parent_author_name,
                                           gboolean is_reply);

/* Bookmark state: update the bookmark button icon based on state */
void gnostr_note_card_row_set_bookmarked(GnostrNoteCardRow *self, gboolean is_bookmarked);

/* NIP-25 Reactions: update the like button icon based on state */
void gnostr_note_card_row_set_liked(GnostrNoteCardRow *self, gboolean is_liked);

/* NIP-25 Reactions: update the like count display */
void gnostr_note_card_row_set_like_count(GnostrNoteCardRow *self, guint count);

/* NIP-57 Zaps: set author's lightning address for zapping */
void gnostr_note_card_row_set_author_lud16(GnostrNoteCardRow *self, const char *lud16);

/* NIP-57 Zaps: update zap statistics display */
void gnostr_note_card_row_set_zap_stats(GnostrNoteCardRow *self, guint zap_count, gint64 total_msat);

/* NIP-10 Threading: set reply count to show thread root indicator */
void gnostr_note_card_row_set_reply_count(GnostrNoteCardRow *self, guint count);

/* NIP-09 Deletion: set whether this is the current user's own note (enables delete option) */
void gnostr_note_card_row_set_is_own_note(GnostrNoteCardRow *self, gboolean is_own);

/* Login state: set whether user is logged in (disables auth-required buttons when logged out) */
void gnostr_note_card_row_set_logged_in(GnostrNoteCardRow *self, gboolean logged_in);

/* NIP-18 Reposts: set repost information to display "reposted by X" attribution */
void gnostr_note_card_row_set_repost_info(GnostrNoteCardRow *self,
                                           const char *reposter_pubkey_hex,
                                           const char *reposter_display_name,
                                           gint64 repost_created_at);

/* NIP-18 Reposts: set whether this card represents a repost (kind 6/16) */
void gnostr_note_card_row_set_is_repost(GnostrNoteCardRow *self, gboolean is_repost);

/* NIP-18 Reposts: update the repost count display */
void gnostr_note_card_row_set_repost_count(GnostrNoteCardRow *self, guint count);

/* NIP-18 Quote Reposts: set quote post info to display the quoted note inline */
void gnostr_note_card_row_set_quote_info(GnostrNoteCardRow *self,
                                          const char *quoted_event_id_hex,
                                          const char *quoted_content,
                                          const char *quoted_author_name);

/* NIP-36 Sensitive Content: set content-warning for sensitive/NSFW content */
void gnostr_note_card_row_set_content_warning(GnostrNoteCardRow *self,
                                               const char *content_warning_reason);

/* NIP-36: Check if content is currently blurred (sensitive content hidden) */
gboolean gnostr_note_card_row_is_content_blurred(GnostrNoteCardRow *self);

/* NIP-36: Reveal sensitive content (show hidden content) */
void gnostr_note_card_row_reveal_sensitive_content(GnostrNoteCardRow *self);

/* NIP-32 Labels: Set labels to display on this note */
void gnostr_note_card_row_set_labels(GnostrNoteCardRow *self, GPtrArray *labels);

/* NIP-32 Labels: Add a single label to this note's display */
void gnostr_note_card_row_add_label(GnostrNoteCardRow *self, const char *namespace, const char *label);

/* NIP-32 Labels: Clear all displayed labels */
void gnostr_note_card_row_clear_labels(GnostrNoteCardRow *self);

/* NIP-23 Long-form Content: Transform this card into article display mode
 * @self: note card row
 * @title: Article title from "title" tag
 * @summary: Article summary from "summary" tag (optional)
 * @image_url: Header image URL from "image" tag (optional)
 * @published_at: Publication timestamp from "published_at" tag (0 to use created_at)
 * @d_tag: The article's unique identifier from "d" tag
 * @hashtags: Array of hashtags from "t" tags (optional, NULL-terminated)
 *
 * When called, switches the card to article display mode:
 * - Shows title prominently
 * - Displays summary instead of full content
 * - Shows header image if available
 * - Adds "Read more" indication
 * - Uses publication date instead of created_at
 */
void gnostr_note_card_row_set_article_mode(GnostrNoteCardRow *self,
                                            const char *title,
                                            const char *summary,
                                            const char *image_url,
                                            gint64 published_at,
                                            const char *d_tag,
                                            const char * const *hashtags);

/* NIP-23: Check if this card is displaying an article */
gboolean gnostr_note_card_row_is_article(GnostrNoteCardRow *self);

/* NIP-23: Get the article's d-tag identifier */
const char *gnostr_note_card_row_get_article_d_tag(GnostrNoteCardRow *self);

G_END_DECLS

#endif /* GNOSTR_NOTE_CARD_ROW_H */
