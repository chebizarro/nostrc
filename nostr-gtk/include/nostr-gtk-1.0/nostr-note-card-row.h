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
 * Formerly NostrGtkNoteCardRow in the app layer.
 */

#include "nostr-gtk-export.h"
#include <gtk/gtk.h>
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include <nostr-gtk-1.0/gn-nostr-reference-card.h>

G_BEGIN_DECLS

/* Forward declaration — full definition in content_renderer.h (app-provided).
 * The actual struct is an anonymous typedef; we declare a compatible tag here
 * so consumers can use pointers without the full definition. */
typedef struct GnContentRenderResult GnContentRenderResult;
typedef struct GnContentDescriptor GnContentDescriptor;

/**
 * NostrGtkMediaResourceClass:
 *
 * Library-neutral media classes used by the injected texture loader.  The
 * application maps these values to its media service without nostr-gtk linking
 * against app sources.
 */
typedef enum {
  NOSTR_GTK_MEDIA_RESOURCE_INLINE,
  NOSTR_GTK_MEDIA_RESOURCE_OG_IMAGE,
  NOSTR_GTK_MEDIA_RESOURCE_VIDEO_POSTER,
} NostrGtkMediaResourceClass;

typedef void (*NostrGtkMediaTextureReadyFunc)(GdkTexture *texture,
                                               const GError *error,
                                               gpointer user_data);

typedef void (*NostrGtkMediaTextureRequestFunc)(gpointer loader,
                                                 const char *url,
                                                 NostrGtkMediaResourceClass resource_class,
                                                 int target_width,
                                                 int target_height,
                                                 gboolean user_initiated,
                                                 GCancellable *cancellable,
                                                 NostrGtkMediaTextureReadyFunc callback,
                                                 gpointer user_data,
                                                 GDestroyNotify user_data_destroy);

typedef struct _NostrGtkMediaTextureLoader NostrGtkMediaTextureLoader;

/**
 * nostr_gtk_media_texture_loader_new:
 * @loader: (nullable): loader-specific data retained by the interface
 * @request_func: async texture request function
 * @loader_destroy: (nullable): destroys @loader when the final ref is dropped
 *
 * Creates a ref-counted loader interface. If @loader needs reference counting,
 * pass an owned reference and its matching unref function as @loader_destroy.
 */
NOSTR_GTK_API NostrGtkMediaTextureLoader *nostr_gtk_media_texture_loader_new(
    gpointer loader,
    NostrGtkMediaTextureRequestFunc request_func,
    GDestroyNotify loader_destroy);
NOSTR_GTK_API NostrGtkMediaTextureLoader *nostr_gtk_media_texture_loader_ref(
    NostrGtkMediaTextureLoader *loader);
NOSTR_GTK_API void nostr_gtk_media_texture_loader_unref(
    NostrGtkMediaTextureLoader *loader);

#define NOSTR_GTK_TYPE_NOTE_CARD_ROW (nostr_gtk_note_card_row_get_type())

NOSTR_GTK_API G_DECLARE_FINAL_TYPE(NostrGtkNoteCardRow, nostr_gtk_note_card_row, NOSTR_GTK, NOTE_CARD_ROW, GtkWidget)

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
 * "measured-geometry" (gchar* geometry_token, guint64 snapshot_generation, gint width, gint height, gpointer user_data)
 */

typedef struct _NostrGtkNoteCardRow NostrGtkNoteCardRow;

NOSTR_GTK_API NostrGtkNoteCardRow *nostr_gtk_note_card_row_new(void);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_author(NostrGtkNoteCardRow *self, const char *display_name, const char *handle, const char *avatar_url);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_timestamp(NostrGtkNoteCardRow *self, gint64 created_at, const char *fallback_ts);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_content(NostrGtkNoteCardRow *self, const char *content);

