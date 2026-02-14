#ifndef NOSTR_GTK_NOSTR_NOTE_CARD_ROW_H
#define NOSTR_GTK_NOSTR_NOTE_CARD_ROW_H

/**
 * SECTION:nostr-note-card-row
 * @short_description: Single-note display widget
 * @title: NostrNoteCardRow
 *
 * A fundamental NIP-01 event rendering component that displays a single
 * Nostr note with author info, content, media, actions, and metadata.
 *
 * Formerly GnostrNoteCardRow in the app layer.
 */

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Forward declaration — full definition in content_renderer.h (app-provided).
 * The actual struct is an anonymous typedef; we declare a compatible tag here
 * so consumers can use pointers without the full definition. */
typedef struct GnContentRenderResult GnContentRenderResult;

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
 * "pin-toggled" (gchar* id_hex, gboolean is_pinned, gpointer user_data)
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
 * gnostr_note_card_row_set_author_name_only:
 * @self: note card row
 * @display_name: (nullable): author display name
 * @handle: (nullable): author handle (e.g., "@alice")
 *
 * Tier 1 bind helper. Sets ONLY the display name and handle labels,
 * without triggering avatar loading. Avatar loading is deferred to
 * Tier 2 (map signal) via gnostr_note_card_row_set_avatar().
 */
void gnostr_note_card_row_set_author_name_only(GnostrNoteCardRow *self,
                                                const char *display_name,
                                                const char *handle);

/**
 * gnostr_note_card_row_set_avatar:
 * @self: note card row
 * @avatar_url: (nullable): URL for the avatar image
 *
 * Tier 2 deferred avatar loading. Loads the avatar from cache
 * or initiates an async download.
 */
void gnostr_note_card_row_set_avatar(GnostrNoteCardRow *self,
                                      const char *avatar_url);

/**
 * gnostr_note_card_row_set_content_markup_only:
 * @self: note card row
 * @content: (nullable): raw text content (for clipboard)
 * @render: (transfer none): pre-rendered content result
 *
 * Tier 1 bind helper. Sets ONLY the Pango markup label from a cached
 * render result, without creating media widgets, OG previews, or note
 * embeds. Those are deferred to Tier 2.
 */
void gnostr_note_card_row_set_content_markup_only(GnostrNoteCardRow *self,
                                                   const char *content,
                                                   const GnContentRenderResult *render);

/**
 * gnostr_note_card_row_apply_deferred_content:
 * @self: note card row
 * @render: (transfer none): pre-rendered content result
 *
 * Tier 2 deferred content. Creates media widgets, OG previews,
 * and note embeds from the cached render result.
 */
void gnostr_note_card_row_apply_deferred_content(GnostrNoteCardRow *self,
                                                  const GnContentRenderResult *render);

/**
 * gnostr_note_card_row_set_content_rendered:
 * @self: note card row
 * @content: raw text content (for clipboard; may be NULL if not needed)
 * @render: (transfer none): pre-rendered content result (Pango markup + media URLs)
 *
 * Sets note content from a pre-rendered GnContentRenderResult,
 * skipping the expensive gnostr_render_content() call.
 */
void gnostr_note_card_row_set_content_rendered(GnostrNoteCardRow *self,
                                                const char *content,
                                                const GnContentRenderResult *render);

/**
 * gnostr_note_card_row_set_content_with_imeta:
 * @self: note card row
 * @content: text content of the note
 * @tags_json: (nullable): JSON array string of event tags for NIP-92 imeta parsing
 *
 * Sets the note content and parses imeta tags for enhanced media display.
 */
void gnostr_note_card_row_set_content_with_imeta(GnostrNoteCardRow *self, const char *content, const char *tags_json);

void gnostr_note_card_row_set_depth(GnostrNoteCardRow *self, guint depth);
void gnostr_note_card_row_set_ids(GnostrNoteCardRow *self, const char *id_hex, const char *root_id, const char *pubkey_hex);
void gnostr_note_card_row_set_embed(GnostrNoteCardRow *self, const char *title, const char *snippet);
void gnostr_note_card_row_set_embed_rich(GnostrNoteCardRow *self, const char *title, const char *meta, const char *snippet);

void gnostr_note_card_row_set_nip05(GnostrNoteCardRow *self, const char *nip05, const char *pubkey_hex);

void gnostr_note_card_row_set_thread_info(GnostrNoteCardRow *self,
                                          const char *root_id,
                                          const char *parent_id,
                                          const char *parent_author_name,
                                          gboolean is_reply);

void gnostr_note_card_row_set_pinned(GnostrNoteCardRow *self, gboolean is_pinned);
void gnostr_note_card_row_set_bookmarked(GnostrNoteCardRow *self, gboolean is_bookmarked);
void gnostr_note_card_row_set_liked(GnostrNoteCardRow *self, gboolean is_liked);
void gnostr_note_card_row_set_like_count(GnostrNoteCardRow *self, guint count);
void gnostr_note_card_row_set_event_kind(GnostrNoteCardRow *self, gint kind);
void gnostr_note_card_row_set_reaction_breakdown(GnostrNoteCardRow *self, GHashTable *breakdown);
void gnostr_note_card_row_add_reaction(GnostrNoteCardRow *self, const char *emoji, const char *reactor_pubkey);
void gnostr_note_card_row_set_author_lud16(GnostrNoteCardRow *self, const char *lud16);
void gnostr_note_card_row_set_zap_stats(GnostrNoteCardRow *self, guint zap_count, gint64 total_msat);
void gnostr_note_card_row_set_reply_count(GnostrNoteCardRow *self, guint count);
void gnostr_note_card_row_set_is_own_note(GnostrNoteCardRow *self, gboolean is_own);
void gnostr_note_card_row_set_logged_in(GnostrNoteCardRow *self, gboolean logged_in);

