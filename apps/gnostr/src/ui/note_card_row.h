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
 * "like-requested" (gchar* id_hex, gchar* pubkey_hex, gint event_kind, gchar* reaction_content, gpointer user_data) - NIP-25 reaction
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
 * "highlight-requested" (gchar* highlighted_text, gchar* context, gchar* id_hex, gchar* pubkey_hex, gpointer user_data) - NIP-84 highlight request
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

/* NIP-25 Reactions: set event kind for proper k-tag in reaction events */
void gnostr_note_card_row_set_event_kind(GnostrNoteCardRow *self, gint kind);

/* NIP-25 Reactions: set reaction breakdown with emoji counts
 * @breakdown: GHashTable of emoji (string) -> count (guint via GPOINTER_TO_UINT) */
void gnostr_note_card_row_set_reaction_breakdown(GnostrNoteCardRow *self, GHashTable *breakdown);

/* NIP-25 Reactions: add a single reaction to the breakdown
 * @emoji: reaction content ("+", "-", or emoji)
 * @reactor_pubkey: pubkey of the user who reacted (nullable) */
void gnostr_note_card_row_add_reaction(GnostrNoteCardRow *self, const char *emoji, const char *reactor_pubkey);

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

/* Hashtags: Set hashtags from "t" tags to display on this note */
void gnostr_note_card_row_set_hashtags(GnostrNoteCardRow *self, const char * const *hashtags);

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

/* NIP-71 Video Events: Transform this card into video display mode
 * @self: note card row
 * @video_url: Video URL from "url" tag (required)
 * @thumb_url: Thumbnail image URL from "thumb" tag (optional)
 * @title: Video title from "title" tag (optional)
 * @summary: Video description from "summary" tag (optional)
 * @duration: Duration in seconds from "duration" tag (0 if unknown)
 * @is_vertical: TRUE for vertical video (kind 34236), FALSE for horizontal (kind 34235)
 * @d_tag: The video's unique identifier from "d" tag (optional)
 * @hashtags: Array of hashtags from "t" tags (optional, NULL-terminated)
 *
 * When called, switches the card to video display mode:
 * - Shows video player with thumbnail overlay
 * - Displays title and duration badge
 * - Shows play button overlay
 * - Supports inline playback and fullscreen
 * - Adapts layout for vertical videos
 */
void gnostr_note_card_row_set_video_mode(GnostrNoteCardRow *self,
                                          const char *video_url,
                                          const char *thumb_url,
                                          const char *title,
                                          const char *summary,
                                          gint64 duration,
                                          gboolean is_vertical,
                                          const char *d_tag,
                                          const char * const *hashtags);

/* NIP-71: Check if this card is displaying a video */
gboolean gnostr_note_card_row_is_video(GnostrNoteCardRow *self);

/* NIP-71: Get the video's d-tag identifier */
const char *gnostr_note_card_row_get_video_d_tag(GnostrNoteCardRow *self);

/* NIP-71: Get the video URL */
const char *gnostr_note_card_row_get_video_url(GnostrNoteCardRow *self);

/* NIP-84 Highlights: Enable text selection mode for highlighting */
void gnostr_note_card_row_enable_text_selection(GnostrNoteCardRow *self, gboolean enable);

/* NIP-84 Highlights: Get the note's content text (for context extraction) */
const char *gnostr_note_card_row_get_content_text(GnostrNoteCardRow *self);

/* NIP-84 Highlights: Get the note's event ID */
const char *gnostr_note_card_row_get_event_id(GnostrNoteCardRow *self);

/* NIP-84 Highlights: Get the note author's pubkey */
const char *gnostr_note_card_row_get_pubkey(GnostrNoteCardRow *self);

/* NIP-48 Proxy Tags: Set proxy information for bridged content
 * @self: note card row
 * @proxy_id: The original source identifier (URL or ID)
 * @protocol: The source protocol name (activitypub, atproto, rss, web, etc.)
 *
 * When called, displays a "via Protocol" indicator showing that this note
 * was bridged from another platform. If the proxy_id is a URL, it becomes
 * a clickable link to the original source.
 */
void gnostr_note_card_row_set_proxy_info(GnostrNoteCardRow *self,
                                          const char *proxy_id,
                                          const char *protocol);

/* NIP-48 Proxy Tags: Set proxy information from event tags JSON
 * @self: note card row
 * @tags_json: JSON array string of event tags
 *
 * Parses the "proxy" tag (if present) and displays bridged content attribution.
 * Equivalent to calling set_proxy_info with extracted values.
 */
void gnostr_note_card_row_set_proxy_from_tags(GnostrNoteCardRow *self,
                                               const char *tags_json);

/* NIP-48: Check if this note is bridged from another protocol */
gboolean gnostr_note_card_row_is_proxied(GnostrNoteCardRow *self);

/* NIP-48: Get the proxy source protocol (NULL if not proxied) */
const char *gnostr_note_card_row_get_proxy_protocol(GnostrNoteCardRow *self);

/* NIP-48: Get the proxy source ID/URL (NULL if not proxied) */
const char *gnostr_note_card_row_get_proxy_id(GnostrNoteCardRow *self);

/* NIP-03 OpenTimestamps: Set OTS proof from event tags
 * @self: note card row
 * @tags_json: JSON array string of event tags (containing "ots" tag)
 *
 * Parses the "ots" tag (if present) and displays timestamp verification status.
 * Shows a badge indicating whether the note has a verified Bitcoin timestamp.
 */
void gnostr_note_card_row_set_ots_proof(GnostrNoteCardRow *self, const char *tags_json);

/* NIP-03 OpenTimestamps: Set OTS status directly
 * @self: note card row
 * @status: verification status (see GnostrOtsStatus enum in nip03_opentimestamps.h)
 * @verified_timestamp: Unix timestamp of Bitcoin attestation (0 if not verified)
 * @block_height: Bitcoin block height (0 if not verified)
 *
 * Directly sets the OTS display status, useful when proof is already parsed.
 */
void gnostr_note_card_row_set_ots_status(GnostrNoteCardRow *self,
                                          gint status,
                                          gint64 verified_timestamp,
                                          guint block_height);

/* NIP-03 OpenTimestamps: Check if note has OTS proof */
gboolean gnostr_note_card_row_has_ots_proof(GnostrNoteCardRow *self);

/* NIP-03 OpenTimestamps: Get the verification timestamp */
gint64 gnostr_note_card_row_get_ots_timestamp(GnostrNoteCardRow *self);

/* NIP-73 External Content IDs: Set external references from event tags
 * @self: note card row
 * @tags_json: JSON array string of event tags (containing "i" tags for external content)
 *
 * Parses the "i" tags (NIP-73) and displays clickable badges for external content
 * references like ISBN, DOI, IMDB, Spotify, YouTube, etc.
 */
void gnostr_note_card_row_set_external_ids(GnostrNoteCardRow *self, const char *tags_json);

/* NIP-73 External Content IDs: Check if note has external references */
gboolean gnostr_note_card_row_has_external_ids(GnostrNoteCardRow *self);

/* NIP-73 External Content IDs: Clear all external ID badges */
void gnostr_note_card_row_clear_external_ids(GnostrNoteCardRow *self);

G_END_DECLS

#endif /* GNOSTR_NOTE_CARD_ROW_H */