/**
 * nostr_gtk_note_card_row_set_reserved_height:
 * @self: note card row
 * @reserved_height: reserved outer row height in pixels, or 0 to clear
 *
 * Reserves at least @reserved_height pixels for the row during measurement so
 * compositor-bound snapshots can allocate stable geometry before content reveal.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_reserved_height(NostrGtkNoteCardRow *self,
                                                 gint reserved_height);

/**
 * nostr_gtk_note_card_row_set_geometry_token:
 * @self: note card row
 * @geometry_token: (nullable): compositor geometry/cache token for this bind
 * @snapshot_generation: snapshot generation associated with @geometry_token
 *
 * Sets the token used when emitting the measured-geometry signal. Changing the
 * token or generation resets the per-bind measurement report guard.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_geometry_token(NostrGtkNoteCardRow *self,
                                                const gchar *geometry_token,
                                                guint64 snapshot_generation);

/**
 * nostr_gtk_note_card_row_set_explicit_footprint_expansion:
 * @self: note card row
 * @allow_expansion: whether explicit user action may grow the row footprint
 *
 * Snapshot-bound rows are fixed to their reserved height by default.  Set this
 * only from deliberate user actions (for example revealing hidden content) to
 * allow GTK's natural height to exceed the reserved footprint.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_explicit_footprint_expansion(NostrGtkNoteCardRow *self,
                                                              gboolean allow_expansion);

NOSTR_GTK_API gint nostr_gtk_note_card_row_get_reserved_height(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const gchar *nostr_gtk_note_card_row_get_geometry_token(NostrGtkNoteCardRow *self);
NOSTR_GTK_API guint64 nostr_gtk_note_card_row_get_snapshot_generation(NostrGtkNoteCardRow *self);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_get_explicit_footprint_expansion(NostrGtkNoteCardRow *self);

/**
 * NostrGtkContentSubstType:
 * @NOSTR_GTK_CONTENT_SUBST_ALT_TEXT: NIP-31 alt text (italic, full opacity)
 * @NOSTR_GTK_CONTENT_SUBST_PLACEHOLDER: No alt text available (italic + dim)
 *
 * Type of content substitution being applied to a note card row.
 */
typedef enum {
  NOSTR_GTK_CONTENT_SUBST_ALT_TEXT,
  NOSTR_GTK_CONTENT_SUBST_PLACEHOLDER
} NostrGtkContentSubstType;

/**
 * nostr_gtk_note_card_row_set_content_substituted:
 * @self: note card row
 * @text: human-readable substitution text to display
 * @type: type of substitution (alt text vs placeholder)
 *
 * Replaces the rendered content with a substituted display string.
 * Used when the event's raw content is not human-readable (e.g., JSON
 * app data). The original content remains accessible via the context menu.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_content_substituted(NostrGtkNoteCardRow *self,
                                                     const gchar *display_text,
                                                     NostrGtkContentSubstType type,
                                                     const gchar *raw_content);

/**
 * nostr_gtk_note_card_row_set_author_name_only:
 * @self: note card row
 * @display_name: (nullable): author display name
 * @handle: (nullable): author handle (e.g., "@alice")
 *
 * Tier 1 bind helper. Sets ONLY the display name and handle labels,
 * without triggering avatar loading. Avatar loading is deferred to
 * Tier 2 (map signal) via nostr_gtk_note_card_row_set_avatar().
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_author_name_only(NostrGtkNoteCardRow *self,
                                                const char *display_name,
                                                const char *handle);

/**
 * nostr_gtk_note_card_row_set_avatar:
 * @self: note card row
 * @avatar_url: (nullable): URL for the avatar image
 *
 * Tier 2 deferred avatar loading. Loads the avatar from cache
 * or initiates an async download.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_avatar(NostrGtkNoteCardRow *self,
                                      const char *avatar_url);

/**
 * nostr_gtk_note_card_row_set_content_markup_only:
 * @self: note card row
 * @content: (nullable): raw text content (for clipboard)
 * @render: (transfer none): pre-rendered content result
 *
 * Tier 1 bind helper. Sets ONLY the Pango markup label from a cached
 * render result, without creating media widgets, OG previews, or note
 * embeds. Those are deferred to Tier 2.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_content_markup_only(NostrGtkNoteCardRow *self,
                                                   const char *content,
                                                   const GnContentRenderResult *render);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_content_tagged_markup_only(NostrGtkNoteCardRow *self,
                                                         const char *content,
                                                         const char *tags_json,
                                                         const GnContentRenderResult *render);

/**
 * nostr_gtk_note_card_row_set_precomputed_markup:
 * @self: note card row
 * @content: (nullable): raw text content for clipboard/copy fallback
 * @markup: (nullable): trusted precomputed Pango markup from an immutable
 *   timeline view model
 *
 * Binds text from compositor-published VM data without parsing content or
 * creating lazy rich widgets during row bind.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_precomputed_markup(NostrGtkNoteCardRow *self,
                                                    const char *content,
                                                    const char *markup);

/**
 * nostr_gtk_note_card_row_set_media_texture_loader:
 * @self: note card row
 * @loader: (nullable): ref-counted media loader interface
 *
 * Injects an application-owned bounded media loader. The row keeps a strong
 * reference for its lifetime. Passing %NULL restores the nostr-gtk fallback.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_media_texture_loader(
    NostrGtkNoteCardRow *self,
    NostrGtkMediaTextureLoader *loader);

/**
 * nostr_gtk_note_card_row_reserve_rich_content:
 * @self: note card row
 * @descriptors: (nullable) (element-type GnContentDescriptor): ordered,
 *   immutable rich-content descriptors borrowed for this call
 * @media_reserved_height: fixed media reservation in pixels
 * @link_preview_reserved_height: fixed link-preview reservation in pixels
 * @embed_reserved_height: fixed event-embed reservation in pixels
 *
 * Builds reusable fixed-size placeholder frames without retaining descriptor
 * data or attaching fetch/render activation. This is the lightweight bind path.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_reserve_rich_content(
    NostrGtkNoteCardRow *self,
    const GPtrArray *descriptors,
    double media_reserved_height,
    double link_preview_reserved_height,
    double embed_reserved_height);

/**
 * nostr_gtk_note_card_row_activate_rich_content:
 * @self: note card row
 * @descriptors: (nullable) (element-type GnContentDescriptor): descriptors used
 *   for the most recent reservation
 *
 * Attaches fetch/render work to an existing reservation without changing its
 * allocation. Call only after the row has become visible.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_activate_rich_content(
    NostrGtkNoteCardRow *self,
    const GPtrArray *descriptors);

/**
 * nostr_gtk_note_card_row_set_rich_content:
 *
 * Compatibility helper that reserves and activates rich content in one call.
 * New GtkListView bind paths should reserve first and activate after visibility.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_rich_content(
    NostrGtkNoteCardRow *self,
    const GPtrArray *descriptors,
    double media_reserved_height,
    double link_preview_reserved_height,
    double embed_reserved_height);

/* Lightweight instrumentation for map-gated rich-child creation tests. */
NOSTR_GTK_API void  nostr_gtk_note_card_row_reset_rich_child_creation_count(void);
NOSTR_GTK_API guint nostr_gtk_note_card_row_get_rich_child_creation_count(void);