void gnostr_note_card_row_set_repost_info(GnostrNoteCardRow *self,
                                          const char *reposter_pubkey_hex,
                                          const char *reposter_display_name,
                                          gint64 repost_created_at);
void gnostr_note_card_row_set_is_repost(GnostrNoteCardRow *self, gboolean is_repost);
void gnostr_note_card_row_set_repost_count(GnostrNoteCardRow *self, guint count);

void gnostr_note_card_row_set_quote_info(GnostrNoteCardRow *self,
                                         const char *quoted_event_id_hex,
                                         const char *quoted_content,
                                         const char *quoted_author_name);

void gnostr_note_card_row_set_zap_receipt_info(GnostrNoteCardRow *self,
                                                const char *sender_pubkey,
                                                const char *sender_display_name,
                                                const char *recipient_pubkey,
                                                const char *recipient_display_name,
                                                const char *target_event_id,
                                                gint64 amount_msat);
void gnostr_note_card_row_set_is_zap_receipt(GnostrNoteCardRow *self, gboolean is_zap);

void gnostr_note_card_row_set_content_warning(GnostrNoteCardRow *self,
                                              const char *content_warning_reason);
gboolean gnostr_note_card_row_is_content_blurred(GnostrNoteCardRow *self);
void gnostr_note_card_row_reveal_sensitive_content(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_hashtags(GnostrNoteCardRow *self, const char *const *hashtags);

void gnostr_note_card_row_set_relay_info(GnostrNoteCardRow *self,
                                          const char *const *relay_urls);

void gnostr_note_card_row_set_labels(GnostrNoteCardRow *self, GPtrArray *labels);
void gnostr_note_card_row_add_label(GnostrNoteCardRow *self, const char *namespace, const char *label);
void gnostr_note_card_row_clear_labels(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_article_mode(GnostrNoteCardRow *self,
                                           const char *title,
                                           const char *summary,
                                           const char *image_url,
                                           gint64 published_at,
                                           const char *d_tag,
                                           const char *const *hashtags);
gboolean gnostr_note_card_row_is_article(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_article_d_tag(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_video_mode(GnostrNoteCardRow *self,
                                         const char *video_url,
                                         const char *thumb_url,
                                         const char *title,
                                         const char *summary,
                                         gint64 duration,
                                         gboolean is_vertical,
                                         const char *d_tag,
                                         const char *const *hashtags);
gboolean gnostr_note_card_row_is_video(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_video_d_tag(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_video_url(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_git_repo_mode(GnostrNoteCardRow *self,
                                             const char *name,
                                             const char *description,
                                             const char *const *clone_urls,
                                             const char *const *web_urls,
                                             const char *const *topics,
                                             gsize maintainer_count,
                                             const char *license);
void gnostr_note_card_row_set_git_patch_mode(GnostrNoteCardRow *self,
                                              const char *title,
                                              const char *repo_name,
                                              const char *commit_id);
void gnostr_note_card_row_set_git_issue_mode(GnostrNoteCardRow *self,
                                              const char *title,
                                              const char *repo_name,
                                              gboolean is_open,
                                              const char *const *labels);
gboolean gnostr_note_card_row_is_git_event(GnostrNoteCardRow *self);

void gnostr_note_card_row_enable_text_selection(GnostrNoteCardRow *self, gboolean enable);
const char *gnostr_note_card_row_get_content_text(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_event_id(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_pubkey(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_proxy_info(GnostrNoteCardRow *self,
                                         const char *proxy_id,
                                         const char *protocol);
void gnostr_note_card_row_set_proxy_from_tags(GnostrNoteCardRow *self,
                                              const char *tags_json);
gboolean gnostr_note_card_row_is_proxied(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_proxy_protocol(GnostrNoteCardRow *self);
const char *gnostr_note_card_row_get_proxy_id(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_ots_proof(GnostrNoteCardRow *self, const char *tags_json);
void gnostr_note_card_row_set_ots_status(GnostrNoteCardRow *self,
                                         gint status,
                                         gint64 verified_timestamp,
                                         guint block_height);
gboolean gnostr_note_card_row_has_ots_proof(GnostrNoteCardRow *self);
gint64 gnostr_note_card_row_get_ots_timestamp(GnostrNoteCardRow *self);

void gnostr_note_card_row_set_external_ids(GnostrNoteCardRow *self, const char *tags_json);
gboolean gnostr_note_card_row_has_external_ids(GnostrNoteCardRow *self);
void gnostr_note_card_row_clear_external_ids(GnostrNoteCardRow *self);

GCancellable *gnostr_note_card_row_get_cancellable(GnostrNoteCardRow *self);
void gnostr_note_card_row_prepare_for_bind(GnostrNoteCardRow *self);
void gnostr_note_card_row_prepare_for_unbind(GnostrNoteCardRow *self);
gboolean gnostr_note_card_row_is_disposed(GnostrNoteCardRow *self);
gboolean gnostr_note_card_row_is_bound(GnostrNoteCardRow *self);
guint64 gnostr_note_card_row_get_binding_id(GnostrNoteCardRow *self);

G_END_DECLS
#endif /* NOSTR_GTK_NOSTR_NOTE_CARD_ROW_H */