/**
 * nostr_gtk_note_card_row_apply_deferred_content:
 * @self: note card row
 * @render: (transfer none): pre-rendered content result
 *
 * Tier 2 deferred content. Creates media widgets, OG previews,
 * and note embeds from the cached render result.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_apply_deferred_content(NostrGtkNoteCardRow *self,
                                                  const GnContentRenderResult *render);
NOSTR_GTK_API void nostr_gtk_note_card_row_apply_deferred_tag_metadata(NostrGtkNoteCardRow *self,
                                                       const char *content,
                                                       const char *tags_json);

/**
 * nostr_gtk_note_card_row_set_content_rendered:
 * @self: note card row
 * @content: raw text content (for clipboard; may be NULL if not needed)
 * @render: (transfer none): pre-rendered content result (Pango markup + media URLs)
 *
 * Sets note content from a pre-rendered GnContentRenderResult,
 * skipping the expensive gnostr_render_content() call.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_content_rendered(NostrGtkNoteCardRow *self,
                                                const char *content,
                                                const GnContentRenderResult *render);

/**
 * nostr_gtk_note_card_row_set_content_with_imeta:
 * @self: note card row
 * @content: text content of the note
 * @tags_json: (nullable): JSON array string of event tags for NIP-92 imeta parsing
 *
 * Sets the note content and parses imeta tags for enhanced media display.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_content_with_imeta(NostrGtkNoteCardRow *self, const char *content, const char *tags_json);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_depth(NostrGtkNoteCardRow *self, guint depth);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_ids(NostrGtkNoteCardRow *self, const char *id_hex, const char *root_id, const char *pubkey_hex);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_embed(NostrGtkNoteCardRow *self, const char *title, const char *snippet);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_embed_rich(NostrGtkNoteCardRow *self, const char *title, const char *meta, const char *snippet);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_nip05(NostrGtkNoteCardRow *self, const char *nip05, const char *pubkey_hex);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_thread_info(NostrGtkNoteCardRow *self,
                                          const char *root_id,
                                          const char *parent_id,
                                          const char *parent_author_name,
                                          gboolean is_reply);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_pinned(NostrGtkNoteCardRow *self, gboolean is_pinned);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_bookmarked(NostrGtkNoteCardRow *self, gboolean is_bookmarked);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_liked(NostrGtkNoteCardRow *self, gboolean is_liked);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_like_count(NostrGtkNoteCardRow *self, guint count);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_event_kind(NostrGtkNoteCardRow *self, gint kind);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_reaction_breakdown(NostrGtkNoteCardRow *self, GHashTable *breakdown);
NOSTR_GTK_API void nostr_gtk_note_card_row_add_reaction(NostrGtkNoteCardRow *self, const char *emoji, const char *reactor_pubkey);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_author_lud16(NostrGtkNoteCardRow *self, const char *lud16);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_zap_stats(NostrGtkNoteCardRow *self, guint zap_count, gint64 total_msat);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_reply_count(NostrGtkNoteCardRow *self, guint count);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_is_own_note(NostrGtkNoteCardRow *self, gboolean is_own);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_logged_in(NostrGtkNoteCardRow *self, gboolean logged_in);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_repost_info(NostrGtkNoteCardRow *self,
                                          const char *reposter_pubkey_hex,
                                          const char *reposter_display_name,
                                          gint64 repost_created_at);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_is_repost(NostrGtkNoteCardRow *self, gboolean is_repost);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_repost_count(NostrGtkNoteCardRow *self, guint count);

/* NIP-18 / NIP-21 (nostrc-8xfib.6): shows @descriptor's target in the row's
 * GnNostrReferenceCard, resolved locally through @resolver (may be NULL).
 * Binding never fetches; the application may call
 * gn_nostr_reference_card_fetch() on nostr_gtk_note_card_row_get_reference_card()
 * under its own policy. NULL @descriptor hides the card. */
NOSTR_GTK_API void nostr_gtk_note_card_row_set_reference(NostrGtkNoteCardRow *self,
                                           const GnNostrRepostDescriptor *descriptor,
                                           GnNostrReferenceResolver *resolver);
/* The row's reference card, or NULL before set_reference(). */
NOSTR_GTK_API GnNostrReferenceCard *nostr_gtk_note_card_row_get_reference_card(NostrGtkNoteCardRow *self);

/* Deprecated: a precomputed quote preview; use set_reference(). */
G_DEPRECATED_FOR(nostr_gtk_note_card_row_set_reference)
NOSTR_GTK_API void nostr_gtk_note_card_row_set_quote_info(NostrGtkNoteCardRow *self,
                                         const char *quoted_event_id_hex,
                                         const char *quoted_content,
                                         const char *quoted_author_name);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_zap_receipt_info(NostrGtkNoteCardRow *self,
                                                const char *sender_pubkey,
                                                const char *sender_display_name,
                                                const char *recipient_pubkey,
                                                const char *recipient_display_name,
                                                const char *target_event_id,
                                                gint64 amount_msat);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_is_zap_receipt(NostrGtkNoteCardRow *self, gboolean is_zap);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_content_warning(NostrGtkNoteCardRow *self,
                                              const char *content_warning_reason);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_content_blurred(NostrGtkNoteCardRow *self);
NOSTR_GTK_API void nostr_gtk_note_card_row_reveal_sensitive_content(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_hashtags(NostrGtkNoteCardRow *self, const char *const *hashtags);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_relay_info(NostrGtkNoteCardRow *self,
                                          const char *const *relay_urls);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_labels(NostrGtkNoteCardRow *self, GPtrArray *labels);
NOSTR_GTK_API void nostr_gtk_note_card_row_add_label(NostrGtkNoteCardRow *self, const char *namespace, const char *label);
NOSTR_GTK_API void nostr_gtk_note_card_row_clear_labels(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_article_mode(NostrGtkNoteCardRow *self,
                                           const char *title,
                                           const char *summary,
                                           const char *image_url,
                                           gint64 published_at,
                                           const char *d_tag,
                                           const char *const *hashtags);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_article(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_article_d_tag(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_video_mode(NostrGtkNoteCardRow *self,
                                         const char *video_url,
                                         const char *thumb_url,
                                         const char *title,
                                         const char *summary,
                                         gint64 duration,
                                         gboolean is_vertical,
                                         const char *d_tag,
                                         const char *const *hashtags);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_video(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_video_d_tag(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_video_url(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_git_repo_mode(NostrGtkNoteCardRow *self,
                                             const char *name,
                                             const char *description,
                                             const char *const *clone_urls,
                                             const char *const *web_urls,
                                             const char *const *topics,
                                             gsize maintainer_count,
                                             const char *license);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_git_patch_mode(NostrGtkNoteCardRow *self,
                                              const char *title,
                                              const char *repo_name,
                                              const char *commit_id);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_git_issue_mode(NostrGtkNoteCardRow *self,
                                              const char *title,
                                              const char *repo_name,
                                              gboolean is_open,
                                              const char *const *labels);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_git_event(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_enable_text_selection(NostrGtkNoteCardRow *self, gboolean enable);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_content_text(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_event_id(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_pubkey(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_proxy_info(NostrGtkNoteCardRow *self,
                                         const char *proxy_id,
                                         const char *protocol);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_proxy_from_tags(NostrGtkNoteCardRow *self,
                                              const char *tags_json);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_proxied(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_proxy_protocol(NostrGtkNoteCardRow *self);
NOSTR_GTK_API const char *nostr_gtk_note_card_row_get_proxy_id(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_ots_proof(NostrGtkNoteCardRow *self, const char *tags_json);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_ots_status(NostrGtkNoteCardRow *self,
                                         gint status,
                                         gint64 verified_timestamp,
                                         guint block_height);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_has_ots_proof(NostrGtkNoteCardRow *self);
NOSTR_GTK_API gint64 nostr_gtk_note_card_row_get_ots_timestamp(NostrGtkNoteCardRow *self);

NOSTR_GTK_API void nostr_gtk_note_card_row_set_external_ids(NostrGtkNoteCardRow *self, const char *tags_json);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_has_external_ids(NostrGtkNoteCardRow *self);
NOSTR_GTK_API void nostr_gtk_note_card_row_clear_external_ids(NostrGtkNoteCardRow *self);

/**
 * nostr_gtk_note_card_row_get_avatar_widgets:
 * @self: note card row
 * @out_image: (out) (optional): the avatar GtkPicture widget
 * @out_initials: (out) (optional): the avatar initials GtkLabel widget
 *
 * Returns the internal avatar image and initials widgets so the app-level
 * factory can trigger async downloads on cache miss.
 */
NOSTR_GTK_API void nostr_gtk_note_card_row_get_avatar_widgets(NostrGtkNoteCardRow *self,
                                                GtkWidget **out_image,
                                                GtkWidget **out_initials);

NOSTR_GTK_API GCancellable *nostr_gtk_note_card_row_get_cancellable(NostrGtkNoteCardRow *self);
NOSTR_GTK_API void nostr_gtk_note_card_row_prepare_for_bind(NostrGtkNoteCardRow *self);
NOSTR_GTK_API void nostr_gtk_note_card_row_prepare_for_unbind(NostrGtkNoteCardRow *self);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_disposed(NostrGtkNoteCardRow *self);
NOSTR_GTK_API gboolean nostr_gtk_note_card_row_is_bound(NostrGtkNoteCardRow *self);
NOSTR_GTK_API guint64 nostr_gtk_note_card_row_get_binding_id(NostrGtkNoteCardRow *self);

/* Binding-cycle handle for asynchronous host callbacks. Cancellation and
 * creation remain widget-owned; hosts retain a handle and acquire the row
 * only while that binding is current. */
#ifndef NOTE_CARD_BINDING_CTX_TYPEDEF
#define NOTE_CARD_BINDING_CTX_TYPEDEF
typedef struct _NoteCardBindingContext NoteCardBindingContext;
#endif
NOSTR_GTK_API NoteCardBindingContext *nostr_gtk_note_card_row_get_binding_ctx(NostrGtkNoteCardRow *self);

/* ref() returns a full reference; unref() consumes it. get_row() returns a
 * full GObject reference, or NULL after cancellation/finalization. */
NOSTR_GTK_API NoteCardBindingContext *note_card_binding_context_ref(NoteCardBindingContext *ctx);
NOSTR_GTK_API void note_card_binding_context_unref(NoteCardBindingContext *ctx);
NOSTR_GTK_API GObject *note_card_binding_context_get_row(NoteCardBindingContext *ctx);

/**
 * nostr_gtk_media_image_cache_size:
 *
 * Note-card media textures are owned and byte-accounted by the application
 * media service; this telemetry accessor is retained for compatibility.
 *
 * Returns: always 0
 */
NOSTR_GTK_API guint nostr_gtk_media_image_cache_size(void);

/**
 * NostrGtkLinkPreviewSetupFunc:
 * @card: a new link-preview card, not yet given its URL
 * @url: the link it will show
 *
 * Lets the application give a card its provider, layout, auto-load policy
 * and activation handler before the row sets the URL (nostrc-8xfib.3).
 * Without one, cards only offer an explicit, signal-only "Load preview".
 * Since: 1.2
 */
typedef void (*NostrGtkLinkPreviewSetupFunc)(GnOgPreviewCard *card, const char *url,
                                             gpointer user_data);
NOSTR_GTK_API void nostr_gtk_note_card_row_set_link_preview_setup(NostrGtkLinkPreviewSetupFunc func,
                                                    gpointer user_data);

G_END_DECLS
#endif /* NOSTR_GTK_NOSTR_NOTE_CARD_ROW_H */
