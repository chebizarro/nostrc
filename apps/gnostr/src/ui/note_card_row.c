#include "note_card_row.h"
#include "og-preview-widget.h"
#include "gnostr-image-viewer.h"
#include "gnostr-video-player.h"
#include "gnostr-note-embed.h"
#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <json.h>
#include "gnostr-avatar-cache.h"
#include "../util/utils.h"
#include "../util/nip05.h"
#include "../util/imeta.h"
#include "../util/zap.h"
#include "../util/custom_emoji.h"
#include "../util/nip32_labels.h"
#include "../util/nip71.h"
#include "../util/nip84_highlights.h"
#include "../util/nip48_proxy.h"
#include "../util/nip03_opentimestamps.h"
#include "../util/nip73_external_ids.h"
#include "../util/markdown_pango.h"
#include "../util/nip21_uri.h"
#include "../storage_ndb.h"
#include <nostr/nip19/nip19.h>
#include "gnostr-profile-provider.h"
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif

#define UI_RESOURCE "/org/gnostr/ui/ui/widgets/note-card-row.ui"

/* No longer using mutex - proper fix is at backend level */

/* Context for async media image loading.
 * CRITICAL: Use GWeakRef to prevent use-after-free crash when
 * GtkListView recycles rows during scrolling. */
#ifdef HAVE_SOUP3
typedef struct {
  GWeakRef picture_ref;  /* weak ref to GtkPicture */
} MediaLoadCtx;

static void media_load_ctx_free(MediaLoadCtx *ctx) {
  if (!ctx) return;
  g_weak_ref_clear(&ctx->picture_ref);
  g_free(ctx);
}
#endif

/* Media image cache to reduce memory usage - LRU with bounded size */
#define MEDIA_IMAGE_CACHE_MAX 50  /* Max cached media images */
static GHashTable *s_media_image_cache = NULL;  /* URL -> GdkTexture */
static GQueue *s_media_image_lru = NULL;        /* URL strings in LRU order */
static GHashTable *s_media_image_lru_nodes = NULL; /* URL -> GList* node */

static void ensure_media_image_cache(void) {
  if (!s_media_image_cache) {
    s_media_image_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    s_media_image_lru = g_queue_new();
    s_media_image_lru_nodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
}

static GdkTexture *media_image_cache_get(const char *url) {
  if (!url || !s_media_image_cache) return NULL;
  GdkTexture *tex = g_hash_table_lookup(s_media_image_cache, url);
  if (tex) {
    /* Touch LRU */
    GList *node = g_hash_table_lookup(s_media_image_lru_nodes, url);
    if (node) {
      g_queue_unlink(s_media_image_lru, node);
      g_queue_push_tail_link(s_media_image_lru, node);
    }
    return g_object_ref(tex);
  }
  return NULL;
}

static void media_image_cache_put(const char *url, GdkTexture *tex) {
  if (!url || !tex) return;
  ensure_media_image_cache();
  
  /* Already cached? */
  if (g_hash_table_contains(s_media_image_cache, url)) return;
  
  /* Evict oldest if over limit */
  while (g_hash_table_size(s_media_image_cache) >= MEDIA_IMAGE_CACHE_MAX &&
         !g_queue_is_empty(s_media_image_lru)) {
    char *oldest = g_queue_pop_head(s_media_image_lru);
    if (oldest) {
      g_hash_table_remove(s_media_image_lru_nodes, oldest);
      g_hash_table_remove(s_media_image_cache, oldest);
      g_free(oldest);
    }
  }
  
  /* Insert new entry */
  g_hash_table_insert(s_media_image_cache, g_strdup(url), g_object_ref(tex));
  char *lru_key = g_strdup(url);
  GList *node = g_list_alloc();
  node->data = lru_key;
  g_queue_push_tail_link(s_media_image_lru, node);
  g_hash_table_insert(s_media_image_lru_nodes, g_strdup(url), node);
}

/* Public: Get current cache size for memory stats */
guint gnostr_media_image_cache_size(void) {
  return s_media_image_cache ? g_hash_table_size(s_media_image_cache) : 0;
}

struct _GnostrNoteCardRow {
  GtkWidget parent_instance;
  // template children
  GtkWidget *root;
  GtkWidget *btn_avatar;
  GtkWidget *btn_display_name;
  GtkWidget *btn_menu;
  GtkWidget *btn_reply;
  GtkWidget *btn_repost;
  GtkWidget *btn_like;
  GtkWidget *lbl_like_count;
  GtkWidget *btn_zap;
  GtkWidget *lbl_zap_count;
  GtkWidget *btn_bookmark;
  GtkWidget *btn_thread;
  GtkWidget *avatar_box;
  GtkWidget *avatar_initials;
  GtkWidget *avatar_image;
  GtkWidget *lbl_display;
  GtkWidget *lbl_handle;
  GtkWidget *lbl_nip05_separator;
  GtkWidget *lbl_nip05;
  GtkWidget *lbl_timestamp_separator;
  GtkWidget *lbl_timestamp;
  GtkWidget *content_label;
  GtkWidget *emoji_box;  /* NIP-30: Custom emoji display box */
  GtkWidget *media_box;
  GtkWidget *embed_box;
  GtkWidget *og_preview_container;
  GtkWidget *actions_box;
  GtkWidget *repost_popover;  /* popover menu for repost/quote options */
  GtkWidget *menu_popover;    /* popover menu for more options (JSON, mute, etc.) */
  /* Reply indicator widgets */
  GtkWidget *reply_indicator_box;
  GtkWidget *reply_indicator_label;
  /* Reply count widgets (thread root indicator) */
  GtkWidget *reply_count_box;
  GtkWidget *reply_count_label;
  guint reply_count;
  // state
  char *avatar_url;
#ifdef HAVE_SOUP3
  GCancellable *avatar_cancellable;
  SoupSession *media_session;
  GHashTable *media_cancellables; /* URL -> GCancellable */
#endif
  guint depth;
  char *id_hex;
  char *root_id;
  char *parent_id;
  char *pubkey_hex;
  char *parent_pubkey;
  gint64 created_at;
  guint timestamp_timer_id;
  OgPreviewWidget *og_preview;
  /* NIP-21 embedded note widget */
  GnostrNoteEmbed *note_embed;
  /* NIP-05 verification state */
  char *nip05;
  GtkWidget *nip05_badge;
  GCancellable *nip05_cancellable;
  /* Reply state */
  gboolean is_reply;
  gboolean is_thread_root;
  /* Bookmark state */
  gboolean is_bookmarked;
  /* Like state (NIP-25 reactions) */
  gboolean is_liked;
  guint like_count;
  gint event_kind;  /* Kind of this event (1=text note, etc.) for NIP-25 k-tag */
  /* NIP-25 reaction breakdown */
  GHashTable *reaction_breakdown;  /* emoji (string) -> count (guint) */
  GPtrArray *reactors;  /* Array of reactor pubkeys (for "who reacted" display) */
  GtkWidget *reactions_popover;  /* Popover showing reaction details */
  GtkWidget *emoji_picker_popover;  /* Popover for picking custom emoji reaction */
  /* Zap state */
  gint64 zap_total_msat;
  guint zap_count;
  gchar *author_lud16;  /* Author's lightning address from profile */
  /* Content state (plain text for clipboard) */
  gchar *content_text;
  /* NIP-09: Track if this is the current user's own note (for delete option) */
  gboolean is_own_note;
  GtkWidget *delete_btn;  /* Reference to delete button for visibility toggle */
  /* Login state: track whether user is logged in (affects button sensitivity) */
  gboolean is_logged_in;
  /* NIP-18 Repost state */
  gboolean is_repost;
  gchar *reposter_pubkey;
  gchar *reposter_display_name;
  gint64 repost_created_at;
  guint repost_count;
  GtkWidget *repost_indicator_box;  /* "Reposted by X" header */
  GtkWidget *repost_indicator_label;
  GtkWidget *lbl_repost_count;  /* Repost count next to button */
  /* NIP-18 Quote state */
  gchar *quoted_event_id;
  GtkWidget *quote_embed_box;  /* Container for quoted note preview */
  /* NIP-14 Subject tag */
  GtkWidget *subject_label;  /* Subject heading for email-like subject lines */
  /* NIP-36 Sensitive content state */
  gboolean is_sensitive;              /* TRUE if note has content-warning tag */
  gboolean sensitive_content_revealed; /* TRUE if user clicked to reveal */
  gchar *content_warning_reason;      /* Optional reason from content-warning tag */
  GtkWidget *sensitive_content_overlay; /* Overlay for blur/reveal UI */
  GtkWidget *sensitive_warning_box;   /* Box with warning icon/text/button */
  GtkWidget *sensitive_warning_label; /* Label showing "Sensitive Content: reason" */
  GtkWidget *btn_show_sensitive;      /* Button to reveal sensitive content */
  /* Hashtags from "t" tags */
  GtkWidget *hashtags_box;            /* FlowBox container for hashtag chips */
  /* NIP-32 Labels state */
  GtkWidget *labels_box;              /* FlowBox container for label chips */
  /* NIP-23 Long-form Content state */
  gboolean is_article;                /* TRUE if this card displays a kind 30023 article */
  gchar *article_d_tag;               /* The article's unique "d" tag identifier */
  gchar *article_title;               /* Article title from "title" tag */
  gchar *article_image_url;           /* Header image URL from "image" tag */
  gint64 article_published_at;        /* Publication timestamp from "published_at" tag */
  GtkWidget *article_title_label;     /* Title label widget (shown in article mode) */
  GtkWidget *article_image_box;       /* Header image container */
  GtkWidget *article_image;           /* Header image GtkPicture */
  GtkWidget *article_hashtags_box;    /* FlowBox for article hashtags */
  GtkWidget *article_reading_time;    /* Reading time estimate label */
#ifdef HAVE_SOUP3
  GCancellable *article_image_cancellable; /* Cancellable for header image fetch */
#endif
  /* NIP-71 Video Events state */
  gboolean is_video;                  /* TRUE if this card displays a video event */
  gchar *video_d_tag;                 /* The video's unique "d" tag identifier */
  gchar *video_url;                   /* Video URL from "url" tag */
  gchar *video_thumb_url;             /* Thumbnail URL from "thumb" tag */
  gchar *video_title;                 /* Video title from "title" tag */
  gint64 video_duration;              /* Duration in seconds */
  gboolean video_is_vertical;         /* TRUE for vertical video (kind 34236) */
  GtkWidget *video_player;            /* GnostrVideoPlayer widget */
  GtkWidget *video_overlay;           /* Overlay container for thumbnail + play button */
  GtkWidget *video_thumb_picture;     /* Thumbnail image */
  GtkWidget *video_play_overlay_btn;  /* Play button overlay on thumbnail */
  GtkWidget *video_duration_badge;    /* Duration badge overlay */
  GtkWidget *video_title_label;       /* Title label widget */
  GtkWidget *video_hashtags_box;      /* FlowBox for video hashtags */
  gboolean video_player_shown;        /* TRUE if player is visible (after user clicked play) */
#ifdef HAVE_SOUP3
  GCancellable *video_thumb_cancellable; /* Cancellable for thumbnail fetch */
#endif
  /* NIP-48 Proxy Tags state */
  GtkWidget *proxy_indicator_box;     /* "via Protocol" indicator widget */
  gchar *proxy_id;                    /* Original source identifier/URL */
  gchar *proxy_protocol;              /* Protocol name (activitypub, atproto, etc.) */
  /* NIP-03 OpenTimestamps state */
  gboolean has_ots_proof;             /* TRUE if event has "ots" tag */
  gint ots_status;                    /* GnostrOtsStatus verification status */
  gint64 ots_verified_timestamp;      /* Bitcoin attestation timestamp */
  guint ots_block_height;             /* Bitcoin block height */
  GtkWidget *ots_badge;               /* OTS verification badge widget */
  /* NIP-73 External Content IDs state */
  GtkWidget *external_ids_box;        /* FlowBox container for external ID badges */
  GPtrArray *external_ids;            /* Array of GnostrExternalContentId* */
  /* Disposal state - prevents async callbacks from accessing widget after dispose starts */
  gboolean disposed;
  
  /* Shared cancellable for ALL async operations (avatar, og-preview, note-embed, etc.)
   * When this widget is disposed, cancelling this single cancellable stops all child operations */
  GCancellable *async_cancellable;
};

G_DEFINE_TYPE(GnostrNoteCardRow, gnostr_note_card_row, GTK_TYPE_WIDGET)

enum {
  SIGNAL_OPEN_NOSTR_TARGET,
  SIGNAL_OPEN_URL,
  SIGNAL_REQUEST_EMBED,
  SIGNAL_OPEN_PROFILE,
  SIGNAL_REPLY_REQUESTED,
  SIGNAL_REPOST_REQUESTED,
  SIGNAL_QUOTE_REQUESTED,
  SIGNAL_LIKE_REQUESTED,
  SIGNAL_ZAP_REQUESTED,
  SIGNAL_VIEW_THREAD_REQUESTED,
  SIGNAL_MUTE_USER_REQUESTED,
  SIGNAL_MUTE_THREAD_REQUESTED,
  SIGNAL_SHOW_TOAST,
  SIGNAL_BOOKMARK_TOGGLED,
  SIGNAL_REPORT_NOTE_REQUESTED,
  SIGNAL_SHARE_NOTE_REQUESTED,
  SIGNAL_SEARCH_HASHTAG,
  SIGNAL_NAVIGATE_TO_NOTE,
  SIGNAL_DELETE_NOTE_REQUESTED,
  SIGNAL_COMMENT_REQUESTED,  /* NIP-22: comment on note */
  SIGNAL_LABEL_NOTE_REQUESTED,  /* NIP-32: add label to note */
  SIGNAL_HIGHLIGHT_REQUESTED,  /* NIP-84: highlight text selection */
  SIGNAL_DM_REQUESTED,  /* NIP-04/17: open DM conversation with user */
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void gnostr_note_card_row_dispose(GObject *obj) {
  GnostrNoteCardRow *self = (GnostrNoteCardRow*)obj;

  /* If already disposed (e.g., by prepare_for_unbind), skip cleanup that was already done.
   * We still need to call parent dispose and dispose_template though. */
  if (self->disposed) {
    goto do_template_dispose;
  }

  /* Mark as disposed FIRST to prevent async callbacks from accessing widget */
  self->disposed = TRUE;

  /* Cancel the shared async_cancellable - this stops ALL child async operations
   * (avatar downloads, og-preview fetches, note-embed queries, etc.) */
  if (self->async_cancellable) {
    g_cancellable_cancel(self->async_cancellable);
    g_clear_object(&self->async_cancellable);
  }

  /* Remove timestamp timer */
  if (self->timestamp_timer_id > 0) {
    g_source_remove(self->timestamp_timer_id);
    self->timestamp_timer_id = 0;
  }

  /* Cancel NIP-05 verification (legacy - will migrate to shared cancellable) */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) { g_cancellable_cancel(self->avatar_cancellable); g_clear_object(&self->avatar_cancellable); }
  /* Cancel all media fetches */
  if (self->media_cancellables) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->media_cancellables);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      GCancellable *cancellable = G_CANCELLABLE(value);
      if (cancellable) g_cancellable_cancel(cancellable);
    }
    g_clear_pointer(&self->media_cancellables, g_hash_table_unref);
  }
  g_clear_object(&self->media_session);
#endif
  /* Do NOT remove og_preview from container during disposal - removing triggers disposal
   * of og_preview while the parent is also being disposed, causing Pango layout corruption.
   * Just clear the pointer and let GTK handle cleanup during template disposal. */
  self->og_preview = NULL;
  
  /* NIP-71: Stop ALL video players BEFORE template disposal to prevent GStreamer from
   * accessing Pango layouts while the widget is being disposed. This fixes crashes
   * when many items are removed at once (e.g., clicking "New Notes" toast).
   * Video players can be in:
   * 1. self->video_player (NIP-71 dedicated video events)
   * 2. self->media_box (inline videos from note content) */
  if (self->video_player && GNOSTR_IS_VIDEO_PLAYER(self->video_player)) {
    gnostr_video_player_stop(GNOSTR_VIDEO_PLAYER(self->video_player));
  }
  self->video_player = NULL;
  
  /* Stop any video players in media_box (inline videos from note content) */
  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    GtkWidget *child = gtk_widget_get_first_child(self->media_box);
    while (child) {
      if (GNOSTR_IS_VIDEO_PLAYER(child)) {
        gnostr_video_player_stop(GNOSTR_VIDEO_PLAYER(child));
      }
      child = gtk_widget_get_next_sibling(child);
    }
  }
  
#ifdef HAVE_SOUP3
  /* Cancel video thumbnail fetch */
  if (self->video_thumb_cancellable) {
    g_cancellable_cancel(self->video_thumb_cancellable);
    g_clear_object(&self->video_thumb_cancellable);
  }
#endif
  
  /* Disconnect signal handlers from note_embed to prevent invalid closure notify.
   * Do NOT call gtk_frame_set_child(NULL) - let GTK handle cleanup automatically.
   * Check GNOSTR_IS_NOTE_EMBED to ensure the widget hasn't been freed already. */
  if (self->note_embed && GNOSTR_IS_NOTE_EMBED(self->note_embed)) {
    g_signal_handlers_disconnect_by_data(self->note_embed, self);
  }
  self->note_embed = NULL;

do_template_dispose:
  
  /* Unparent popovers BEFORE template disposal to prevent GTK warnings about
   * "Finalizing GtkButton but it still has children left". The popovers are
   * parented to buttons in the template, so they must be unparented first.
   * Use g_clear_pointer with gtk_widget_unparent to safely handle NULL. */
  if (self->repost_popover && GTK_IS_WIDGET(self->repost_popover)) {
    gtk_widget_unparent(self->repost_popover);
  }
  self->repost_popover = NULL;
  
  if (self->menu_popover && GTK_IS_WIDGET(self->menu_popover)) {
    gtk_widget_unparent(self->menu_popover);
  }
  self->menu_popover = NULL;
  
  if (self->emoji_picker_popover && GTK_IS_WIDGET(self->emoji_picker_popover)) {
    gtk_widget_unparent(self->emoji_picker_popover);
  }
  self->emoji_picker_popover = NULL;
  
  if (self->reactions_popover && GTK_IS_WIDGET(self->reactions_popover)) {
    gtk_widget_unparent(self->reactions_popover);
  }
  self->reactions_popover = NULL;
  /* NIP-25: Clean up reaction breakdown */
  g_clear_pointer(&self->reaction_breakdown, g_hash_table_unref);
  if (self->reactors) {
    g_ptr_array_unref(self->reactors);
    self->reactors = NULL;
  }
  
  /* Do NOT manipulate widgets during disposal - any calls to gtk_label_set_text(),
   * gtk_label_set_attributes(), gtk_picture_set_paintable(), etc. can trigger
   * recalculation/rendering while the widget is being disposed, causing crashes.
   * GTK will handle all widget cleanup automatically during finalization. */
  
  gtk_widget_dispose_template(GTK_WIDGET(self), GNOSTR_TYPE_NOTE_CARD_ROW);
  self->root = NULL; self->avatar_box = NULL; self->avatar_initials = NULL; self->avatar_image = NULL;
  self->lbl_display = NULL; self->lbl_handle = NULL; self->lbl_nip05_separator = NULL; self->lbl_nip05 = NULL;
  self->lbl_timestamp_separator = NULL; self->lbl_timestamp = NULL; self->content_label = NULL;
  self->emoji_box = NULL; self->media_box = NULL; self->embed_box = NULL; self->og_preview_container = NULL; self->actions_box = NULL;
  self->btn_repost = NULL; self->btn_like = NULL; self->btn_bookmark = NULL; self->btn_thread = NULL;
  self->reply_indicator_box = NULL; self->reply_indicator_label = NULL;
  self->reply_count_box = NULL; self->reply_count_label = NULL;
  /* NIP-18 repost widgets */
  self->repost_indicator_box = NULL; self->repost_indicator_label = NULL;
  self->lbl_repost_count = NULL; self->quote_embed_box = NULL;
  /* NIP-14 subject widget */
  self->subject_label = NULL;
  /* NIP-36 sensitive content widgets */
  self->sensitive_content_overlay = NULL;
  self->sensitive_warning_box = NULL;
  self->sensitive_warning_label = NULL;
  self->btn_show_sensitive = NULL;
  /* NIP-23 article widgets */
  self->article_title_label = NULL;
  self->article_image_box = NULL;
  self->article_image = NULL;
  self->article_hashtags_box = NULL;
  self->article_reading_time = NULL;
#ifdef HAVE_SOUP3
  if (self->article_image_cancellable) {
    g_cancellable_cancel(self->article_image_cancellable);
    g_clear_object(&self->article_image_cancellable);
  }
#endif
  /* NIP-48 proxy widget */
  self->proxy_indicator_box = NULL;
  /* NIP-03 OTS widget */
  self->ots_badge = NULL;
  /* NIP-73 external content IDs */
  self->external_ids_box = NULL;
  if (self->external_ids) {
    g_ptr_array_unref(self->external_ids);
    self->external_ids = NULL;
  }
  G_OBJECT_CLASS(gnostr_note_card_row_parent_class)->dispose(obj);
}

static void gnostr_note_card_row_finalize(GObject *obj) {
  GnostrNoteCardRow *self = (GnostrNoteCardRow*)obj;
  g_clear_pointer(&self->avatar_url, g_free);
  g_clear_pointer(&self->id_hex, g_free);
  g_clear_pointer(&self->root_id, g_free);
  g_clear_pointer(&self->parent_id, g_free);
  g_clear_pointer(&self->pubkey_hex, g_free);
  g_clear_pointer(&self->parent_pubkey, g_free);
  g_clear_pointer(&self->nip05, g_free);
  g_clear_pointer(&self->author_lud16, g_free);
  g_clear_pointer(&self->content_text, g_free);
  /* NIP-18 repost state cleanup */
  g_clear_pointer(&self->reposter_pubkey, g_free);
  g_clear_pointer(&self->reposter_display_name, g_free);
  g_clear_pointer(&self->quoted_event_id, g_free);
  /* NIP-36 sensitive content state cleanup */
  g_clear_pointer(&self->content_warning_reason, g_free);
  /* NIP-23 article state cleanup */
  g_clear_pointer(&self->article_d_tag, g_free);
  g_clear_pointer(&self->article_title, g_free);
  g_clear_pointer(&self->article_image_url, g_free);
  /* NIP-48 proxy state cleanup */
  g_clear_pointer(&self->proxy_id, g_free);
  g_clear_pointer(&self->proxy_protocol, g_free);
  /* NIP-71 video state cleanup */
  g_clear_pointer(&self->video_d_tag, g_free);
  g_clear_pointer(&self->video_url, g_free);
  g_clear_pointer(&self->video_thumb_url, g_free);
  g_clear_pointer(&self->video_title, g_free);
  G_OBJECT_CLASS(gnostr_note_card_row_parent_class)->finalize(obj);
}

static void on_avatar_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

static void on_display_name_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, self->pubkey_hex);
  }
}

/* Callback for embedded profile click (NIP-21) */
static void on_embed_profile_clicked(GnostrNoteEmbed *embed, const char *pubkey_hex, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)embed;
  if (self && pubkey_hex && *pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_OPEN_PROFILE], 0, pubkey_hex);
  }
}

static gboolean on_content_activate_link(GtkLabel *label, const char *uri, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)label;
  if (!self || !uri) return FALSE;
  /* Hashtag links - emit search signal */
  if (g_str_has_prefix(uri, "hashtag:")) {
    const char *tag = uri + 8; /* Skip "hashtag:" prefix */
    if (tag && *tag) {
      g_signal_emit(self, signals[SIGNAL_SEARCH_HASHTAG], 0, tag);
    }
    return TRUE;
  }
  /* nostr: URIs and bech32 entities */
  if (g_str_has_prefix(uri, "nostr:") || g_str_has_prefix(uri, "note1") || g_str_has_prefix(uri, "npub1") ||
      g_str_has_prefix(uri, "nevent1") || g_str_has_prefix(uri, "nprofile1") || g_str_has_prefix(uri, "naddr1")) {
    /* Check if this is an npub or nprofile - emit dm-requested signal */
    gchar *nostr_uri = g_str_has_prefix(uri, "nostr:") ? g_strdup(uri) : g_strdup_printf("nostr:%s", uri);
    GnostrUri *parsed = gnostr_uri_parse(nostr_uri);
    g_free(nostr_uri);
    if (parsed) {
      if ((parsed->type == GNOSTR_URI_TYPE_NPUB || parsed->type == GNOSTR_URI_TYPE_NPROFILE) &&
          parsed->pubkey_hex && *parsed->pubkey_hex) {
        /* Emit dm-requested signal to open DM conversation */
        g_signal_emit(self, signals[SIGNAL_DM_REQUESTED], 0, parsed->pubkey_hex);
        gnostr_uri_free(parsed);
        return TRUE;
      }
      gnostr_uri_free(parsed);
    }
    /* Fall back to open-nostr-target for other entity types (note, nevent, naddr) */
    g_signal_emit(self, signals[SIGNAL_OPEN_NOSTR_TARGET], 0, uri);
    return TRUE;
  }
  if (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")) {
    /* Open URL in the default browser using GtkUriLauncher */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
    GtkUriLauncher *launcher = gtk_uri_launcher_new(uri);
    gtk_uri_launcher_launch(launcher, parent, NULL, NULL, NULL);
    g_object_unref(launcher);
    g_signal_emit(self, signals[SIGNAL_OPEN_URL], 0, uri);
    return TRUE;
  }
  return FALSE;
}

/* Helper: convert 64-char hex string to 32 bytes */
static gboolean hex_to_bytes_32(const char *hex, unsigned char out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    unsigned int byte;
    if (sscanf(hex + i*2, "%2x", &byte) != 1) return FALSE;
    out[i] = (unsigned char)byte;
  }
  return TRUE;
}

/* Ensure NostrDB is initialized (idempotent). Mirrors logic in main_app.c */
static void ensure_ndb_initialized(void) {
  /* storage_ndb_init is idempotent; if already initialized it returns 1 */
  gchar *dbdir = g_build_filename(g_get_user_cache_dir(), "gnostr", "ndb", NULL);
  (void)g_mkdir_with_parents(dbdir, 0700);
  const char *opts = "{\"mapsize\":1073741824,\"ingester_threads\":4}";
  storage_ndb_init(dbdir, opts);
  g_free(dbdir);
}

/* Helper: pretty-print JSON string with indentation */
static char *pretty_print_json(const char *json_str) {
  if (!json_str) return NULL;

  char *pretty = nostr_json_prettify(json_str);
  if (!pretty) {
    /* Return original if prettify fails */
    return g_strdup(json_str);
  }

  /* Convert to g_strdup for GLib memory management */
  char *result = g_strdup(pretty);
  free(pretty);
  return result;
}

static void show_json_viewer(GnostrNoteCardRow *self) {
  if (!self || !self->id_hex) {
    g_warning("No event ID available to fetch JSON");
    return;
  }

  /* Ensure DB is initialized (safe if already initialized) */
  ensure_ndb_initialized();

  /* Fetch event JSON from NostrDB using nontxn helper with built-in retries */
  char *event_json = NULL;
  int json_len = 0;

  int rc = storage_ndb_get_note_by_id_nontxn(self->id_hex, &event_json, &json_len);

  if (rc != 0 || !event_json) {
    g_warning("Failed to fetch event JSON from NostrDB (id=%s, rc=%d)",
              self->id_hex, rc);
    return;
  }

  /* Pretty-print the JSON for readability */
  char *pretty_json = pretty_print_json(event_json);
  free(event_json); /* Free the original */

  /* Get the toplevel window */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  /* Create dialog */
  GtkWidget *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Event JSON");
  gtk_window_set_default_size(GTK_WINDOW(dialog), 700, 500);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  if (parent) gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);

  /* Create scrolled window */
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  /* Create text view */
  GtkWidget *text_view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_WORD_CHAR);
  gtk_widget_set_margin_start(text_view, 12);
  gtk_widget_set_margin_end(text_view, 12);
  gtk_widget_set_margin_top(text_view, 12);
  gtk_widget_set_margin_bottom(text_view, 12);

  /* Set the pretty-printed JSON content */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
  gtk_text_buffer_set_text(buffer, pretty_json ? pretty_json : "", -1);

  g_free(pretty_json);

  /* Assemble the dialog */
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), text_view);
  gtk_window_set_child(GTK_WINDOW(dialog), scrolled);

  gtk_window_present(GTK_WINDOW(dialog));
}

static void on_view_json_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }
  show_json_viewer(self);
}

static void on_mute_user_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->pubkey_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit signal to mute this user */
  g_signal_emit(self, signals[SIGNAL_MUTE_USER_REQUESTED], 0, self->pubkey_hex);
}

static void on_mute_thread_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Mute the root event of this thread (or self if it's the root) */
  const char *event_to_mute = self->root_id ? self->root_id : self->id_hex;
  if (event_to_mute) {
    g_signal_emit(self, signals[SIGNAL_MUTE_THREAD_REQUESTED], 0, event_to_mute);
  }
}

static void copy_to_clipboard(GnostrNoteCardRow *self, const char *text) {
  if (!text || !*text) return;

  GtkWidget *widget = GTK_WIDGET(self);
  GdkDisplay *display = gtk_widget_get_display(widget);
  if (display) {
    GdkClipboard *clipboard = gdk_display_get_clipboard(display);
    if (clipboard) {
      gdk_clipboard_set_text(clipboard, text);
      g_signal_emit(self, signals[SIGNAL_SHOW_TOAST], 0, "Copied to clipboard");
    }
  }
}

static void on_copy_note_id_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || strlen(self->id_hex) != 64) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Encode as nevent with relay hints if available, otherwise as note1 */
  char *encoded = NULL;

  /* Try nevent first (includes more metadata) */
  NostrNEventConfig cfg = {
    .id = self->id_hex,
    .author = self->pubkey_hex,
    .kind = 1,  /* text note */
    .relays = NULL,
    .relays_count = 0
  };

  NostrPointer *ptr = NULL;
  if (nostr_pointer_from_nevent_config(&cfg, &ptr) == 0 && ptr) {
    nostr_pointer_to_bech32(ptr, &encoded);
    nostr_pointer_free(ptr);
  }

  /* Fallback to simple note1 if nevent encoding failed */
  if (!encoded) {
    uint8_t id_bytes[32];
    if (hex_to_bytes_32(self->id_hex, id_bytes)) {
      nostr_nip19_encode_note(id_bytes, &encoded);
    }
  }

  if (encoded) {
    copy_to_clipboard(self, encoded);
    free(encoded);
  }
}

static void on_copy_pubkey_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->pubkey_hex || strlen(self->pubkey_hex) != 64) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Encode as npub1 */
  uint8_t pubkey_bytes[32];
  if (hex_to_bytes_32(self->pubkey_hex, pubkey_bytes)) {
    char *npub = NULL;
    if (nostr_nip19_encode_npub(pubkey_bytes, &npub) == 0 && npub) {
      copy_to_clipboard(self, npub);
      free(npub);
    }
  }
}

static void on_copy_note_text_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->content_text) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  copy_to_clipboard(self, self->content_text);
}

static void on_report_note_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || !self->pubkey_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit signal to report this note (NIP-56) */
  g_signal_emit(self, signals[SIGNAL_REPORT_NOTE_REQUESTED], 0, self->id_hex, self->pubkey_hex);
}

static void on_share_note_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Build nostr: URI for sharing */
  char *encoded = NULL;
  NostrNEventConfig cfg = {
    .id = self->id_hex,
    .author = self->pubkey_hex,
    .kind = 1,
    .relays = NULL,
    .relays_count = 0
  };

  NostrPointer *ptr = NULL;
  if (nostr_pointer_from_nevent_config(&cfg, &ptr) == 0 && ptr) {
    nostr_pointer_to_bech32(ptr, &encoded);
    nostr_pointer_free(ptr);
  }

  /* Fallback to simple note1 */
  if (!encoded) {
    uint8_t id_bytes[32];
    if (hex_to_bytes_32(self->id_hex, id_bytes)) {
      nostr_nip19_encode_note(id_bytes, &encoded);
    }
  }

  if (encoded) {
    /* Create nostr: URI */
    char *uri = g_strdup_printf("nostr:%s", encoded);

    /* Use GtkUriLauncher or system share if available */
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

    /* Copy to clipboard as fallback and show toast */
    copy_to_clipboard(self, uri);
    g_signal_emit(self, signals[SIGNAL_SHARE_NOTE_REQUESTED], 0, uri);

    g_free(uri);
    free(encoded);
  }
}

/* NIP-09: Delete note (only shown for own notes) */
static void on_delete_note_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || !self->pubkey_hex) return;

  /* Hide popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit signal to request deletion (NIP-09) */
  g_signal_emit(self, signals[SIGNAL_DELETE_NOTE_REQUESTED], 0, self->id_hex, self->pubkey_hex);
}

/* Forward declaration for label selection callback */
static void on_label_preset_clicked(GtkButton *btn, gpointer user_data);

/* NIP-32: Show label selection dialog */
static void on_add_label_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex) return;

  /* Hide menu popover first */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Get parent window */
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  /* Create label selection dialog */
  GtkWidget *dialog = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dialog), "Add Label");
  gtk_window_set_default_size(GTK_WINDOW(dialog), 350, 400);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  if (parent) gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);

  /* Store reference to note card for later use */
  g_object_set_data(G_OBJECT(dialog), "note-card-row", self);

  /* Main content box */
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);

  /* Title label */
  GtkWidget *title = gtk_label_new("Select a label for this note:");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(content), title);

  /* Preset labels grid */
  GtkWidget *grid = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(grid), FALSE);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(grid), 8);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(grid), 8);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 4);
  gtk_widget_add_css_class(grid, "label-dialog-grid");

  /* Add predefined labels */
  const GnostrPredefinedLabel *presets = gnostr_nip32_get_predefined_labels();
  for (int i = 0; presets[i].label != NULL; i++) {
    GtkWidget *preset_btn = gtk_button_new_with_label(presets[i].display_name);
    gtk_widget_add_css_class(preset_btn, "label-preset-btn");

    /* Store namespace and label in button data */
    g_object_set_data_full(G_OBJECT(preset_btn), "label-namespace",
                           g_strdup(presets[i].namespace), g_free);
    g_object_set_data_full(G_OBJECT(preset_btn), "label-value",
                           g_strdup(presets[i].label), g_free);
    g_object_set_data(G_OBJECT(preset_btn), "label-dialog", dialog);

    g_signal_connect(preset_btn, "clicked", G_CALLBACK(on_label_preset_clicked), self);
    gtk_flow_box_append(GTK_FLOW_BOX(grid), preset_btn);
  }

  gtk_box_append(GTK_BOX(content), grid);

  /* Separator */
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(sep, 8);
  gtk_widget_set_margin_bottom(sep, 8);
  gtk_box_append(GTK_BOX(content), sep);

  /* Custom label section */
  GtkWidget *custom_label = gtk_label_new("Or add a custom label:");
  gtk_widget_set_halign(custom_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(content), custom_label);

  /* Namespace entry */
  GtkWidget *ns_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *ns_label = gtk_label_new("Namespace:");
  GtkWidget *ns_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(ns_entry), "ugc");
  gtk_widget_set_hexpand(ns_entry, TRUE);
  gtk_box_append(GTK_BOX(ns_box), ns_label);
  gtk_box_append(GTK_BOX(ns_box), ns_entry);
  gtk_box_append(GTK_BOX(content), ns_box);

  /* Label entry */
  GtkWidget *lbl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *lbl_label = gtk_label_new("Label:");
  GtkWidget *lbl_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(lbl_entry), "interesting");
  gtk_widget_set_hexpand(lbl_entry, TRUE);
  gtk_box_append(GTK_BOX(lbl_box), lbl_label);
  gtk_box_append(GTK_BOX(lbl_box), lbl_entry);
  gtk_box_append(GTK_BOX(content), lbl_box);

  /* Store entries in dialog for later access */
  g_object_set_data(G_OBJECT(dialog), "namespace-entry", ns_entry);
  g_object_set_data(G_OBJECT(dialog), "label-entry", lbl_entry);

  /* Add Custom Label button */
  GtkWidget *add_btn = gtk_button_new_with_label("Add Custom Label");
  gtk_widget_add_css_class(add_btn, "suggested-action");
  gtk_widget_set_margin_top(add_btn, 8);

  /* Connect add button */
  g_object_set_data(G_OBJECT(add_btn), "label-dialog", dialog);
  g_signal_connect(add_btn, "clicked", G_CALLBACK(on_label_preset_clicked), self);

  gtk_box_append(GTK_BOX(content), add_btn);

  gtk_window_set_child(GTK_WINDOW(dialog), content);
  gtk_window_present(GTK_WINDOW(dialog));
}

/* Callback for preset label button click */
static void on_label_preset_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  GtkWidget *dialog = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "label-dialog"));
  if (!GTK_IS_WINDOW(dialog)) return;

  const char *namespace = NULL;
  const char *label = NULL;

  /* Check if this is a preset button or the custom add button */
  const char *preset_ns = g_object_get_data(G_OBJECT(btn), "label-namespace");
  const char *preset_label = g_object_get_data(G_OBJECT(btn), "label-value");

  if (preset_ns && preset_label) {
    /* Preset button */
    namespace = preset_ns;
    label = preset_label;
  } else {
    /* Custom label - get from entries */
    GtkWidget *ns_entry = g_object_get_data(G_OBJECT(dialog), "namespace-entry");
    GtkWidget *lbl_entry = g_object_get_data(G_OBJECT(dialog), "label-entry");

    if (!GTK_IS_ENTRY(ns_entry) || !GTK_IS_ENTRY(lbl_entry)) {
      gtk_window_close(GTK_WINDOW(dialog));
      return;
    }

    const char *ns_text = gtk_editable_get_text(GTK_EDITABLE(ns_entry));
    const char *lbl_text = gtk_editable_get_text(GTK_EDITABLE(lbl_entry));

    if (!lbl_text || !*lbl_text) {
      /* No label entered, just close */
      gtk_window_close(GTK_WINDOW(dialog));
      return;
    }

    namespace = (ns_text && *ns_text) ? ns_text : NIP32_NS_UGC;
    label = lbl_text;
  }

  /* Emit signal to request label creation */
  if (self->id_hex && self->pubkey_hex && namespace && label) {
    g_signal_emit(self, signals[SIGNAL_LABEL_NOTE_REQUESTED], 0,
                  self->id_hex, namespace, label, self->pubkey_hex);

    /* Also add label to display immediately (optimistic update) */
    gnostr_note_card_row_add_label(self, namespace, label);

    g_signal_emit(self, signals[SIGNAL_SHOW_TOAST], 0, "Label added");
  }

  gtk_window_close(GTK_WINDOW(dialog));
}

/* NIP-22 Comment menu item handler */
static void on_comment_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || !self->pubkey_hex) return;

  /* Close menu popover */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Emit comment-requested signal with kind 1 (text note) */
  g_signal_emit(self, signals[SIGNAL_COMMENT_REQUESTED], 0,
                self->id_hex, 1, self->pubkey_hex);
}

/* NIP-84: Highlight selected text menu item handler */
static void on_highlight_text_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self || !self->id_hex || !self->pubkey_hex || !self->content_text) return;

  /* Close menu popover */
  if (self->menu_popover && GTK_IS_POPOVER(self->menu_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->menu_popover));
  }

  /* Get selected text from the content label */
  const char *selected_text = NULL;
  if (GTK_IS_LABEL(self->content_label)) {
    /* In GTK4, we need to get selection bounds and extract text */
    int start, end;
    if (gtk_label_get_selection_bounds(GTK_LABEL(self->content_label), &start, &end)) {
      const char *label_text = gtk_label_get_text(GTK_LABEL(self->content_label));
      if (label_text && start >= 0 && end > start) {
        gsize len = strlen(label_text);
        if ((gsize)end <= len) {
          char *extracted = g_strndup(label_text + start, end - start);
          if (extracted && *extracted) {
            /* Extract context around the selection */
            char *context = gnostr_highlight_extract_context(
              self->content_text,
              (gsize)start,
              (gsize)end,
              100  /* 100 chars of context */
            );

            /* Emit highlight signal */
            g_signal_emit(self, signals[SIGNAL_HIGHLIGHT_REQUESTED], 0,
                          extracted, context ? context : "",
                          self->id_hex, self->pubkey_hex);

            g_signal_emit(self, signals[SIGNAL_SHOW_TOAST], 0, "Text highlighted");
            g_free(context);
          }
          g_free(extracted);
          return;
        }
      }
    }
  }

  /* No selection - highlight full content with toast message */
  g_signal_emit(self, signals[SIGNAL_SHOW_TOAST], 0,
                "Select text to highlight (enable selection in settings)");
}

static void on_menu_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Create popover if not already created */
  if (!self->menu_popover) {
    self->menu_popover = gtk_popover_new();

    /* Create a vertical box for the menu items */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* View JSON button */
    GtkWidget *json_btn = gtk_button_new();
    GtkWidget *json_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *json_icon = gtk_image_new_from_icon_name("text-x-generic-symbolic");
    GtkWidget *json_label = gtk_label_new("View Raw JSON");
    gtk_box_append(GTK_BOX(json_box), json_icon);
    gtk_box_append(GTK_BOX(json_box), json_label);
    gtk_button_set_child(GTK_BUTTON(json_btn), json_box);
    gtk_button_set_has_frame(GTK_BUTTON(json_btn), FALSE);
    g_signal_connect(json_btn, "clicked", G_CALLBACK(on_view_json_clicked), self);
    gtk_box_append(GTK_BOX(box), json_btn);

    /* Copy Note ID button */
    GtkWidget *copy_note_btn = gtk_button_new();
    GtkWidget *copy_note_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_note_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_note_label = gtk_label_new("Copy Note ID");
    gtk_box_append(GTK_BOX(copy_note_box), copy_note_icon);
    gtk_box_append(GTK_BOX(copy_note_box), copy_note_label);
    gtk_button_set_child(GTK_BUTTON(copy_note_btn), copy_note_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_note_btn), FALSE);
    g_signal_connect(copy_note_btn, "clicked", G_CALLBACK(on_copy_note_id_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_note_btn);

    /* Copy Note Text button */
    GtkWidget *copy_text_btn = gtk_button_new();
    GtkWidget *copy_text_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_text_icon = gtk_image_new_from_icon_name("edit-copy-symbolic");
    GtkWidget *copy_text_label = gtk_label_new("Copy Note Text");
    gtk_box_append(GTK_BOX(copy_text_box), copy_text_icon);
    gtk_box_append(GTK_BOX(copy_text_box), copy_text_label);
    gtk_button_set_child(GTK_BUTTON(copy_text_btn), copy_text_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_text_btn), FALSE);
    g_signal_connect(copy_text_btn, "clicked", G_CALLBACK(on_copy_note_text_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_text_btn);

    /* Copy Author Pubkey button */
    GtkWidget *copy_pubkey_btn = gtk_button_new();
    GtkWidget *copy_pubkey_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *copy_pubkey_icon = gtk_image_new_from_icon_name("avatar-default-symbolic");
    GtkWidget *copy_pubkey_label = gtk_label_new("Copy Author Pubkey");
    gtk_box_append(GTK_BOX(copy_pubkey_box), copy_pubkey_icon);
    gtk_box_append(GTK_BOX(copy_pubkey_box), copy_pubkey_label);
    gtk_button_set_child(GTK_BUTTON(copy_pubkey_btn), copy_pubkey_box);
    gtk_button_set_has_frame(GTK_BUTTON(copy_pubkey_btn), FALSE);
    g_signal_connect(copy_pubkey_btn, "clicked", G_CALLBACK(on_copy_pubkey_clicked), self);
    gtk_box_append(GTK_BOX(box), copy_pubkey_btn);

    /* Separator - Share section */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep1, 4);
    gtk_widget_set_margin_bottom(sep1, 4);
    gtk_box_append(GTK_BOX(box), sep1);

    /* Share Note button */
    GtkWidget *share_btn = gtk_button_new();
    GtkWidget *share_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *share_icon = gtk_image_new_from_icon_name("emblem-shared-symbolic");
    GtkWidget *share_label = gtk_label_new("Share Note");
    gtk_box_append(GTK_BOX(share_box), share_icon);
    gtk_box_append(GTK_BOX(share_box), share_label);
    gtk_button_set_child(GTK_BUTTON(share_btn), share_box);
    gtk_button_set_has_frame(GTK_BUTTON(share_btn), FALSE);
    g_signal_connect(share_btn, "clicked", G_CALLBACK(on_share_note_clicked), self);
    gtk_box_append(GTK_BOX(box), share_btn);

    /* NIP-22: Comment button */
    GtkWidget *comment_btn = gtk_button_new();
    GtkWidget *comment_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *comment_icon = gtk_image_new_from_icon_name("document-edit-symbolic");
    GtkWidget *comment_label = gtk_label_new("Comment (NIP-22)");
    gtk_box_append(GTK_BOX(comment_box), comment_icon);
    gtk_box_append(GTK_BOX(comment_box), comment_label);
    gtk_button_set_child(GTK_BUTTON(comment_btn), comment_box);
    gtk_button_set_has_frame(GTK_BUTTON(comment_btn), FALSE);
    g_signal_connect(comment_btn, "clicked", G_CALLBACK(on_comment_menu_clicked), self);
    gtk_box_append(GTK_BOX(box), comment_btn);

    /* NIP-32: Add Label button */
    GtkWidget *label_btn = gtk_button_new();
    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label_icon = gtk_image_new_from_icon_name("tag-symbolic");
    GtkWidget *label_label_text = gtk_label_new("Add Label");
    gtk_box_append(GTK_BOX(label_box), label_icon);
    gtk_box_append(GTK_BOX(label_box), label_label_text);
    gtk_button_set_child(GTK_BUTTON(label_btn), label_box);
    gtk_button_set_has_frame(GTK_BUTTON(label_btn), FALSE);
    g_signal_connect(label_btn, "clicked", G_CALLBACK(on_add_label_clicked), self);
    gtk_box_append(GTK_BOX(box), label_btn);

    /* NIP-84: Highlight Text button */
    GtkWidget *highlight_btn = gtk_button_new();
    GtkWidget *highlight_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *highlight_icon = gtk_image_new_from_icon_name("edit-select-all-symbolic");
    GtkWidget *highlight_label = gtk_label_new("Highlight Selection");
    gtk_box_append(GTK_BOX(highlight_box), highlight_icon);
    gtk_box_append(GTK_BOX(highlight_box), highlight_label);
    gtk_button_set_child(GTK_BUTTON(highlight_btn), highlight_box);
    gtk_button_set_has_frame(GTK_BUTTON(highlight_btn), FALSE);
    g_signal_connect(highlight_btn, "clicked", G_CALLBACK(on_highlight_text_clicked), self);
    gtk_box_append(GTK_BOX(box), highlight_btn);

    /* Separator - Moderation section */
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep2, 4);
    gtk_widget_set_margin_bottom(sep2, 4);
    gtk_box_append(GTK_BOX(box), sep2);

    /* Mute Author button */
    GtkWidget *mute_btn = gtk_button_new();
    GtkWidget *mute_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mute_icon = gtk_image_new_from_icon_name("action-unavailable-symbolic");
    GtkWidget *mute_label = gtk_label_new("Mute Author");
    gtk_box_append(GTK_BOX(mute_box), mute_icon);
    gtk_box_append(GTK_BOX(mute_box), mute_label);
    gtk_button_set_child(GTK_BUTTON(mute_btn), mute_box);
    gtk_button_set_has_frame(GTK_BUTTON(mute_btn), FALSE);
    g_signal_connect(mute_btn, "clicked", G_CALLBACK(on_mute_user_clicked), self);
    gtk_box_append(GTK_BOX(box), mute_btn);

    /* Mute Thread button */
    GtkWidget *mute_thread_btn = gtk_button_new();
    GtkWidget *mute_thread_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *mute_thread_icon = gtk_image_new_from_icon_name("mail-mark-junk-symbolic");
    GtkWidget *mute_thread_label = gtk_label_new("Mute Thread");
    gtk_box_append(GTK_BOX(mute_thread_box), mute_thread_icon);
    gtk_box_append(GTK_BOX(mute_thread_box), mute_thread_label);
    gtk_button_set_child(GTK_BUTTON(mute_thread_btn), mute_thread_box);
    gtk_button_set_has_frame(GTK_BUTTON(mute_thread_btn), FALSE);
    g_signal_connect(mute_thread_btn, "clicked", G_CALLBACK(on_mute_thread_clicked), self);
    gtk_box_append(GTK_BOX(box), mute_thread_btn);

    /* Report Note button */
    GtkWidget *report_btn = gtk_button_new();
    GtkWidget *report_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *report_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    GtkWidget *report_label = gtk_label_new("Report Note");
    gtk_box_append(GTK_BOX(report_box), report_icon);
    gtk_box_append(GTK_BOX(report_box), report_label);
    gtk_button_set_child(GTK_BUTTON(report_btn), report_box);
    gtk_button_set_has_frame(GTK_BUTTON(report_btn), FALSE);
    g_signal_connect(report_btn, "clicked", G_CALLBACK(on_report_note_clicked), self);
    gtk_box_append(GTK_BOX(box), report_btn);

    /* Separator before Delete section (NIP-09) */
    GtkWidget *sep3 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep3, 4);
    gtk_widget_set_margin_bottom(sep3, 4);
    gtk_box_append(GTK_BOX(box), sep3);

    /* Delete Note button (NIP-09) - only visible for own notes */
    GtkWidget *delete_btn = gtk_button_new();
    GtkWidget *delete_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *delete_icon = gtk_image_new_from_icon_name("user-trash-symbolic");
    GtkWidget *delete_label = gtk_label_new("Delete Note");
    gtk_box_append(GTK_BOX(delete_box), delete_icon);
    gtk_box_append(GTK_BOX(delete_box), delete_label);
    gtk_button_set_child(GTK_BUTTON(delete_btn), delete_box);
    gtk_button_set_has_frame(GTK_BUTTON(delete_btn), FALSE);
    gtk_widget_add_css_class(delete_btn, "destructive-action");
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_delete_note_clicked), self);
    gtk_box_append(GTK_BOX(box), delete_btn);
    /* Store reference for visibility toggle */
    self->delete_btn = delete_btn;
    /* Initially hide delete button - will be shown if is_own_note is set */
    gtk_widget_set_visible(delete_btn, self->is_own_note);
    gtk_widget_set_visible(sep3, self->is_own_note);
    /* Store separator reference for visibility toggle */
    g_object_set_data(G_OBJECT(delete_btn), "delete-separator", sep3);

    gtk_popover_set_child(GTK_POPOVER(self->menu_popover), box);
    gtk_widget_set_parent(self->menu_popover, GTK_WIDGET(self->btn_menu));
  }

  /* Show the popover */
  gtk_popover_popup(GTK_POPOVER(self->menu_popover));
}

/* REMOVED: Right-click and long-press context menu handlers.
 * These were causing the menu to appear off-screen when right-clicking anywhere
 * on the note card. The 3-dot menu button is the only way to access the menu now. */

static void on_reply_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    g_signal_emit(self, signals[SIGNAL_REPLY_REQUESTED], 0,
                  self->id_hex, self->root_id, self->pubkey_hex);
  }
}

/* NIP-22: Comment button handler - emits comment-requested signal */
static void on_comment_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* For kind 1 text notes, emit comment signal with kind=1 */
    g_signal_emit(self, signals[SIGNAL_COMMENT_REQUESTED], 0,
                  self->id_hex, 1, self->pubkey_hex);
  }
}

static void on_repost_action_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Hide popover first */
    if (self->repost_popover && GTK_IS_POPOVER(self->repost_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->repost_popover));
    }
    g_signal_emit(self, signals[SIGNAL_REPOST_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex);
  }
}

static void on_quote_action_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Hide popover first */
    if (self->repost_popover && GTK_IS_POPOVER(self->repost_popover)) {
      gtk_popover_popdown(GTK_POPOVER(self->repost_popover));
    }
    g_signal_emit(self, signals[SIGNAL_QUOTE_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex);
  }
}

static void on_repost_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Create popover if not already created */
  if (!self->repost_popover) {
    self->repost_popover = gtk_popover_new();

    /* Create a vertical box for the menu items */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(box, 6);
    gtk_widget_set_margin_end(box, 6);
    gtk_widget_set_margin_top(box, 6);
    gtk_widget_set_margin_bottom(box, 6);

    /* Repost button */
    GtkWidget *repost_btn = gtk_button_new();
    GtkWidget *repost_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *repost_icon = gtk_image_new_from_icon_name("object-rotate-right-symbolic");
    GtkWidget *repost_label = gtk_label_new("Repost");
    gtk_box_append(GTK_BOX(repost_box), repost_icon);
    gtk_box_append(GTK_BOX(repost_box), repost_label);
    gtk_button_set_child(GTK_BUTTON(repost_btn), repost_box);
    gtk_button_set_has_frame(GTK_BUTTON(repost_btn), FALSE);
    g_signal_connect(repost_btn, "clicked", G_CALLBACK(on_repost_action_clicked), self);
    gtk_box_append(GTK_BOX(box), repost_btn);

    /* Quote button */
    GtkWidget *quote_btn = gtk_button_new();
    GtkWidget *quote_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *quote_icon = gtk_image_new_from_icon_name("format-text-quote-symbolic");
    GtkWidget *quote_label = gtk_label_new("Quote");
    gtk_box_append(GTK_BOX(quote_box), quote_icon);
    gtk_box_append(GTK_BOX(quote_box), quote_label);
    gtk_button_set_child(GTK_BUTTON(quote_btn), quote_box);
    gtk_button_set_has_frame(GTK_BUTTON(quote_btn), FALSE);
    g_signal_connect(quote_btn, "clicked", G_CALLBACK(on_quote_action_clicked), self);
    gtk_box_append(GTK_BOX(box), quote_btn);

    gtk_popover_set_child(GTK_POPOVER(self->repost_popover), box);
    gtk_widget_set_parent(self->repost_popover, GTK_WIDGET(self->btn_repost));
  }

  /* Show the popover */
  gtk_popover_popup(GTK_POPOVER(self->repost_popover));
}

/* NIP-25: Callback for emoji picker selection */
static void on_emoji_selected(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  if (!self || !self->id_hex || !self->pubkey_hex) return;

  const char *emoji = g_object_get_data(G_OBJECT(btn), "emoji");
  if (!emoji) emoji = "+";

  /* Emit reaction-requested signal with emoji content */
  g_signal_emit(self, signals[SIGNAL_LIKE_REQUESTED], 0,
                self->id_hex, self->pubkey_hex, self->event_kind, emoji);

  /* Close the popover */
  if (self->emoji_picker_popover && GTK_IS_POPOVER(self->emoji_picker_popover)) {
    gtk_popover_popdown(GTK_POPOVER(self->emoji_picker_popover));
  }
}

/* NIP-25: Create emoji picker popover with common reactions */
static void ensure_emoji_picker_popover(GnostrNoteCardRow *self) {
  if (self->emoji_picker_popover) return;

  self->emoji_picker_popover = gtk_popover_new();

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);

  /* Common reaction emojis */
  const char *emojis[] = {"+", "\xf0\x9f\x91\x8d", "\xe2\x9d\xa4\xef\xb8\x8f", "\xf0\x9f\x94\xa5",
                          "\xf0\x9f\x98\x82", "\xf0\x9f\xa4\x94", "\xf0\x9f\x91\x80", "-", NULL};
  const char *labels[] = {"Like", NULL, NULL, NULL, NULL, NULL, NULL, "Dislike", NULL};

  for (int i = 0; emojis[i] != NULL; i++) {
    GtkWidget *btn = gtk_button_new_with_label(emojis[i]);
    gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
    g_object_set_data_full(G_OBJECT(btn), "emoji", g_strdup(emojis[i]), g_free);
    if (labels[i]) {
      gtk_widget_set_tooltip_text(btn, labels[i]);
    }
    g_signal_connect(btn, "clicked", G_CALLBACK(on_emoji_selected), self);
    gtk_box_append(GTK_BOX(box), btn);
  }

  gtk_popover_set_child(GTK_POPOVER(self->emoji_picker_popover), box);
  gtk_widget_set_parent(self->emoji_picker_popover, GTK_WIDGET(self->btn_like));
}

static void on_like_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Default like: emit signal with "+" content */
    g_signal_emit(self, signals[SIGNAL_LIKE_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex, self->event_kind, "+");
  }
}

/* NIP-25: Long press/right-click to show emoji picker */
static void on_like_long_press(GtkGestureLongPress *gesture, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture; (void)x; (void)y;
  if (!self) return;

  ensure_emoji_picker_popover(self);
  gtk_popover_popup(GTK_POPOVER(self->emoji_picker_popover));
}

/* NIP-25: Show reaction details popover (who reacted) */
static void on_like_count_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture; (void)n_press; (void)x; (void)y;
  if (!self || !self->reaction_breakdown) return;

  /* Create/update reactions popover */
  gboolean popover_created = FALSE;
  if (!self->reactions_popover) {
    self->reactions_popover = gtk_popover_new();
    popover_created = TRUE;
  }

  /* Build content showing reaction breakdown */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);

  GtkWidget *title = gtk_label_new("Reactions");
  gtk_widget_add_css_class(title, "heading");
  gtk_box_append(GTK_BOX(box), title);

  /* Show breakdown by emoji */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->reaction_breakdown);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *emoji = (const char *)key;
    guint count = GPOINTER_TO_UINT(value);
    char *text = g_strdup_printf("%s  %u", emoji, count);
    GtkWidget *row = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(row), 0.0);
    gtk_box_append(GTK_BOX(box), row);
    g_free(text);
  }

  gtk_popover_set_child(GTK_POPOVER(self->reactions_popover), box);
  if (popover_created) {
    gtk_widget_set_parent(self->reactions_popover, GTK_WIDGET(self->btn_like));
  }
  gtk_popover_popup(GTK_POPOVER(self->reactions_popover));
}

static void on_zap_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex && self->pubkey_hex) {
    /* Emit zap requested signal with event_id, pubkey, and lud16 if available */
    g_signal_emit(self, signals[SIGNAL_ZAP_REQUESTED], 0,
                  self->id_hex, self->pubkey_hex, self->author_lud16);
  }
}

static void on_bookmark_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self && self->id_hex) {
    /* Toggle bookmark state */
    self->is_bookmarked = !self->is_bookmarked;

    /* Update button icon */
    if (GTK_IS_BUTTON(self->btn_bookmark)) {
      gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
        self->is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
    }

    /* Emit signal so main window can handle NIP-51 storage */
    g_signal_emit(self, signals[SIGNAL_BOOKMARK_TOGGLED], 0,
                  self->id_hex, self->is_bookmarked);
  }
}

static void on_thread_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (self) {
    /* Use root_id if available, otherwise use the note's own id as thread root */
    const char *thread_root = self->root_id ? self->root_id : self->id_hex;
    if (thread_root) {
      g_signal_emit(self, signals[SIGNAL_VIEW_THREAD_REQUESTED], 0, thread_root);
    }
  }
}

/* Callback when reply indicator is clicked - navigate to parent note */
static void on_reply_indicator_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  if (!self) return;

  /* Navigate to parent note if available, otherwise to thread root */
  const char *target = self->parent_id ? self->parent_id : self->root_id;
  if (target && *target) {
    g_signal_emit(self, signals[SIGNAL_NAVIGATE_TO_NOTE], 0, target);
  }
}

/* Callback when reply count badge is clicked - open thread view */
static void on_reply_count_clicked(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  if (!self || !self->id_hex) return;

  /* Open thread view with this note as the root */
  g_signal_emit(self, signals[SIGNAL_VIEW_THREAD_REQUESTED], 0, self->id_hex);
}

/* NIP-36: Callback when user clicks "Show Content" button for sensitive content */
static void on_show_sensitive_clicked(GtkButton *btn, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  (void)btn;
  if (!self) return;

  /* Mark sensitive content as revealed */
  self->sensitive_content_revealed = TRUE;

  /* Hide the overlay and show the content */
  if (self->sensitive_content_overlay && GTK_IS_WIDGET(self->sensitive_content_overlay)) {
    gtk_widget_set_visible(self->sensitive_content_overlay, FALSE);
  }

  /* Remove blur CSS class from content */
  if (self->content_label && GTK_IS_WIDGET(self->content_label)) {
    gtk_widget_remove_css_class(self->content_label, "content-blurred");
    gtk_widget_set_visible(self->content_label, TRUE);
  }
  if (self->media_box && GTK_IS_WIDGET(self->media_box)) {
    gtk_widget_remove_css_class(self->media_box, "content-blurred");
  }
  if (self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    gtk_widget_remove_css_class(self->embed_box, "content-blurred");
  }
  if (self->og_preview_container && GTK_IS_WIDGET(self->og_preview_container)) {
    gtk_widget_remove_css_class(self->og_preview_container, "content-blurred");
  }
}

static void gnostr_note_card_row_class_init(GnostrNoteCardRowClass *klass) {
  GtkWidgetClass *wclass = GTK_WIDGET_CLASS(klass);
  GObjectClass *gclass = G_OBJECT_CLASS(klass);
  gclass->dispose = gnostr_note_card_row_dispose;
  gclass->finalize = gnostr_note_card_row_finalize;

  gtk_widget_class_set_layout_manager_type(wclass, GTK_TYPE_BOX_LAYOUT);
  gtk_widget_class_set_template_from_resource(wclass, UI_RESOURCE);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, root);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_avatar);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_display_name);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_menu);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_reply);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_repost);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_repost_count);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_like);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_like_count);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_zap);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_zap_count);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_bookmark);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_thread);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_indicator_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_indicator_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_count_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, reply_count_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, avatar_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, avatar_initials);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, avatar_image);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_display);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_handle);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_nip05_separator);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_nip05);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_timestamp_separator);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, lbl_timestamp);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, content_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, media_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, embed_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, og_preview_container);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, actions_box);
  /* NIP-14 subject label */
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, subject_label);
  /* NIP-36 sensitive content widgets */
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, sensitive_content_overlay);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, sensitive_warning_box);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, sensitive_warning_label);
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, btn_show_sensitive);
  /* Hashtags container */
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, hashtags_box);
  /* NIP-32 labels container */
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, labels_box);
  /* NIP-73 external content IDs container */
  gtk_widget_class_bind_template_child(wclass, GnostrNoteCardRow, external_ids_box);

  signals[SIGNAL_OPEN_NOSTR_TARGET] = g_signal_new("open-nostr-target",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_OPEN_URL] = g_signal_new("open-url",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_REQUEST_EMBED] = g_signal_new("request-embed",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_OPEN_PROFILE] = g_signal_new("open-profile",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_REPLY_REQUESTED] = g_signal_new("reply-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_REPOST_REQUESTED] = g_signal_new("repost-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_QUOTE_REQUESTED] = g_signal_new("quote-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  /* NIP-25: like-requested signal now includes kind and reaction content
   * Parameters: id_hex, pubkey_hex, event_kind, reaction_content */
  signals[SIGNAL_LIKE_REQUESTED] = g_signal_new("like-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
  signals[SIGNAL_ZAP_REQUESTED] = g_signal_new("zap-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_VIEW_THREAD_REQUESTED] = g_signal_new("view-thread-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_MUTE_USER_REQUESTED] = g_signal_new("mute-user-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_MUTE_THREAD_REQUESTED] = g_signal_new("mute-thread-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SHOW_TOAST] = g_signal_new("show-toast",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_BOOKMARK_TOGGLED] = g_signal_new("bookmark-toggled",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_BOOLEAN);
  signals[SIGNAL_REPORT_NOTE_REQUESTED] = g_signal_new("report-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIGNAL_SHARE_NOTE_REQUESTED] = g_signal_new("share-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_SEARCH_HASHTAG] = g_signal_new("search-hashtag",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIGNAL_NAVIGATE_TO_NOTE] = g_signal_new("navigate-to-note",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
  /* NIP-09 deletion request: id_hex, pubkey_hex */
  signals[SIGNAL_DELETE_NOTE_REQUESTED] = g_signal_new("delete-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);
  /* NIP-22 comment request: id_hex, kind, pubkey_hex */
  signals[SIGNAL_COMMENT_REQUESTED] = g_signal_new("comment-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING);
  /* NIP-32 label request: id_hex, namespace, label, pubkey_hex */
  signals[SIGNAL_LABEL_NOTE_REQUESTED] = g_signal_new("label-note-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  /* NIP-84 highlight request: highlighted_text, context, id_hex, pubkey_hex */
  signals[SIGNAL_HIGHLIGHT_REQUESTED] = g_signal_new("highlight-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  /* NIP-04/17 DM request: open DM conversation with pubkey_hex */
  signals[SIGNAL_DM_REQUESTED] = g_signal_new("dm-requested",
    G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void gnostr_note_card_row_init(GnostrNoteCardRow *self) {
  /* Explicitly initialize embedded widget pointers to NULL before template init.
   * These may be accessed in prepare_for_unbind during GTK widget recycling.
   * GObject should zero-initialize, but be explicit to prevent garbage pointer
   * crashes in GNOSTR_IS_NOTE_EMBED / OG_IS_PREVIEW_WIDGET type checks.
   * nostrc-csj: Fix ASAN heap-buffer-overflow in prepare_for_unbind. */
  self->note_embed = NULL;
  self->og_preview = NULL;

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Create shared cancellable for all async operations */
  self->async_cancellable = g_cancellable_new();
  
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_reply),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Note Reply", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_menu),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Note More", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_avatar),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Open Profile", -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_display_name),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Open Profile", -1);
  gtk_widget_add_css_class(GTK_WIDGET(self), "note-card");
  if (GTK_IS_LABEL(self->content_label)) {
    gtk_label_set_wrap(GTK_LABEL(self->content_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->content_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_selectable(GTK_LABEL(self->content_label), FALSE);
    g_signal_connect(self->content_label, "activate-link", G_CALLBACK(on_content_activate_link), self);
  }
  /* Connect profile click handlers */
  if (GTK_IS_BUTTON(self->btn_avatar)) {
    g_signal_connect(self->btn_avatar, "clicked", G_CALLBACK(on_avatar_clicked), self);
  }
  if (GTK_IS_BUTTON(self->btn_display_name)) {
    g_signal_connect(self->btn_display_name, "clicked", G_CALLBACK(on_display_name_clicked), self);
  }
  /* Connect menu button to show JSON viewer */
  if (GTK_IS_BUTTON(self->btn_menu)) {
    g_signal_connect(self->btn_menu, "clicked", G_CALLBACK(on_menu_clicked), self);
  }
  /* Connect reply button */
  if (GTK_IS_BUTTON(self->btn_reply)) {
    g_signal_connect(self->btn_reply, "clicked", G_CALLBACK(on_reply_clicked), self);
  }
  /* Connect repost button */
  if (GTK_IS_BUTTON(self->btn_repost)) {
    g_signal_connect(self->btn_repost, "clicked", G_CALLBACK(on_repost_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_repost),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Repost Note", -1);
  }
  /* Connect like button and NIP-25 long-press for emoji picker */
  if (GTK_IS_BUTTON(self->btn_like)) {
    g_signal_connect(self->btn_like, "clicked", G_CALLBACK(on_like_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_like),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Like Note", -1);
    /* Long press on like button shows emoji picker */
    GtkGesture *like_long_press = gtk_gesture_long_press_new();
    gtk_gesture_long_press_set_delay_factor(GTK_GESTURE_LONG_PRESS(like_long_press), 1.0);
    g_signal_connect(like_long_press, "pressed", G_CALLBACK(on_like_long_press), self);
    gtk_widget_add_controller(GTK_WIDGET(self->btn_like), GTK_EVENT_CONTROLLER(like_long_press));
  }
  /* NIP-25: Initialize reaction breakdown hash table */
  self->reaction_breakdown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->event_kind = 1;  /* Default to text note */
  /* Connect zap button */
  if (GTK_IS_BUTTON(self->btn_zap)) {
    g_signal_connect(self->btn_zap, "clicked", G_CALLBACK(on_zap_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_zap),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Zap Note", -1);
  }
  /* Connect bookmark button */
  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    g_signal_connect(self->btn_bookmark, "clicked", G_CALLBACK(on_bookmark_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_bookmark),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Bookmark Note", -1);
  }
  /* Connect view thread button */
  if (GTK_IS_BUTTON(self->btn_thread)) {
    g_signal_connect(self->btn_thread, "clicked", G_CALLBACK(on_thread_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_thread),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "View Thread", -1);
  }

  /* Make reply indicator clickable - navigate to parent note */
  if (GTK_IS_WIDGET(self->reply_indicator_box)) {
    GtkGesture *reply_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(reply_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(reply_click, "pressed", G_CALLBACK(on_reply_indicator_clicked), self);
    gtk_widget_add_controller(self->reply_indicator_box, GTK_EVENT_CONTROLLER(reply_click));
    /* Add CSS class for hover styling and cursor */
    gtk_widget_add_css_class(self->reply_indicator_box, "reply-indicator-clickable");
    gtk_widget_set_cursor_from_name(self->reply_indicator_box, "pointer");
  }

  /* Make reply count badge clickable - opens thread view */
  if (GTK_IS_WIDGET(self->reply_count_box)) {
    GtkGesture *count_click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(count_click), GDK_BUTTON_PRIMARY);
    g_signal_connect(count_click, "pressed", G_CALLBACK(on_reply_count_clicked), self);
    gtk_widget_add_controller(self->reply_count_box, GTK_EVENT_CONTROLLER(count_click));
    gtk_widget_set_cursor_from_name(self->reply_count_box, "pointer");
  }

  /* REMOVED: Right-click and long-press gestures that were showing context menu
   * at incorrect positions. Users access the menu via the 3-dot button only. */

  /* NIP-36: Connect sensitive content reveal button */
  if (GTK_IS_BUTTON(self->btn_show_sensitive)) {
    g_signal_connect(self->btn_show_sensitive, "clicked", G_CALLBACK(on_show_sensitive_clicked), self);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_show_sensitive),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, "Show Sensitive Content", -1);
  }

#ifdef HAVE_SOUP3
  self->avatar_cancellable = g_cancellable_new();
  /* Use shared session instead of per-widget session to reduce memory overhead.
   * Each SoupSession has significant overhead (TLS state, connection pool, etc.) */
  self->media_session = NULL; /* Will use gnostr_get_shared_soup_session() */
  self->media_cancellables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
#endif
}

GnostrNoteCardRow *gnostr_note_card_row_new(void) {
  return g_object_new(GNOSTR_TYPE_NOTE_CARD_ROW, NULL);
}

static void set_avatar_initials(GnostrNoteCardRow *self, const char *display, const char *handle) {
  if (!self || !GTK_IS_LABEL(self->avatar_initials)) return;
  const char *src = (display && *display) ? display : (handle && *handle ? handle : "AN");
  char initials[3] = {0}; int i = 0;
  for (const char *p = src; *p && i < 2; p++) if (g_ascii_isalnum(*p)) initials[i++] = g_ascii_toupper(*p);
  if (i == 0) { initials[0] = 'A'; initials[1] = 'N'; }
  gtk_label_set_text(GTK_LABEL(self->avatar_initials), initials);
  if (self->avatar_image) gtk_widget_set_visible(self->avatar_image, FALSE);
  gtk_widget_set_visible(self->avatar_initials, TRUE);
}

#ifdef HAVE_SOUP3
static void on_avatar_http_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || self->disposed) return;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (!bytes) { g_clear_error(&error); if (!self->disposed) set_avatar_initials(self, NULL, NULL); return; }
  if (self->disposed) { g_bytes_unref(bytes); return; }
  GdkTexture *tex = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);
  if (!tex) { g_clear_error(&error); if (!self->disposed) set_avatar_initials(self, NULL, NULL); return; }
  if (!self->disposed && GTK_IS_PICTURE(self->avatar_image)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(tex));
    gtk_widget_set_visible(self->avatar_image, TRUE);
  }
  if (!self->disposed && GTK_IS_WIDGET(self->avatar_initials)) gtk_widget_set_visible(self->avatar_initials, FALSE);
  g_object_unref(tex);
}

/* Helper to show broken image fallback in container */
static void show_broken_image_fallback(GtkWidget *container) {
  if (!GTK_IS_OVERLAY(container)) return;

  /* Hide spinner if present */
  GtkWidget *spinner = g_object_get_data(G_OBJECT(container), "loading-spinner");
  if (GTK_IS_SPINNER(spinner)) {
    gtk_spinner_stop(GTK_SPINNER(spinner));
    gtk_widget_set_visible(spinner, FALSE);
  }

  /* Show error image */
  GtkWidget *error_image = g_object_get_data(G_OBJECT(container), "error-image");
  if (GTK_IS_IMAGE(error_image)) {
    gtk_widget_set_visible(error_image, TRUE);
  }

  /* Hide picture */
  GtkWidget *picture = g_object_get_data(G_OBJECT(container), "media-picture");
  if (GTK_IS_PICTURE(picture)) {
    gtk_widget_set_visible(picture, FALSE);
  }
}

/* Helper to show loaded image in container */
static void show_loaded_image(GtkWidget *container) {
  if (!GTK_IS_OVERLAY(container)) return;

  /* Hide spinner */
  GtkWidget *spinner = g_object_get_data(G_OBJECT(container), "loading-spinner");
  if (GTK_IS_SPINNER(spinner)) {
    gtk_spinner_stop(GTK_SPINNER(spinner));
    gtk_widget_set_visible(spinner, FALSE);
  }

  /* Hide error image */
  GtkWidget *error_image = g_object_get_data(G_OBJECT(container), "error-image");
  if (GTK_IS_IMAGE(error_image)) {
    gtk_widget_set_visible(error_image, FALSE);
  }

  /* Show picture */
  GtkWidget *picture = g_object_get_data(G_OBJECT(container), "media-picture");
  if (GTK_IS_PICTURE(picture)) {
    gtk_widget_set_visible(picture, TRUE);
  }
}

/* Callback for media image loading.
 * CRITICAL: Uses GWeakRef to prevent use-after-free crash when
 * GtkListView recycles rows during scrolling. */
static void on_media_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  MediaLoadCtx *ctx = (MediaLoadCtx*)user_data;
  GError *error = NULL;

  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);
  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("Media: Failed to load image: %s", error->message);
    }
    g_error_free(error);
    media_load_ctx_free(ctx);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    media_load_ctx_free(ctx);
    return;
  }

  /* Create texture from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (error) {
    g_debug("Media: Failed to create texture: %s", error->message);
    g_error_free(error);
    media_load_ctx_free(ctx);
    return;
  }

  /* CRITICAL: Use g_weak_ref_get to safely check if widget still exists.
   * If widget was recycled/disposed during HTTP fetch, weak ref returns NULL
   * and we skip the update, preventing use-after-free crash. */
  GtkWidget *picture = g_weak_ref_get(&ctx->picture_ref);
  if (picture) {
    if (GTK_IS_PICTURE(picture)) {
      /* Get URL for caching from widget data */
      const char *url = g_object_get_data(G_OBJECT(picture), "image-url");
      if (url) {
        media_image_cache_put(url, texture);
      }
      gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(texture));
      /* Show the loaded image and hide spinner */
      GtkWidget *container = gtk_widget_get_parent(picture);
      if (container) show_loaded_image(container);
    }
    g_object_unref(picture); /* g_weak_ref_get returns a ref */
  } else {
    g_debug("Media: picture widget was recycled, skipping UI update");
  }

  g_object_unref(texture);
  media_load_ctx_free(ctx);
}

/* Load media image asynchronously - internal function that starts the actual fetch.
 * CRITICAL: Uses GWeakRef for the picture widget to prevent use-after-free
 * crash when GtkListView recycles rows during scrolling. */
static void load_media_image_internal(GnostrNoteCardRow *self, const char *url, GtkPicture *picture) {
  if (!url || !*url || !GTK_IS_PICTURE(picture)) return;

  /* Check cache first */
  GdkTexture *cached = media_image_cache_get(url);
  if (cached) {
    gtk_picture_set_paintable(picture, GDK_PAINTABLE(cached));
    GtkWidget *container = gtk_widget_get_parent(GTK_WIDGET(picture));
    if (container) show_loaded_image(container);
    g_object_unref(cached);
    return;
  }

  /* Create cancellable for this request */
  GCancellable *cancellable = g_cancellable_new();
  g_hash_table_insert(self->media_cancellables, g_strdup(url), cancellable);

  /* Create HTTP request */
  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) {
    g_debug("Media: Invalid image URL: %s", url);
    return;
  }

  /* CRITICAL: Use weak ref instead of strong ref to prevent crash
   * when widget is recycled before HTTP completes. */
  MediaLoadCtx *ctx = g_new0(MediaLoadCtx, 1);
  g_weak_ref_init(&ctx->picture_ref, picture);

  /* Start async fetch */
  soup_session_send_and_read_async(
    gnostr_get_shared_soup_session(),
    msg,
    G_PRIORITY_LOW,
    cancellable,
    on_media_image_loaded,
    ctx
  );

  g_object_unref(msg);
}

/* Lazy loading context for deferred media loading */
typedef struct {
  GnostrNoteCardRow *self;
  GtkPicture *picture;
  char *url;
  guint timeout_id;
  gulong map_handler_id;
  gulong unmap_handler_id;
  gboolean loaded;
} LazyLoadContext;

static void lazy_load_context_free(LazyLoadContext *ctx) {
  if (!ctx) return;
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
  }
  g_free(ctx->url);
  g_free(ctx);
}

/* Timeout callback - actually load the image after delay */
static gboolean on_lazy_load_timeout(gpointer user_data) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;

  if (!ctx || ctx->loaded) return G_SOURCE_REMOVE;

  /* Check if widgets are still valid */
  if (!GNOSTR_IS_NOTE_CARD_ROW(ctx->self) || !GTK_IS_PICTURE(ctx->picture)) {
    ctx->timeout_id = 0;
    return G_SOURCE_REMOVE;
  }

  /* Check if widget is still mapped (visible) */
  if (!gtk_widget_get_mapped(GTK_WIDGET(ctx->picture))) {
    ctx->timeout_id = 0;
    return G_SOURCE_REMOVE;
  }

  g_debug("Media: Lazy loading image: %s", ctx->url);
  ctx->loaded = TRUE;
  load_media_image_internal(ctx->self, ctx->url, ctx->picture);

  ctx->timeout_id = 0;
  return G_SOURCE_REMOVE;
}

/* Called when the picture widget becomes visible */
static void on_picture_mapped(GtkWidget *widget, gpointer user_data) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;
  (void)widget;

  if (!ctx || ctx->loaded) return;

  /* Cancel any pending timeout */
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
  }

  /* Schedule load after a short delay (150ms) to avoid loading during fast scrolling */
  ctx->timeout_id = g_timeout_add(150, on_lazy_load_timeout, ctx);
}

/* Called when the picture widget becomes hidden */
static void on_picture_unmapped(GtkWidget *widget, gpointer user_data) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;
  (void)widget;

  if (!ctx || ctx->loaded) return;

  /* Cancel pending load if user scrolled past */
  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
    g_debug("Media: Cancelled lazy load (scrolled past): %s", ctx->url);
  }
}

/* Called when the picture widget is destroyed */
static void on_lazy_load_picture_destroyed(gpointer user_data, GObject *where_the_object_was) {
  LazyLoadContext *ctx = (LazyLoadContext *)user_data;
  (void)where_the_object_was;
  lazy_load_context_free(ctx);
}

/* Load media image with lazy loading - defers actual loading until widget is visible */
static void load_media_image(GnostrNoteCardRow *self, const char *url, GtkPicture *picture) {
  if (!url || !*url || !GTK_IS_PICTURE(picture)) return;

  /* Create lazy loading context */
  LazyLoadContext *ctx = g_new0(LazyLoadContext, 1);
  ctx->self = self;
  ctx->picture = picture;
  ctx->url = g_strdup(url);
  ctx->loaded = FALSE;
  ctx->timeout_id = 0;

  /* Connect to map/unmap signals for visibility tracking */
  ctx->map_handler_id = g_signal_connect(picture, "map", G_CALLBACK(on_picture_mapped), ctx);
  ctx->unmap_handler_id = g_signal_connect(picture, "unmap", G_CALLBACK(on_picture_unmapped), ctx);

  /* Track widget destruction to free context */
  g_object_weak_ref(G_OBJECT(picture), on_lazy_load_picture_destroyed, ctx);

  /* If already mapped, start loading immediately */
  if (gtk_widget_get_mapped(GTK_WIDGET(picture))) {
    on_picture_mapped(GTK_WIDGET(picture), ctx);
  }
}
#endif

/* Use centralized avatar cache API (avatar_cache.h) */

void gnostr_note_card_row_set_author(GnostrNoteCardRow *self, const char *display_name, const char *handle, const char *avatar_url) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (GTK_IS_LABEL(self->lbl_display)) gtk_label_set_text(GTK_LABEL(self->lbl_display), (display_name && *display_name) ? display_name : (handle ? handle : _("Anonymous")));
  if (GTK_IS_LABEL(self->lbl_handle))  gtk_label_set_text(GTK_LABEL(self->lbl_handle), (handle && *handle) ? handle : "@anon");
  g_clear_pointer(&self->avatar_url, g_free);
  self->avatar_url = g_strdup(avatar_url);
  set_avatar_initials(self, display_name, handle);
  
#ifdef HAVE_SOUP3
  /* OPTIMIZATION: Check cache before downloading */
  if (avatar_url && *avatar_url && GTK_IS_PICTURE(self->avatar_image)) {
    g_debug("note_card: set_author called with avatar_url=%s", avatar_url);
    /* First, try to load from cache (memory or disk) */
    GdkTexture *cached = gnostr_avatar_try_load_cached(avatar_url);
    if (cached) {
      /* Cache hit! Apply immediately without HTTP request */
      g_debug("note_card: avatar cache HIT, displaying url=%s", avatar_url);
      gtk_picture_set_paintable(GTK_PICTURE(self->avatar_image), GDK_PAINTABLE(cached));
      gtk_widget_set_visible(self->avatar_image, TRUE);
      if (GTK_IS_WIDGET(self->avatar_initials)) {
        gtk_widget_set_visible(self->avatar_initials, FALSE);
      }
      g_object_unref(cached);
    } else {
      /* Cache miss - download asynchronously */
      g_debug("note_card: avatar cache MISS, downloading url=%s", avatar_url);
      gnostr_avatar_download_async(avatar_url, self->avatar_image, self->avatar_initials);
    }
  } else {
    if (!avatar_url || !*avatar_url) {
      g_debug("note_card: set_author called with NO avatar_url");
    } else if (!GTK_IS_PICTURE(self->avatar_image)) {
      g_warning("note_card: avatar_image is not a GtkPicture!");
    }
  }
#endif
}

/* Timer callback to update timestamp display */
static gboolean update_timestamp_tick(gpointer user_data) {
  /* CRITICAL: Don't use type-check macros on user_data - they dereference
   * the pointer and can crash if it's stale/freed. Check NULL first. */
  if (user_data == NULL) {
    return G_SOURCE_REMOVE;
  }

  GnostrNoteCardRow *self = (GnostrNoteCardRow *)user_data;

  /* Check disposed flag - if set, the widget is being torn down.
   * Also verify the timer ID is still valid (non-zero means we're still active) */
  if (self->disposed || self->timestamp_timer_id == 0) {
    return G_SOURCE_REMOVE;
  }

  /* Now safe to use type-check since disposed==FALSE means widget is valid */
  if (!GTK_IS_LABEL(self->lbl_timestamp)) {
    return G_SOURCE_REMOVE;
  }

  /* Additional safety: check widget is still in widget tree (has parent) */
  if (gtk_widget_get_parent(GTK_WIDGET(self)) == NULL) {
    self->timestamp_timer_id = 0;  /* Clear so we don't try to remove again */
    return G_SOURCE_REMOVE;
  }

  if (self->created_at > 0) {
    time_t now = time(NULL);
    long diff = (long)(now - (time_t)self->created_at);
    if (diff < 0) diff = 0;
    char buf[32];
    if (diff < 5) g_strlcpy(buf, "now", sizeof(buf));
    else if (diff < 3600) g_snprintf(buf, sizeof(buf), "%ldm", diff/60);
    else if (diff < 86400) g_snprintf(buf, sizeof(buf), "%ldh", diff/3600);
    else g_snprintf(buf, sizeof(buf), "%ldd", diff/86400);
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), buf);
  }

  return G_SOURCE_CONTINUE;
}

void gnostr_note_card_row_set_timestamp(GnostrNoteCardRow *self, gint64 created_at, const char *fallback_ts) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->lbl_timestamp)) return;

  /* Store the created_at timestamp */
  self->created_at = created_at;

  /* Update the display immediately */
  if (created_at > 0) {
    time_t now = time(NULL);
    long diff = (long)(now - (time_t)created_at);
    if (diff < 0) diff = 0;
    char buf[32];
    if (diff < 5) g_strlcpy(buf, "now", sizeof(buf));
    else if (diff < 3600) g_snprintf(buf, sizeof(buf), "%ldm", diff/60);
    else if (diff < 86400) g_snprintf(buf, sizeof(buf), "%ldh", diff/3600);
    else g_snprintf(buf, sizeof(buf), "%ldd", diff/86400);
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), buf);

    /* Set tooltip with full date/time */
    GDateTime *dt = g_date_time_new_from_unix_local(created_at);
    if (dt) {
      gchar *full_date = g_date_time_format(dt, "%B %d, %Y at %l:%M %p");
      if (full_date) {
        gtk_widget_set_tooltip_text(GTK_WIDGET(self->lbl_timestamp), full_date);
        g_free(full_date);
      }
      g_date_time_unref(dt);
    }

    /* Remove old timer if exists */
    if (self->timestamp_timer_id > 0) {
      g_source_remove(self->timestamp_timer_id);
    }

    /* Add timer to update every 60 seconds */
    self->timestamp_timer_id = g_timeout_add_seconds(60, update_timestamp_tick, self);
  } else {
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), fallback_ts ? fallback_ts : "now");
  }
}

static gchar *escape_markup(const char *s) {
  if (!s) return g_strdup("");
  return g_markup_escape_text(s, -1);
}

static gboolean is_image_url(const char *u) {
  if (!u) return FALSE;
  gchar *lower = g_ascii_strdown(u, -1);
  const char *exts[] = {".jpg",".jpeg",".png",".gif",".webp",".bmp",".svg",".avif",".ico",".tiff",".tif",".heic",".heif"};
  gboolean result = FALSE;
  for (guint i=0; i<G_N_ELEMENTS(exts); i++) {
    if (g_str_has_suffix(lower, exts[i])) {
      result = TRUE;
      break;
    }
  }
  g_free(lower);
  return result;
}

static gboolean is_video_url(const char *u) {
  if (!u) return FALSE;
  gchar *lower = g_ascii_strdown(u, -1);
  const char *exts[] = {".mp4",".webm",".mov",".avi",".mkv",".m4v"};
  gboolean result = FALSE;
  for (guint i=0; i<G_N_ELEMENTS(exts); i++) {
    if (g_str_has_suffix(lower, exts[i])) {
      result = TRUE;
      break;
    }
  }
  g_free(lower);
  return result;
}

/**
 * create_image_container:
 * @url: the image URL
 * @height: the preferred height for the image
 * @alt_text: (nullable): alt text for accessibility tooltip
 *
 * Creates a GtkOverlay container containing:
 * - GtkPicture for the image (hidden initially)
 * - GtkSpinner for loading state (visible and spinning initially)
 * - GtkImage for broken image fallback (hidden initially)
 *
 * The container stores references to its children via g_object_set_data():
 * - "media-picture": the GtkPicture widget
 * - "loading-spinner": the GtkSpinner widget
 * - "error-image": the broken image GtkImage widget
 * - "image-url": the URL string (for click handler)
 *
 * Returns: (transfer full): the GtkOverlay container widget
 */
static GtkWidget *create_image_container(const char *url, int height, const char *alt_text) {
  /* Create overlay container */
  GtkWidget *container = gtk_overlay_new();
  gtk_widget_add_css_class(container, "media-image-container");
  gtk_widget_set_size_request(container, -1, height);
  gtk_widget_set_hexpand(container, TRUE);
  gtk_widget_set_vexpand(container, FALSE);

  /* Create picture widget (visible but empty initially - needed for lazy loading map signal) */
  GtkWidget *pic = gtk_picture_new();
  gtk_widget_add_css_class(pic, "note-media-image");
  gtk_widget_add_css_class(pic, "clickable-image");
  gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
  gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request(pic, -1, height);
  gtk_widget_set_hexpand(pic, TRUE);
  gtk_widget_set_vexpand(pic, FALSE);
  /* Picture must be visible for lazy loading to work (map signal won't fire otherwise).
   * The spinner overlay covers it until loading completes. */
  gtk_widget_set_visible(pic, TRUE);
  gtk_widget_set_cursor_from_name(pic, "pointer");
  if (alt_text && *alt_text) {
    gtk_widget_set_tooltip_text(pic, alt_text);
  }
  gtk_overlay_set_child(GTK_OVERLAY(container), pic);

  /* Create loading spinner (visible and active initially) */
  GtkWidget *spinner = gtk_spinner_new();
  gtk_widget_add_css_class(spinner, "media-loading-spinner");
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(spinner, 32, 32);
  gtk_spinner_start(GTK_SPINNER(spinner));
  gtk_widget_set_visible(spinner, TRUE);
  gtk_overlay_add_overlay(GTK_OVERLAY(container), spinner);

  /* Create error image (hidden initially, shown on load failure) */
  GtkWidget *error_image = gtk_image_new_from_icon_name("image-missing-symbolic");
  gtk_widget_add_css_class(error_image, "media-error-image");
  gtk_image_set_pixel_size(GTK_IMAGE(error_image), 48);
  gtk_widget_set_halign(error_image, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(error_image, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(error_image, FALSE);
  gtk_widget_set_tooltip_text(error_image, "Failed to load image");
  gtk_overlay_add_overlay(GTK_OVERLAY(container), error_image);

  /* Store references for access in callbacks */
  g_object_set_data(G_OBJECT(container), "media-picture", pic);
  g_object_set_data(G_OBJECT(container), "loading-spinner", spinner);
  g_object_set_data(G_OBJECT(container), "error-image", error_image);
  g_object_set_data_full(G_OBJECT(container), "image-url", g_strdup(url), g_free);
  /* Also store URL on picture for on_media_image_clicked compatibility */
  g_object_set_data_full(G_OBJECT(pic), "image-url", g_strdup(url), g_free);

  return container;
}

/* Image click handler - opens full-size image viewer with gallery support */
static void on_media_image_clicked(GtkGestureClick *gesture,
                                    int n_press,
                                    double x, double y,
                                    gpointer user_data) {
  (void)n_press;
  (void)x;
  (void)y;
  (void)user_data;

  /* Get the clicked picture widget */
  GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
  if (!pic) return;

  /* Get the URL of this image */
  const char *clicked_url = g_object_get_data(G_OBJECT(pic), "image-url");
  if (!clicked_url || !*clicked_url) return;

  /* Navigate up to find the media_box:
   * New structure: picture -> overlay (container) -> media_box
   * Old structure: picture -> media_box (for backwards compat) */
  GtkWidget *container = gtk_widget_get_parent(pic);
  GtkWidget *media_box = NULL;

  if (GTK_IS_OVERLAY(container)) {
    /* New container structure */
    media_box = gtk_widget_get_parent(container);
  } else if (GTK_IS_BOX(container)) {
    /* Old/fallback structure - picture directly in media_box */
    media_box = container;
    container = NULL;
  }

  if (!media_box || !GTK_IS_BOX(media_box)) {
    /* Fallback: single image mode */
    GtkRoot *root = gtk_widget_get_root(pic);
    GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;
    GnostrImageViewer *viewer = gnostr_image_viewer_new(parent);
    gnostr_image_viewer_set_image_url(viewer, clicked_url);
    gnostr_image_viewer_present(viewer);
    return;
  }

  /* Collect all image URLs from media_box */
  GPtrArray *urls = g_ptr_array_new();
  guint clicked_index = 0;
  GtkWidget *child = gtk_widget_get_first_child(media_box);
  while (child) {
    /* Check for new container structure (GtkOverlay) or old structure (GtkPicture) */
    const char *url = NULL;
    GtkWidget *check_pic = NULL;

    if (GTK_IS_OVERLAY(child)) {
      /* New structure: get URL from container's data */
      url = g_object_get_data(G_OBJECT(child), "image-url");
      check_pic = g_object_get_data(G_OBJECT(child), "media-picture");
    } else if (GTK_IS_PICTURE(child)) {
      /* Old structure: picture directly in media_box */
      url = g_object_get_data(G_OBJECT(child), "image-url");
      check_pic = child;
    }

    if (url && *url) {
      if (check_pic == pic || (container && child == container)) {
        clicked_index = urls->len;
      }
      g_ptr_array_add(urls, (gpointer)url);
    }
    child = gtk_widget_get_next_sibling(child);
  }
  g_ptr_array_add(urls, NULL);  /* NULL terminate */

  /* Get parent window */
  GtkRoot *root = gtk_widget_get_root(pic);
  GtkWindow *parent = GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL;

  /* Create and show the image viewer with gallery */
  GnostrImageViewer *viewer = gnostr_image_viewer_new(parent);
  if (urls->len > 2) {  /* More than just clicked + NULL terminator */
    gnostr_image_viewer_set_gallery(viewer, (const char * const *)urls->pdata, clicked_index);
  } else {
    gnostr_image_viewer_set_image_url(viewer, clicked_url);
  }
  gnostr_image_viewer_present(viewer);

  g_ptr_array_free(urls, TRUE);
}

static gboolean is_media_url(const char *u) {
  return is_image_url(u) || is_video_url(u);
}

/* Extract clean URL from token, stripping trailing punctuation.
 * Handles: trailing periods, commas, semicolons, unbalanced parens/brackets.
 * Returns newly allocated clean URL and sets suffix to trailing characters.
 */
static gchar *extract_clean_url(const char *token, gchar **suffix) {
  if (!token || !*token) {
    if (suffix) *suffix = g_strdup("");
    return NULL;
  }
  size_t len = strlen(token);
  if (len == 0) {
    if (suffix) *suffix = g_strdup("");
    return NULL;
  }
  size_t end = len;
  int paren_balance = 0, bracket_balance = 0;
  /* Count balanced parens/brackets */
  for (size_t i = 0; i < len; i++) {
    if (token[i] == '(') paren_balance++;
    else if (token[i] == ')') paren_balance--;
    else if (token[i] == '[') bracket_balance++;
    else if (token[i] == ']') bracket_balance--;
  }
  /* Trim trailing punctuation */
  while (end > 0) {
    char c = token[end - 1];
    if (c == ',' || c == ';' || c == '!' || c == '\'' || c == '"' || c == '.') {
      end--;
      continue;
    }
    if (c == ':' && end > 1 && !g_ascii_isdigit(token[end - 2])) {
      end--;
      continue;
    }
    if (c == ')' && paren_balance < 0) {
      paren_balance++;
      end--;
      continue;
    }
    if (c == ']' && bracket_balance < 0) {
      bracket_balance++;
      end--;
      continue;
    }
    break;
  }
  if (suffix) *suffix = g_strdup(token + end);
  return (end > 0) ? g_strndup(token, end) : NULL;
}

/* Check if token starts with URL prefix */
static gboolean token_is_url(const char *t) {
  return t && (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://") || g_str_has_prefix(t, "www."));
}

/* Check if token is a nostr entity */
static gboolean token_is_nostr(const char *t) {
  return t && (g_str_has_prefix(t, "nostr:") || g_str_has_prefix(t, "note1") || g_str_has_prefix(t, "npub1") ||
               g_str_has_prefix(t, "nevent1") || g_str_has_prefix(t, "nprofile1") || g_str_has_prefix(t, "naddr1"));
}

/* Check if token is a hashtag (#word) */
static gboolean token_is_hashtag(const char *t) {
  if (!t || t[0] != '#' || t[1] == '\0') return FALSE;
  /* Must have at least one alphanumeric char after # */
  return g_ascii_isalnum(t[1]) || (unsigned char)t[1] > 127; /* Allow Unicode */
}

/* Extract hashtag text (without # prefix and trailing punctuation) */
static gchar *extract_hashtag(const char *t, gchar **suffix) {
  if (!t || t[0] != '#') { if (suffix) *suffix = NULL; return NULL; }
  const char *start = t + 1; /* Skip # */
  size_t len = strlen(start);
  /* Find end of hashtag (alphanumeric, underscore, or high-byte UTF-8) */
  size_t end = 0;
  while (end < len) {
    unsigned char c = (unsigned char)start[end];
    if (g_ascii_isalnum(c) || c == '_' || c > 127) {
      end++;
    } else {
      break;
    }
  }
  if (end == 0) { if (suffix) *suffix = NULL; return NULL; }
  if (suffix) *suffix = (end < len) ? g_strdup(start + end) : NULL;
  return g_strndup(start, end);
}

/* NIP-27: Check if nostr token is a profile mention (npub/nprofile) */
static gboolean token_is_nostr_profile(const char *t) {
  if (!t) return FALSE;
  const char *entity = t;
  if (g_str_has_prefix(t, "nostr:")) entity = t + 6;
  return g_str_has_prefix(entity, "npub1") || g_str_has_prefix(entity, "nprofile1");
}

/* NIP-27: Check if nostr token is an event mention (note/nevent/naddr) */
static gboolean token_is_nostr_event(const char *t) {
  if (!t) return FALSE;
  const char *entity = t;
  if (g_str_has_prefix(t, "nostr:")) entity = t + 6;
  return g_str_has_prefix(entity, "note1") || g_str_has_prefix(entity, "nevent1") ||
         g_str_has_prefix(entity, "naddr1");
}

/* Context for NIP-14 subject tag extraction callback */
typedef struct {
  gchar *subject;
} SubjectExtractContext;

/* Callback for extracting subject tag */
static bool extract_subject_callback(size_t index, const char *tag_json, void *user_data) {
  (void)index;
  SubjectExtractContext *ctx = (SubjectExtractContext *)user_data;
  if (!tag_json || !ctx || ctx->subject) return true; /* Stop if already found */

  if (!nostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 0, &tag_name) != 0 || !tag_name) {
    return true;
  }

  if (g_strcmp0(tag_name, "subject") != 0) {
    free(tag_name);
    return true;
  }
  free(tag_name);

  char *subject_value = NULL;
  if (nostr_json_get_array_string(tag_json, NULL, 1, &subject_value) != 0 || !subject_value || !*subject_value) {
    free(subject_value);
    return true;
  }

  /* Truncate to 80 chars per NIP-14 recommendation */
  if (strlen(subject_value) > 80) {
    ctx->subject = g_strndup(subject_value, 77);
    gchar *truncated = g_strdup_printf("%s...", ctx->subject);
    g_free(ctx->subject);
    ctx->subject = truncated;
  } else {
    ctx->subject = g_strdup(subject_value);
  }

  free(subject_value);
  return false; /* Stop iteration - found subject */
}

/* NIP-14: Extract subject tag from tags JSON array
 * Returns newly allocated string or NULL if no subject found.
 * Caller must free the returned string.
 */
static gchar *extract_subject_from_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;
  if (!nostr_json_is_array_str(tags_json)) return NULL;

  SubjectExtractContext ctx = { .subject = NULL };
  nostr_json_array_foreach_root(tags_json, extract_subject_callback, &ctx);

  return ctx.subject;
}

/* NIP-27: Format nostr mention for display (truncated bech32 with prefix)
 * Returns a newly allocated string. Caller must free.
 * Profile mentions: @{display_name} or @{name} or @{nip05} or truncated bech32
 * Event mentions: 📝note1abc...xyz (first 9 + last 4 chars of bech32)
 */
static gchar *format_nostr_mention_display(const char *t) {
  if (!t) return NULL;

  /* Strip nostr: prefix if present */
  const char *entity = t;
  if (g_str_has_prefix(t, "nostr:")) entity = t + 6;

  size_t len = strlen(entity);

  if (token_is_nostr_profile(t)) {
    /* Profile mention: try to resolve to display name */
    char *pubkey_hex = NULL;

    if (g_str_has_prefix(entity, "npub1")) {
      /* Decode npub to get pubkey bytes */
      uint8_t pubkey[32];
      if (nostr_nip19_decode_npub(entity, pubkey) == 0) {
        /* Convert bytes to hex string */
        pubkey_hex = g_malloc(65);
        for (int i = 0; i < 32; i++) {
          snprintf(pubkey_hex + i*2, 3, "%02x", pubkey[i]);
        }
      }
    } else if (g_str_has_prefix(entity, "nprofile1")) {
      /* Decode nprofile to get pubkey hex directly */
      NostrProfilePointer *pp = NULL;
      if (nostr_nip19_decode_nprofile(entity, &pp) == 0 && pp && pp->public_key) {
        pubkey_hex = g_strdup(pp->public_key);
        nostr_profile_pointer_free(pp);
      }
    }

    /* Look up profile if we have a pubkey */
    if (pubkey_hex) {
      GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
      if (meta) {
        const char *name = NULL;
        /* Priority: display_name > name > nip05 */
        if (meta->display_name && meta->display_name[0]) {
          name = meta->display_name;
        } else if (meta->name && meta->name[0]) {
          name = meta->name;
        } else if (meta->nip05 && meta->nip05[0]) {
          name = meta->nip05;
        }

        if (name) {
          gchar *result = g_strdup_printf("@%s", name);
          gnostr_profile_meta_free(meta);
          g_free(pubkey_hex);
          return result;
        }
        gnostr_profile_meta_free(meta);
      }
      g_free(pubkey_hex);
    }

    /* Fallback: truncated bech32 */
    if (len > 16) {
      /* Truncate: show first 8 chars + ... + last 4 chars */
      return g_strdup_printf("@%.*s…%s", 8, entity, entity + len - 4);
    } else {
      return g_strdup_printf("@%s", entity);
    }
  } else if (token_is_nostr_event(t)) {
    /* Event mention: show with note emoji */
    if (len > 17) {
      /* Truncate: show first 9 chars + ... + last 4 chars */
      return g_strdup_printf("📝%.*s…%s", 9, entity, entity + len - 4);
    } else {
      return g_strdup_printf("📝%s", entity);
    }
  }

  /* Fallback: return truncated version */
  if (len > 20) {
    return g_strdup_printf("%.*s…%s", 12, entity, entity + len - 4);
  }
  return g_strdup(entity);
}

void gnostr_note_card_row_set_content(GnostrNoteCardRow *self, const char *content) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->content_label)) return;

  /* Store plain text content for clipboard operations */
  g_clear_pointer(&self->content_text, g_free);
  self->content_text = g_strdup(content);

  /* Parse content: detect URLs and nostr entities, handle trailing punctuation */
  GString *out = g_string_new("");
  if (content && *content) {
    gchar **tokens = g_strsplit_set(content, " \n\t", -1);
    for (guint i=0; tokens && tokens[i]; i++) {
      const char *t = tokens[i];
      if (t[0]=='\0') { g_string_append(out, " "); continue; }
      gboolean is_url = token_is_url(t);
      gboolean is_nostr = token_is_nostr(t);
      gboolean is_hashtag = token_is_hashtag(t);
      if (is_nostr) {
        /* NIP-27: Handle nostr mentions with formatted display text */
        gchar *suffix = NULL;
        gchar *clean = extract_clean_url(t, &suffix);
        if (clean && *clean) {
          /* Ensure nostr: prefix for href */
          gchar *href = g_str_has_prefix(clean, "nostr:") ? g_strdup(clean) : g_strdup_printf("nostr:%s", clean);
          gchar *esc_href = g_markup_escape_text(href, -1);
          /* Format display text based on mention type (NIP-27) */
          gchar *display = format_nostr_mention_display(clean);
          gchar *esc_display = g_markup_escape_text(display ? display : clean, -1);
          /* Add CSS class for styling via span - profile vs event mentions */
          if (token_is_nostr_profile(clean)) {
            g_string_append_printf(out, "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);
          } else {
            g_string_append_printf(out, "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);
          }
          g_free(display);
          g_free(esc_href);
          g_free(esc_display);
          g_free(href);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(clean);
        g_free(suffix);
      } else if (is_url) {
        /* Handle regular URLs */
        gchar *suffix = NULL;
        gchar *clean = extract_clean_url(t, &suffix);
        if (clean && *clean) {
          /* For www. URLs, use https:// in href */
          gchar *href = g_str_has_prefix(clean, "www.") ? g_strdup_printf("https://%s", clean) : g_strdup(clean);
          gchar *esc_href = g_markup_escape_text(href, -1);
          /* Shorten display URL if longer than 40 chars to fit 640px width */
          gchar *display_url = NULL;
          gsize clean_len = strlen(clean);
          if (clean_len > 40) {
            display_url = g_strdup_printf("%.35s...", clean);
          } else {
            display_url = g_strdup(clean);
          }
          gchar *esc_display = g_markup_escape_text(display_url, -1);
          g_string_append_printf(out, "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);
          g_free(esc_href);
          g_free(esc_display);
          g_free(display_url);
          g_free(href);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(clean);
        g_free(suffix);
      } else if (is_hashtag) {
        gchar *suffix = NULL;
        gchar *tag = extract_hashtag(t, &suffix);
        if (tag && *tag) {
          gchar *esc_tag = g_markup_escape_text(tag, -1);
          /* Use hashtag: URI scheme for internal handling */
          g_string_append_printf(out, "<a href=\"hashtag:%s\">#%s</a>", esc_tag, esc_tag);
          g_free(esc_tag);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(tag);
        g_free(suffix);
      } else {
        gchar *esc = g_markup_escape_text(t, -1);
        g_string_append(out, esc);
        g_free(esc);
      }
      g_string_append_c(out, ' ');
    }
    g_strfreev(tokens);
  }
  gchar *markup = out->len ? g_string_free(out, FALSE) : g_string_free(out, TRUE);
  gboolean markup_allocated = (markup != NULL);
  if (!markup) markup = escape_markup(content);
  gtk_label_set_use_markup(GTK_LABEL(self->content_label), TRUE);
  gtk_label_set_markup(GTK_LABEL(self->content_label), markup ? markup : "");
  if (markup_allocated || markup) g_free(markup); /* Only free once */

  /* Media detection: detect images and videos in content and display them */
  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    /* Clear existing media widgets */
    GtkWidget *child = gtk_widget_get_first_child(self->media_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->media_box), child);
      child = next;
    }
    gtk_widget_set_visible(self->media_box, FALSE);
    
    if (content) {
      gchar **tokens = g_strsplit_set(content, " \n\t", -1);
      for (guint i=0; tokens && tokens[i]; i++) {
        const char *url = tokens[i];
        if (!url || !*url) continue;
        
        /* Check if it's an HTTP(S) URL */
        if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
          /* Handle images */
          if (is_image_url(url)) {
            /* Create image container with spinner and error fallback */
            GtkWidget *container = create_image_container(url, 300, NULL);
            GtkWidget *pic = g_object_get_data(G_OBJECT(container), "media-picture");

            /* Add click gesture to the picture to open image viewer */
            GtkGesture *click_gesture = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
            g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_media_image_clicked), NULL);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(click_gesture));

            gtk_box_append(GTK_BOX(self->media_box), container);
            gtk_widget_set_visible(self->media_box, TRUE);

#ifdef HAVE_SOUP3
            /* Load image asynchronously */
            load_media_image(self, url, GTK_PICTURE(pic));
#endif
          }
          /* Handle videos - use enhanced video player with controls overlay */
          else if (is_video_url(url)) {
            GnostrVideoPlayer *player = gnostr_video_player_new();
            gtk_widget_add_css_class(GTK_WIDGET(player), "note-media-video");
            /* Set minimum height only - let width be flexible */
            gtk_widget_set_size_request(GTK_WIDGET(player), -1, 300);
            gtk_widget_set_hexpand(GTK_WIDGET(player), FALSE);
            gtk_widget_set_vexpand(GTK_WIDGET(player), FALSE);

            /* Set video URI - settings (autoplay/loop) are read from GSettings */
            gnostr_video_player_set_uri(player, url);

            gtk_box_append(GTK_BOX(self->media_box), GTK_WIDGET(player));
            gtk_widget_set_visible(self->media_box, TRUE);
          }
        }
      }
      g_strfreev(tokens);
    }
  }

  /* Detect NIP-19/21 nostr: references and create embedded note widgets */
  if (self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    /* Clear existing embeds from the embed_box */
    if (GTK_IS_FRAME(self->embed_box)) {
      gtk_frame_set_child(GTK_FRAME(self->embed_box), NULL);
    }
    gtk_widget_set_visible(self->embed_box, FALSE);
    self->note_embed = NULL;

    const char *p = content;
    if (p && *p) {
      /* Scan for nostr: URIs and bech32 references */
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      const char *first_nostr_ref = NULL;

      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;

        /* Check for nostr: URI or bare bech32 (NIP-21) */
        if (g_str_has_prefix(t, "nostr:") ||
            g_str_has_prefix(t, "note1") ||
            g_str_has_prefix(t, "nevent1") ||
            g_str_has_prefix(t, "naddr1") ||
            g_str_has_prefix(t, "npub1") ||
            g_str_has_prefix(t, "nprofile1")) {
          first_nostr_ref = t;
          break;
        }
      }

      if (first_nostr_ref) {
        /* Create the NIP-21 embed widget */
        self->note_embed = gnostr_note_embed_new();
        
        /* Use parent's cancellable for lifecycle management */
        gnostr_note_embed_set_cancellable(self->note_embed, self->async_cancellable);

        /* Connect profile-clicked signal to relay to main window */
        g_signal_connect(self->note_embed, "profile-clicked",
                        G_CALLBACK(on_embed_profile_clicked), self);

        /* Set the nostr URI - this triggers async loading via NIP-19 decoding */
        gnostr_note_embed_set_nostr_uri(self->note_embed, first_nostr_ref);

        /* Add embed widget to the embed_box frame */
        if (GTK_IS_FRAME(self->embed_box)) {
          gtk_frame_set_child(GTK_FRAME(self->embed_box), GTK_WIDGET(self->note_embed));
        }
        gtk_widget_set_visible(self->embed_box, TRUE);

        /* Also emit the signal for timeline-level handling (backwards compatibility) */
        g_signal_emit(self, signals[SIGNAL_REQUEST_EMBED], 0, first_nostr_ref);
      }

      if (tokens) g_strfreev(tokens);
    }
  }

  /* Detect first HTTP(S) URL and create OG preview */
  if (self->og_preview_container && GTK_IS_BOX(self->og_preview_container)) {
    /* Clear any existing preview */
    if (self->og_preview) {
      gtk_box_remove(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
      self->og_preview = NULL;
    }
    gtk_widget_set_visible(self->og_preview_container, FALSE);
    
    const char *p = content;
    const char *url_start = NULL;
    if (p && *p) {
      /* Find first HTTP(S) URL */
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;
        if (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://")) {
          /* Skip media URLs */
          if (!is_media_url(t)) {
            url_start = t;
            break;
          }
        }
      }
      
      if (url_start) {
        /* Create OG preview widget */
        self->og_preview = og_preview_widget_new();
        gtk_box_append(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
        gtk_widget_set_visible(self->og_preview_container, TRUE);
        
        /* Set URL to fetch metadata - use parent's cancellable for lifecycle management */
        og_preview_widget_set_url_with_cancellable(self->og_preview, url_start, self->async_cancellable);
      }
      
      if (tokens) g_strfreev(tokens);
    }
  }
}

/* NIP-92 imeta-aware content setter */
void gnostr_note_card_row_set_content_with_imeta(GnostrNoteCardRow *self, const char *content, const char *tags_json) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_LABEL(self->content_label)) return;

  /* Store plain text content for clipboard operations */
  g_clear_pointer(&self->content_text, g_free);
  self->content_text = g_strdup(content);

  /* NIP-14: Extract and display subject tag if present */
  if (tags_json && *tags_json) {
    gchar *subject = extract_subject_from_tags_json(tags_json);
    if (subject && self->subject_label && GTK_IS_LABEL(self->subject_label)) {
      gchar *escaped = g_markup_escape_text(subject, -1);
      gtk_label_set_markup(GTK_LABEL(self->subject_label), escaped);
      gtk_widget_set_visible(self->subject_label, TRUE);
      g_free(escaped);
      g_debug("NIP-14: Displaying subject: %s", subject);
    } else if (self->subject_label && GTK_IS_WIDGET(self->subject_label)) {
      gtk_widget_set_visible(self->subject_label, FALSE);
    }
    g_free(subject);
  } else if (self->subject_label && GTK_IS_WIDGET(self->subject_label)) {
    gtk_widget_set_visible(self->subject_label, FALSE);
  }

  GnostrImetaList *imeta_list = NULL;
  GnostrEmojiList *emoji_list = NULL;
  if (tags_json && *tags_json) {
    imeta_list = gnostr_imeta_parse_tags_json(tags_json);
    if (imeta_list) {
      g_debug("note_card: Parsed %zu imeta tags from event", imeta_list->count);
    }
    /* NIP-30: Parse custom emoji tags */
    emoji_list = gnostr_emoji_parse_tags_json(tags_json);
    if (emoji_list) {
      g_debug("note_card: Parsed %zu custom emoji tags from event", emoji_list->count);
      /* Prefetch all emoji images to cache */
      for (size_t i = 0; i < emoji_list->count; i++) {
        GnostrCustomEmoji *emoji = emoji_list->items[i];
        if (emoji && emoji->url) {
          gnostr_emoji_cache_prefetch(emoji->url);
        }
      }
    }
  }

  GString *out = g_string_new("");
  if (content && *content) {
    gchar **tokens = g_strsplit_set(content, " \n\t", -1);
    for (guint i = 0; tokens && tokens[i]; i++) {
      const char *t = tokens[i];
      if (t[0] == '\0') { g_string_append(out, " "); continue; }
      gboolean is_url = token_is_url(t);
      gboolean is_nostr = token_is_nostr(t);
      gboolean is_hashtag = token_is_hashtag(t);
      if (is_nostr) {
        /* NIP-27: Handle nostr mentions with formatted display text */
        gchar *suffix = NULL;
        gchar *clean = extract_clean_url(t, &suffix);
        if (clean && *clean) {
          gchar *href = g_str_has_prefix(clean, "nostr:") ? g_strdup(clean) : g_strdup_printf("nostr:%s", clean);
          gchar *esc_href = g_markup_escape_text(href, -1);
          gchar *display = format_nostr_mention_display(clean);
          gchar *esc_display = g_markup_escape_text(display ? display : clean, -1);
          g_string_append_printf(out, "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);
          g_free(display);
          g_free(esc_href);
          g_free(esc_display);
          g_free(href);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(clean);
        g_free(suffix);
      } else if (is_url) {
        gchar *suffix = NULL;
        gchar *clean = extract_clean_url(t, &suffix);
        if (clean && *clean) {
          gchar *href = g_str_has_prefix(clean, "www.") ? g_strdup_printf("https://%s", clean) : g_strdup(clean);
          gchar *esc_href = g_markup_escape_text(href, -1);
          /* Shorten display URL if longer than 40 chars to fit 640px width */
          gchar *display_url = NULL;
          gsize clean_len = strlen(clean);
          if (clean_len > 40) {
            display_url = g_strdup_printf("%.35s...", clean);
          } else {
            display_url = g_strdup(clean);
          }
          gchar *esc_display = g_markup_escape_text(display_url, -1);
          g_string_append_printf(out, "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);
          g_free(esc_href);
          g_free(esc_display);
          g_free(display_url);
          g_free(href);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc);
          g_free(esc);
        }
        g_free(clean);
        g_free(suffix);
      } else if (is_hashtag) {
        gchar *suffix = NULL;
        gchar *tag = extract_hashtag(t, &suffix);
        if (tag && *tag) {
          gchar *esc_tag = g_markup_escape_text(tag, -1);
          g_string_append_printf(out, "<a href=\"hashtag:%s\">#%s</a>", esc_tag, esc_tag);
          g_free(esc_tag);
          if (suffix && *suffix) {
            gchar *esc_suffix = g_markup_escape_text(suffix, -1);
            g_string_append(out, esc_suffix);
            g_free(esc_suffix);
          }
        } else {
          gchar *esc = g_markup_escape_text(t, -1);
          g_string_append(out, esc); g_free(esc);
        }
        g_free(tag);
        g_free(suffix);
      } else {
        gchar *esc = g_markup_escape_text(t, -1);
        g_string_append(out, esc); g_free(esc);
      }
      g_string_append_c(out, ' ');
    }
    g_strfreev(tokens);
  }
  gchar *markup = out->len ? g_string_free(out, FALSE) : g_string_free(out, TRUE);
  gboolean markup_allocated = (markup != NULL);
  if (!markup) markup = escape_markup(content);
  gtk_label_set_use_markup(GTK_LABEL(self->content_label), TRUE);
  gtk_label_set_markup(GTK_LABEL(self->content_label), markup ? markup : "");
  if (markup_allocated || markup) g_free(markup);

  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    GtkWidget *child = gtk_widget_get_first_child(self->media_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_box_remove(GTK_BOX(self->media_box), child);
      child = next;
    }
    gtk_widget_set_visible(self->media_box, FALSE);

    if (content) {
      gchar **tokens = g_strsplit_set(content, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *url = tokens[i];
        if (!url || !*url) continue;
        if (g_str_has_prefix(url, "http://") || g_str_has_prefix(url, "https://")) {
          GnostrImeta *imeta = imeta_list ? gnostr_imeta_find_by_url(imeta_list, url) : NULL;
          GnostrMediaType media_type = GNOSTR_MEDIA_TYPE_UNKNOWN;
          if (imeta) {
            media_type = imeta->media_type;
            g_debug("note_card: imeta for %s: type=%d dim=%dx%d alt=%s",
                    url, media_type, imeta->width, imeta->height,
                    imeta->alt ? imeta->alt : "(none)");
          }
          if (media_type == GNOSTR_MEDIA_TYPE_UNKNOWN) {
            if (is_image_url(url)) media_type = GNOSTR_MEDIA_TYPE_IMAGE;
            else if (is_video_url(url)) media_type = GNOSTR_MEDIA_TYPE_VIDEO;
          }

          if (media_type == GNOSTR_MEDIA_TYPE_IMAGE) {
            /* Calculate height from imeta dimensions if available */
            int height = 300;
            if (imeta && imeta->width > 0 && imeta->height > 0) {
              int cw = 400;
              height = imeta->width <= cw ? imeta->height : (int)((double)imeta->height * cw / imeta->width);
              if (height > 400) height = 400;
              if (height < 100) height = 100;
            }

            /* Create image container with spinner and error fallback */
            const char *alt_text = (imeta && imeta->alt && *imeta->alt) ? imeta->alt : NULL;
            GtkWidget *container = create_image_container(url, height, alt_text);
            GtkWidget *pic = g_object_get_data(G_OBJECT(container), "media-picture");

            /* Add click gesture to the picture to open image viewer */
            GtkGesture *click_gesture = gtk_gesture_click_new();
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
            g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_media_image_clicked), NULL);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(click_gesture));

            gtk_box_append(GTK_BOX(self->media_box), container);
            gtk_widget_set_visible(self->media_box, TRUE);
#ifdef HAVE_SOUP3
            load_media_image(self, url, GTK_PICTURE(pic));
#endif
          } else if (media_type == GNOSTR_MEDIA_TYPE_VIDEO) {
            /* Use enhanced video player with controls overlay */
            GnostrVideoPlayer *player = gnostr_video_player_new();
            gtk_widget_add_css_class(GTK_WIDGET(player), "note-media-video");
            /* Constrain video width to card width (640px) less margins (16px each side) */
            int max_width = 608;
            int height = 300;
            if (imeta && imeta->width > 0 && imeta->height > 0) {
              int cw = max_width;
              height = imeta->width <= cw ? imeta->height : (int)((double)imeta->height * cw / imeta->width);
              if (height > 400) height = 400;
              if (height < 100) height = 100;
            }
            gtk_widget_set_size_request(GTK_WIDGET(player), max_width, height);
            if (imeta && imeta->alt && *imeta->alt) gtk_widget_set_tooltip_text(GTK_WIDGET(player), imeta->alt);
            gtk_widget_set_hexpand(GTK_WIDGET(player), FALSE);
            gtk_widget_set_vexpand(GTK_WIDGET(player), FALSE);
            /* Set video URI - settings (autoplay/loop) are read from GSettings */
            gnostr_video_player_set_uri(player, url);
            gtk_box_append(GTK_BOX(self->media_box), GTK_WIDGET(player));
            gtk_widget_set_visible(self->media_box, TRUE);
          }
        }
      }
      g_strfreev(tokens);
    }
  }

  gnostr_imeta_list_free(imeta_list);

  /* NIP-30: Display custom emoji images in emoji_box */
  if (emoji_list && emoji_list->count > 0 && content) {
    /* Create emoji_box if it doesn't exist (dynamically created since not in template) */
    if (!self->emoji_box) {
      self->emoji_box = gtk_flow_box_new();
      gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(self->emoji_box), FALSE);
      gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->emoji_box), GTK_SELECTION_NONE);
      gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->emoji_box), 1);
      gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->emoji_box), 20);
      gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->emoji_box), 4);
      gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->emoji_box), 4);
      gtk_widget_set_halign(self->emoji_box, GTK_ALIGN_START);
      gtk_widget_add_css_class(self->emoji_box, "custom-emoji-box");

      /* Insert emoji_box after content_label if possible */
      if (self->content_label && GTK_IS_WIDGET(self->content_label)) {
        GtkWidget *parent = gtk_widget_get_parent(self->content_label);
        if (parent && GTK_IS_BOX(parent)) {
          /* Find position of content_label and insert after it */
          GtkWidget *child = gtk_widget_get_first_child(parent);
          int pos = 0;
          while (child) {
            if (child == self->content_label) {
              /* Insert after content_label */
              gtk_box_insert_child_after(GTK_BOX(parent), self->emoji_box, self->content_label);
              break;
            }
            child = gtk_widget_get_next_sibling(child);
            pos++;
          }
        }
      }
    }

    /* Clear existing emoji widgets */
    if (self->emoji_box && GTK_IS_FLOW_BOX(self->emoji_box)) {
      GtkWidget *child = gtk_widget_get_first_child(self->emoji_box);
      while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_flow_box_remove(GTK_FLOW_BOX(self->emoji_box), child);
        child = next;
      }
      gtk_widget_set_visible(self->emoji_box, FALSE);

      /* Check which emojis are actually used in content and display them */
      GHashTable *used_emojis = g_hash_table_new(g_str_hash, g_str_equal);

      /* Find :shortcode: patterns in content */
      const char *p = content;
      while (*p) {
        if (*p == ':') {
          const char *start = p + 1;
          const char *end = start;
          /* Find closing colon */
          while (*end && *end != ':' && *end != ' ' && *end != '\n' && *end != '\t') {
            char c = *end;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) {
              break;
            }
            end++;
          }
          if (*end == ':' && end > start) {
            gchar *shortcode = g_strndup(start, end - start);
            GnostrCustomEmoji *emoji = gnostr_emoji_find_by_shortcode(emoji_list, shortcode);
            if (emoji && !g_hash_table_contains(used_emojis, shortcode)) {
              g_hash_table_add(used_emojis, shortcode);

              /* Create container box for emoji + label */
              GtkWidget *emoji_item = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
              gtk_widget_add_css_class(emoji_item, "custom-emoji-item");

              /* Create picture for the emoji */
              GtkWidget *picture = gtk_picture_new();
              gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
              gtk_widget_set_size_request(picture, 24, 24);
              gtk_widget_add_css_class(picture, "custom-emoji");

              /* Try to load from cache */
              GdkTexture *cached = gnostr_emoji_try_load_cached(emoji->url);
              if (cached) {
                gtk_picture_set_paintable(GTK_PICTURE(picture), GDK_PAINTABLE(cached));
                g_object_unref(cached);
              }
              /* Note: If not cached, the prefetch above will load it async
               * Future improvement: add callback to update picture when loaded */

              gtk_box_append(GTK_BOX(emoji_item), picture);

              /* Add shortcode label */
              gchar *label_text = g_strdup_printf(":%s:", shortcode);
              GtkWidget *label = gtk_label_new(label_text);
              gtk_widget_add_css_class(label, "custom-emoji-label");
              gtk_label_set_xalign(GTK_LABEL(label), 0);
              gtk_box_append(GTK_BOX(emoji_item), label);
              g_free(label_text);

              /* Set tooltip with full URL */
              gtk_widget_set_tooltip_text(emoji_item, emoji->url);

              gtk_flow_box_append(GTK_FLOW_BOX(self->emoji_box), emoji_item);
              gtk_widget_set_visible(self->emoji_box, TRUE);
            } else {
              g_free(shortcode);
            }
            p = end + 1;
            continue;
          }
        }
        p++;
      }

      g_hash_table_unref(used_emojis);
    }
  } else if (self->emoji_box && GTK_IS_WIDGET(self->emoji_box)) {
    /* No custom emojis - hide the box */
    gtk_widget_set_visible(self->emoji_box, FALSE);
  }

  gnostr_emoji_list_free(emoji_list);

  /* Detect NIP-19/21 nostr: references and create embedded note widgets */
  if (self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    /* Clear existing embeds from the embed_box */
    if (GTK_IS_FRAME(self->embed_box)) {
      gtk_frame_set_child(GTK_FRAME(self->embed_box), NULL);
    }
    gtk_widget_set_visible(self->embed_box, FALSE);
    self->note_embed = NULL;

    const char *p = content;
    if (p && *p) {
      /* Scan for nostr: URIs and bech32 references */
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      const char *first_nostr_ref = NULL;

      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;

        /* Check for nostr: URI or bare bech32 (NIP-21) */
        if (g_str_has_prefix(t, "nostr:") ||
            g_str_has_prefix(t, "note1") ||
            g_str_has_prefix(t, "nevent1") ||
            g_str_has_prefix(t, "naddr1") ||
            g_str_has_prefix(t, "npub1") ||
            g_str_has_prefix(t, "nprofile1")) {
          first_nostr_ref = t;
          break;
        }
      }

      if (first_nostr_ref) {
        /* Create the NIP-21 embed widget */
        self->note_embed = gnostr_note_embed_new();
        
        /* Use parent's cancellable for lifecycle management */
        gnostr_note_embed_set_cancellable(self->note_embed, self->async_cancellable);

        /* Connect profile-clicked signal to relay to main window */
        g_signal_connect(self->note_embed, "profile-clicked",
                        G_CALLBACK(on_embed_profile_clicked), self);

        /* Set the nostr URI - this triggers async loading via NIP-19 decoding */
        gnostr_note_embed_set_nostr_uri(self->note_embed, first_nostr_ref);

        /* Add embed widget to the embed_box frame */
        if (GTK_IS_FRAME(self->embed_box)) {
          gtk_frame_set_child(GTK_FRAME(self->embed_box), GTK_WIDGET(self->note_embed));
        }
        gtk_widget_set_visible(self->embed_box, TRUE);

        /* Also emit the signal for timeline-level handling (backwards compatibility) */
        g_signal_emit(self, signals[SIGNAL_REQUEST_EMBED], 0, first_nostr_ref);
      }

      if (tokens) g_strfreev(tokens);
    }
  }

  if (self->og_preview_container && GTK_IS_BOX(self->og_preview_container)) {
    if (self->og_preview) {
      gtk_box_remove(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
      self->og_preview = NULL;
    }
    gtk_widget_set_visible(self->og_preview_container, FALSE);
    const char *p = content;
    const char *url_start = NULL;
    if (p && *p) {
      gchar **tokens = g_strsplit_set(p, " \n\t", -1);
      for (guint i = 0; tokens && tokens[i]; i++) {
        const char *t = tokens[i];
        if (!t || !*t) continue;
        if (g_str_has_prefix(t, "http://") || g_str_has_prefix(t, "https://")) {
          if (!is_media_url(t)) { url_start = t; break; }
        }
      }
      if (url_start) {
        self->og_preview = og_preview_widget_new();
        gtk_box_append(GTK_BOX(self->og_preview_container), GTK_WIDGET(self->og_preview));
        gtk_widget_set_visible(self->og_preview_container, TRUE);
        /* Use parent's cancellable for lifecycle management */
        og_preview_widget_set_url_with_cancellable(self->og_preview, url_start, self->async_cancellable);
      }
      if (tokens) g_strfreev(tokens);
    }
  }
}

void gnostr_note_card_row_set_depth(GnostrNoteCardRow *self, guint depth) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  self->depth = depth;
  gtk_widget_set_margin_start(GTK_WIDGET(self), depth * 16);

  /* Apply CSS class for depth styling using GTK4 API */
  GtkWidget *widget = GTK_WIDGET(self);

  /* Remove existing depth classes */
  for (guint i = 1; i <= 4; i++) {
    gchar *class_name = g_strdup_printf("thread-depth-%u", i);
    gtk_widget_remove_css_class(widget, class_name);
    g_free(class_name);
  }
  
  /* Add appropriate depth class */
  if (depth > 0 && depth <= 4) {
    gchar *class_name = g_strdup_printf("thread-depth-%u", depth);
    gtk_widget_add_css_class(widget, class_name);
    g_free(class_name);
  }
  
  /* Add thread-reply class for any depth > 0 */
  if (depth > 0) {
    gtk_widget_add_css_class(widget, "thread-reply");
  } else {
    gtk_widget_remove_css_class(widget, "thread-reply");
  }
}

void gnostr_note_card_row_set_ids(GnostrNoteCardRow *self, const char *id_hex, const char *root_id, const char *pubkey_hex) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  g_free(self->id_hex); self->id_hex = g_strdup(id_hex);
  g_free(self->root_id); self->root_id = g_strdup(root_id);
  g_free(self->pubkey_hex); self->pubkey_hex = g_strdup(pubkey_hex);
}

void gnostr_note_card_row_set_thread_info(GnostrNoteCardRow *self,
                                           const char *root_id,
                                           const char *parent_id,
                                           const char *parent_author_name,
                                           gboolean is_reply) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  g_free(self->root_id);
  self->root_id = g_strdup(root_id);
  g_free(self->parent_id);
  self->parent_id = g_strdup(parent_id);
  self->is_reply = is_reply;

  /* Update reply indicator visibility and text */
  if (GTK_IS_WIDGET(self->reply_indicator_box)) {
    gtk_widget_set_visible(self->reply_indicator_box, is_reply);
  }

  if (is_reply && GTK_IS_LABEL(self->reply_indicator_label)) {
    char *indicator_text = NULL;
    if (parent_author_name && *parent_author_name) {
      indicator_text = g_strdup_printf("In reply to %s", parent_author_name);
    } else {
      indicator_text = g_strdup("In reply to...");
    }
    gtk_label_set_text(GTK_LABEL(self->reply_indicator_label), indicator_text);
    g_free(indicator_text);
  }

  /* Show/hide view thread button - visible if this is a reply or has a root */
  if (GTK_IS_BUTTON(self->btn_thread)) {
    gboolean show_thread_btn = (is_reply || (root_id != NULL && *root_id));
    gtk_widget_set_visible(GTK_WIDGET(self->btn_thread), show_thread_btn);
  }
}

/* Public helper to set the embed mini-card content */
void gnostr_note_card_row_set_embed(GnostrNoteCardRow *self, const char *title, const char *snippet) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_FRAME(self->embed_box)) return;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *lbl_title = gtk_label_new(title ? title : "");
  GtkWidget *lbl_snip = gtk_label_new(snippet ? snippet : "");
  gtk_widget_add_css_class(lbl_title, "note-author");
  gtk_widget_add_css_class(lbl_snip, "note-content");
  gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
  gtk_label_set_xalign(GTK_LABEL(lbl_snip), 0.0);
  gtk_box_append(GTK_BOX(box), lbl_title);
  gtk_box_append(GTK_BOX(box), lbl_snip);
  gtk_frame_set_child(GTK_FRAME(self->embed_box), box);
  gtk_widget_set_visible(self->embed_box, TRUE);
}

/* Rich embed variant: adds meta line between title and snippet */
void gnostr_note_card_row_set_embed_rich(GnostrNoteCardRow *self, const char *title, const char *meta, const char *snippet) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !GTK_IS_FRAME(self->embed_box)) return;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *lbl_title = gtk_label_new(title ? title : "");
  GtkWidget *lbl_meta  = gtk_label_new(meta ? meta : "");
  GtkWidget *lbl_snip  = gtk_label_new(snippet ? snippet : "");
  gtk_widget_add_css_class(lbl_title, "note-author");
  gtk_widget_add_css_class(lbl_meta,  "note-meta");
  gtk_widget_add_css_class(lbl_snip,  "note-content");
  gtk_label_set_xalign(GTK_LABEL(lbl_title), 0.0);
  gtk_label_set_xalign(GTK_LABEL(lbl_meta),  0.0);
  gtk_label_set_xalign(GTK_LABEL(lbl_snip),  0.0);
  gtk_box_append(GTK_BOX(box), lbl_title);
  gtk_box_append(GTK_BOX(box), lbl_meta);
  gtk_box_append(GTK_BOX(box), lbl_snip);
  gtk_frame_set_child(GTK_FRAME(self->embed_box), box);
  gtk_widget_set_visible(self->embed_box, TRUE);
}

/* NIP-05 verification callback for note card.
 * user_data is a weak ref pointer that will be NULL if widget was destroyed. */
static void on_note_nip05_verified(GnostrNip05Result *result, gpointer user_data) {
  GObject **weak_ref = (GObject **)user_data;
  GnostrNoteCardRow *self = NULL;

  /* Check weak reference - if NULL, widget was destroyed */
  if (weak_ref && *weak_ref) {
    self = GNOSTR_NOTE_CARD_ROW(*weak_ref);
  }

  /* Clean up weak ref container */
  g_free(weak_ref);

  if (!self || !result) {
    if (result) gnostr_nip05_result_free(result);
    return;
  }

  /* Double-check widget is still valid and not disposed */
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || self->disposed) {
    gnostr_nip05_result_free(result);
    return;
  }

  g_debug("note_card: NIP-05 verification result for %s: %s",
          result->identifier, gnostr_nip05_status_to_string(result->status));

  /* Show badge if verified */
  if (result->status == GNOSTR_NIP05_STATUS_VERIFIED && self->nip05_badge) {
    gtk_widget_set_visible(self->nip05_badge, TRUE);
    g_debug("note_card: showing NIP-05 verified badge for %s", result->identifier);
  }

  gnostr_nip05_result_free(result);
}

/* Set NIP-05 and trigger async verification */
void gnostr_note_card_row_set_nip05(GnostrNoteCardRow *self, const char *nip05, const char *pubkey_hex) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  /* Clear previous state */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
    g_clear_object(&self->nip05_cancellable);
  }
  g_clear_pointer(&self->nip05, g_free);

  /* Hide/remove previous badge */
  if (self->nip05_badge) {
    gtk_widget_set_visible(self->nip05_badge, FALSE);
  }

  /* Hide nip-05 label and separator if no nip-05 */
  if (GTK_IS_WIDGET(self->lbl_nip05)) {
    gtk_widget_set_visible(self->lbl_nip05, FALSE);
  }
  if (GTK_IS_WIDGET(self->lbl_nip05_separator)) {
    gtk_widget_set_visible(self->lbl_nip05_separator, FALSE);
  }

  if (!nip05 || !*nip05 || !pubkey_hex || strlen(pubkey_hex) != 64) {
    return;
  }

  /* Set nip-05 label in header (show after display name) */
  if (GTK_IS_LABEL(self->lbl_nip05)) {
    gtk_label_set_text(GTK_LABEL(self->lbl_nip05), nip05);
    gtk_widget_set_visible(self->lbl_nip05, TRUE);
    gtk_widget_set_tooltip_text(self->lbl_nip05, nip05);
  }
  if (GTK_IS_WIDGET(self->lbl_nip05_separator)) {
    gtk_widget_set_visible(self->lbl_nip05_separator, TRUE);
  }

  /* Store NIP-05 identifier */
  self->nip05 = g_strdup(nip05);

  /* Create badge widget if needed (add next to handle label) */
  if (!self->nip05_badge && GTK_IS_LABEL(self->lbl_handle)) {
    /* Get parent of handle label */
    GtkWidget *parent = gtk_widget_get_parent(self->lbl_handle);
    if (GTK_IS_BOX(parent)) {
      /* Create the badge */
      self->nip05_badge = gnostr_nip05_create_badge();
      gtk_widget_set_visible(self->nip05_badge, FALSE);

      /* Insert badge after handle label */
      GtkWidget *next_sibling = gtk_widget_get_next_sibling(self->lbl_handle);
      if (next_sibling) {
        gtk_box_insert_child_after(GTK_BOX(parent), self->nip05_badge, self->lbl_handle);
      } else {
        gtk_box_append(GTK_BOX(parent), self->nip05_badge);
      }
    }
  }

  /* Check cache first for immediate display */
  GnostrNip05Result *cached = gnostr_nip05_cache_get(nip05);
  if (cached) {
    if (cached->status == GNOSTR_NIP05_STATUS_VERIFIED &&
        cached->pubkey_hex &&
        g_ascii_strcasecmp(cached->pubkey_hex, pubkey_hex) == 0) {
      if (self->nip05_badge) {
        gtk_widget_set_visible(self->nip05_badge, TRUE);
      }
      g_debug("note_card: NIP-05 verified from cache for %s", nip05);
    }
    gnostr_nip05_result_free(cached);
    return;
  }

  /* Verify async - use weak reference to safely handle callback after widget destruction */
  self->nip05_cancellable = g_cancellable_new();
  
  /* Create weak reference container that will be set to NULL when widget is destroyed */
  GObject **weak_ref = g_new0(GObject *, 1);
  *weak_ref = G_OBJECT(self);
  g_object_add_weak_pointer(G_OBJECT(self), (gpointer *)weak_ref);
  
  gnostr_nip05_verify_async(nip05, pubkey_hex, on_note_nip05_verified, weak_ref, self->nip05_cancellable);
}

/* Set bookmark state and update button icon */
void gnostr_note_card_row_set_bookmarked(GnostrNoteCardRow *self, gboolean is_bookmarked) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_bookmarked = is_bookmarked;

  /* Update button icon */
  if (GTK_IS_BUTTON(self->btn_bookmark)) {
    gtk_button_set_icon_name(GTK_BUTTON(self->btn_bookmark),
      is_bookmarked ? "user-bookmarks-symbolic" : "bookmark-new-symbolic");
  }
}

/* Set like state and update button icon (NIP-25 reactions) */
void gnostr_note_card_row_set_liked(GnostrNoteCardRow *self, gboolean is_liked) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_liked = is_liked;

  /* Update button visual state */
  if (GTK_IS_BUTTON(self->btn_like)) {
    /* Use CSS class for visual differentiation - more reliable than icon switching.
     * CSS can style the "liked" class with different color (e.g., red/pink). */
    if (is_liked) {
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_like), "liked");
    } else {
      gtk_widget_remove_css_class(GTK_WIDGET(self->btn_like), "liked");
    }
  }
}

/* Set like count and update display (NIP-25 reactions) */
void gnostr_note_card_row_set_like_count(GnostrNoteCardRow *self, guint count) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->like_count = count;

  /* Update the like count label */
  if (GTK_IS_LABEL(self->lbl_like_count)) {
    if (count > 0) {
      gchar *text = g_strdup_printf("%u", count);
      gtk_label_set_text(GTK_LABEL(self->lbl_like_count), text);
      gtk_widget_set_visible(self->lbl_like_count, TRUE);
      g_free(text);
    } else {
      gtk_widget_set_visible(self->lbl_like_count, FALSE);
    }
  }
}

/* NIP-25: Set event kind for proper reaction k-tag */
void gnostr_note_card_row_set_event_kind(GnostrNoteCardRow *self, gint kind) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  self->event_kind = kind;
}

/* NIP-25: Set reaction breakdown with emoji counts for display
 * @breakdown: GHashTable of emoji (string) -> count (guint via GPOINTER_TO_UINT) */
void gnostr_note_card_row_set_reaction_breakdown(GnostrNoteCardRow *self, GHashTable *breakdown) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  /* Clear existing breakdown */
  if (self->reaction_breakdown) {
    g_hash_table_remove_all(self->reaction_breakdown);
  } else {
    self->reaction_breakdown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }

  if (!breakdown) return;

  /* Copy breakdown data */
  GHashTableIter iter;
  gpointer key, value;
  guint total = 0;
  g_hash_table_iter_init(&iter, breakdown);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    const char *emoji = (const char *)key;
    guint count = GPOINTER_TO_UINT(value);
    g_hash_table_insert(self->reaction_breakdown, g_strdup(emoji), GUINT_TO_POINTER(count));
    total += count;
  }

  /* Update total count display */
  gnostr_note_card_row_set_like_count(self, total);

  /* Update tooltip with breakdown summary */
  if (GTK_IS_BUTTON(self->btn_like)) {
    if (total > 0) {
      GString *tooltip = g_string_new("Reactions:\n");
      g_hash_table_iter_init(&iter, self->reaction_breakdown);
      while (g_hash_table_iter_next(&iter, &key, &value)) {
        const char *emoji = (const char *)key;
        guint count = GPOINTER_TO_UINT(value);
        g_string_append_printf(tooltip, "%s: %u\n", emoji, count);
      }
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_like), tooltip->str);
      g_string_free(tooltip, TRUE);
    } else {
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_like), "Like");
    }
  }
}

/* NIP-25: Add a single reaction to the breakdown */
void gnostr_note_card_row_add_reaction(GnostrNoteCardRow *self, const char *emoji, const char *reactor_pubkey) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || !emoji) return;

  /* Initialize breakdown table if needed */
  if (!self->reaction_breakdown) {
    self->reaction_breakdown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }

  /* Increment count for this emoji */
  gpointer existing = g_hash_table_lookup(self->reaction_breakdown, emoji);
  guint count = existing ? GPOINTER_TO_UINT(existing) + 1 : 1;
  g_hash_table_insert(self->reaction_breakdown, g_strdup(emoji), GUINT_TO_POINTER(count));

  /* Track reactor pubkey if provided */
  if (reactor_pubkey) {
    if (!self->reactors) {
      self->reactors = g_ptr_array_new_with_free_func(g_free);
    }
    g_ptr_array_add(self->reactors, g_strdup(reactor_pubkey));
  }

  /* Update total count */
  self->like_count++;
  gnostr_note_card_row_set_like_count(self, self->like_count);
}

/* Set author's lightning address for NIP-57 zaps */
void gnostr_note_card_row_set_author_lud16(GnostrNoteCardRow *self, const char *lud16) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  g_clear_pointer(&self->author_lud16, g_free);
  self->author_lud16 = g_strdup(lud16);

  /* Update zap button sensitivity based on whether lud16 is available */
  if (GTK_IS_BUTTON(self->btn_zap)) {
    gboolean can_zap = (lud16 != NULL && *lud16 != '\0');
    gtk_widget_set_sensitive(GTK_WIDGET(self->btn_zap), can_zap);
    if (!can_zap) {
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_zap), "User has no lightning address");
    } else {
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->btn_zap), "Zap");
    }
  }
}

/* Update zap statistics display */
void gnostr_note_card_row_set_zap_stats(GnostrNoteCardRow *self, guint zap_count, gint64 total_msat) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->zap_count = zap_count;
  self->zap_total_msat = total_msat;

  /* Update the zap count label */
  if (GTK_IS_LABEL(self->lbl_zap_count)) {
    if (zap_count > 0) {
      gchar *formatted = gnostr_zap_format_amount(total_msat);
      gtk_label_set_text(GTK_LABEL(self->lbl_zap_count), formatted);
      gtk_widget_set_visible(self->lbl_zap_count, TRUE);
      g_free(formatted);
    } else {
      gtk_widget_set_visible(self->lbl_zap_count, FALSE);
    }
  }
}

/* Set the reply count for thread root indicator */
void gnostr_note_card_row_set_reply_count(GnostrNoteCardRow *self, guint count) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->reply_count = count;
  self->is_thread_root = (count > 0);

  /* Update the reply count box visibility and label */
  if (GTK_IS_WIDGET(self->reply_count_box)) {
    gtk_widget_set_visible(self->reply_count_box, count > 0);
  }

  if (count > 0 && GTK_IS_LABEL(self->reply_count_label)) {
    gchar *text = NULL;
    if (count == 1) {
      text = g_strdup("1 reply");
    } else {
      text = g_strdup_printf("%u replies", count);
    }
    gtk_label_set_text(GTK_LABEL(self->reply_count_label), text);
    g_free(text);
  }

  /* Also show the thread button when there are replies */
  if (GTK_IS_BUTTON(self->btn_thread)) {
    gtk_widget_set_visible(GTK_WIDGET(self->btn_thread), count > 0);
  }
}

/* NIP-09: Set whether this is the current user's own note (enables delete option) */
void gnostr_note_card_row_set_is_own_note(GnostrNoteCardRow *self, gboolean is_own) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_own_note = is_own;

  /* Update delete button visibility if menu has been created */
  if (GTK_IS_BUTTON(self->delete_btn)) {
    gtk_widget_set_visible(self->delete_btn, is_own);
    /* Also update separator visibility */
    GtkWidget *sep = g_object_get_data(G_OBJECT(self->delete_btn), "delete-separator");
    if (GTK_IS_WIDGET(sep)) {
      gtk_widget_set_visible(sep, is_own);
    }
  }
}

/* Set login state: disables authentication-required buttons when logged out */
void gnostr_note_card_row_set_logged_in(GnostrNoteCardRow *self, gboolean logged_in) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_logged_in = logged_in;

  const char *logged_out_tooltip = "Sign in to use this feature";

  /* Reply button requires signing */
  if (GTK_IS_WIDGET(self->btn_reply)) {
    gtk_widget_set_sensitive(self->btn_reply, logged_in);
    gtk_widget_set_tooltip_text(self->btn_reply, logged_in ? "Reply" : logged_out_tooltip);
  }

  /* Repost button requires signing */
  if (GTK_IS_WIDGET(self->btn_repost)) {
    gtk_widget_set_sensitive(self->btn_repost, logged_in);
    gtk_widget_set_tooltip_text(self->btn_repost, logged_in ? "Repost" : logged_out_tooltip);
  }

  /* Like button requires signing */
  if (GTK_IS_WIDGET(self->btn_like)) {
    gtk_widget_set_sensitive(self->btn_like, logged_in);
    gtk_widget_set_tooltip_text(self->btn_like, logged_in ? "Like" : logged_out_tooltip);
  }

  /* Zap button requires signing */
  if (GTK_IS_WIDGET(self->btn_zap)) {
    gtk_widget_set_sensitive(self->btn_zap, logged_in);
    gtk_widget_set_tooltip_text(self->btn_zap, logged_in ? "Zap" : logged_out_tooltip);
  }

  /* Bookmark button requires signing (NIP-51 list management) */
  if (GTK_IS_WIDGET(self->btn_bookmark)) {
    gtk_widget_set_sensitive(self->btn_bookmark, logged_in);
    gtk_widget_set_tooltip_text(self->btn_bookmark, logged_in ? "Bookmark" : logged_out_tooltip);
  }
}

/* NIP-18: Set repost information to display "reposted by X" attribution */
void gnostr_note_card_row_set_repost_info(GnostrNoteCardRow *self,
                                           const char *reposter_pubkey_hex,
                                           const char *reposter_display_name,
                                           gint64 repost_created_at) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  g_clear_pointer(&self->reposter_pubkey, g_free);
  g_clear_pointer(&self->reposter_display_name, g_free);

  self->reposter_pubkey = g_strdup(reposter_pubkey_hex);
  self->reposter_display_name = g_strdup(reposter_display_name);
  self->repost_created_at = repost_created_at;

  /* Create repost indicator box if it doesn't exist */
  if (!self->repost_indicator_box && self->root && GTK_IS_WIDGET(self->root)) {
    /* Create "Reposted by X" indicator */
    self->repost_indicator_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(self->repost_indicator_box, "repost-indicator");
    gtk_widget_set_margin_start(self->repost_indicator_box, 52);  /* Align with content */
    gtk_widget_set_margin_bottom(self->repost_indicator_box, 4);

    /* Repost icon */
    GtkWidget *icon = gtk_image_new_from_icon_name("object-rotate-right-symbolic");
    gtk_widget_add_css_class(icon, "dim-label");
    gtk_box_append(GTK_BOX(self->repost_indicator_box), icon);

    /* "Reposted by" label */
    self->repost_indicator_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->repost_indicator_label, "dim-label");
    gtk_widget_add_css_class(self->repost_indicator_label, "caption");
    gtk_box_append(GTK_BOX(self->repost_indicator_box), self->repost_indicator_label);

    /* Insert at the top of the card - before the main content box */
    if (GTK_IS_BOX(self->root)) {
      gtk_box_prepend(GTK_BOX(self->root), self->repost_indicator_box);
    }
  }

  /* Update the label text */
  if (GTK_IS_LABEL(self->repost_indicator_label)) {
    const char *display = reposter_display_name && *reposter_display_name
                          ? reposter_display_name : "Someone";
    gchar *text = g_strdup_printf("Reposted by %s", display);
    gtk_label_set_text(GTK_LABEL(self->repost_indicator_label), text);
    g_free(text);
  }

  /* Show the indicator */
  if (GTK_IS_WIDGET(self->repost_indicator_box)) {
    gtk_widget_set_visible(self->repost_indicator_box, TRUE);
  }
}

/* NIP-18: Set whether this card represents a repost (kind 6/16) */
void gnostr_note_card_row_set_is_repost(GnostrNoteCardRow *self, gboolean is_repost) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_repost = is_repost;

  /* Add/remove repost CSS class for styling */
  if (is_repost) {
    gtk_widget_add_css_class(GTK_WIDGET(self), "repost");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self), "repost");
    /* Hide repost indicator if not a repost */
    if (GTK_IS_WIDGET(self->repost_indicator_box)) {
      gtk_widget_set_visible(self->repost_indicator_box, FALSE);
    }
  }
}

/* NIP-18: Update the repost count display */
void gnostr_note_card_row_set_repost_count(GnostrNoteCardRow *self, guint count) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->repost_count = count;

  /* Update the repost count label if it exists */
  if (GTK_IS_LABEL(self->lbl_repost_count)) {
    if (count > 0) {
      gchar *text = g_strdup_printf("%u", count);
      gtk_label_set_text(GTK_LABEL(self->lbl_repost_count), text);
      gtk_widget_set_visible(self->lbl_repost_count, TRUE);
      g_free(text);
    } else {
      gtk_widget_set_visible(self->lbl_repost_count, FALSE);
    }
  }
}

/* NIP-18 Quote Reposts: Set quote post info to display the quoted note inline */
void gnostr_note_card_row_set_quote_info(GnostrNoteCardRow *self,
                                          const char *quoted_event_id_hex,
                                          const char *quoted_content,
                                          const char *quoted_author_name) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  g_clear_pointer(&self->quoted_event_id, g_free);
  self->quoted_event_id = g_strdup(quoted_event_id_hex);

  /* Create quote embed box if it doesn't exist */
  if (!self->quote_embed_box && self->embed_box && GTK_IS_WIDGET(self->embed_box)) {
    self->quote_embed_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(self->quote_embed_box, "quote-embed");
    gtk_widget_add_css_class(self->quote_embed_box, "card");
    gtk_widget_set_margin_top(self->quote_embed_box, 8);
    gtk_widget_set_margin_bottom(self->quote_embed_box, 8);

    /* Container padding */
    gtk_widget_set_margin_start(self->quote_embed_box, 8);
    gtk_widget_set_margin_end(self->quote_embed_box, 8);

    /* Author label */
    GtkWidget *author_label = gtk_label_new(NULL);
    gtk_widget_add_css_class(author_label, "caption");
    gtk_widget_add_css_class(author_label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(author_label), 0);
    g_object_set_data(G_OBJECT(self->quote_embed_box), "author-label", author_label);
    gtk_box_append(GTK_BOX(self->quote_embed_box), author_label);

    /* Content label */
    GtkWidget *content_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(content_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(content_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(content_label), 0);
    gtk_label_set_max_width_chars(GTK_LABEL(content_label), 60);
    gtk_label_set_ellipsize(GTK_LABEL(content_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_lines(GTK_LABEL(content_label), 3);
    g_object_set_data(G_OBJECT(self->quote_embed_box), "content-label", content_label);
    gtk_box_append(GTK_BOX(self->quote_embed_box), content_label);

    /* Add to embed_box */
    gtk_box_append(GTK_BOX(self->embed_box), self->quote_embed_box);
  }

  /* Update content */
  if (GTK_IS_WIDGET(self->quote_embed_box)) {
    GtkWidget *author_label = g_object_get_data(G_OBJECT(self->quote_embed_box), "author-label");
    GtkWidget *content_label = g_object_get_data(G_OBJECT(self->quote_embed_box), "content-label");

    if (GTK_IS_LABEL(author_label)) {
      const char *author = quoted_author_name && *quoted_author_name
                           ? quoted_author_name : "Unknown";
      gchar *author_text = g_strdup_printf("Quoting %s", author);
      gtk_label_set_text(GTK_LABEL(author_label), author_text);
      g_free(author_text);
    }

    if (GTK_IS_LABEL(content_label)) {
      gtk_label_set_text(GTK_LABEL(content_label),
                         quoted_content && *quoted_content ? quoted_content : "(content unavailable)");
    }

    gtk_widget_set_visible(self->quote_embed_box, TRUE);
    gtk_widget_set_visible(self->embed_box, TRUE);
  }
}

/* NIP-36: Set content-warning for sensitive/NSFW content */
void gnostr_note_card_row_set_content_warning(GnostrNoteCardRow *self,
                                               const char *content_warning_reason) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  /* Store the content warning state and reason */
  g_clear_pointer(&self->content_warning_reason, g_free);
  self->is_sensitive = (content_warning_reason != NULL);
  self->content_warning_reason = g_strdup(content_warning_reason);
  self->sensitive_content_revealed = FALSE;

  /* Check GSettings for auto-show preference */
  gboolean auto_show = FALSE;
  GSettings *display_settings = g_settings_new("org.gnostr.Display");
  if (display_settings) {
    auto_show = g_settings_get_boolean(display_settings, "auto-show-sensitive");
    g_object_unref(display_settings);
  }

  if (self->is_sensitive && !auto_show) {
    /* Show the sensitive content overlay and blur the content */
    if (GTK_IS_WIDGET(self->sensitive_content_overlay)) {
      gtk_widget_set_visible(self->sensitive_content_overlay, TRUE);
    }

    /* Update the warning label with the reason if provided */
    if (GTK_IS_LABEL(self->sensitive_warning_label)) {
      if (content_warning_reason && *content_warning_reason) {
        gchar *label_text = g_strdup_printf("Sensitive Content: %s", content_warning_reason);
        gtk_label_set_text(GTK_LABEL(self->sensitive_warning_label), label_text);
        g_free(label_text);
      } else {
        gtk_label_set_text(GTK_LABEL(self->sensitive_warning_label), "Sensitive Content");
      }
    }

    /* Add blur CSS class to content elements */
    if (GTK_IS_WIDGET(self->content_label)) {
      gtk_widget_add_css_class(self->content_label, "content-blurred");
    }
    if (GTK_IS_WIDGET(self->media_box)) {
      gtk_widget_add_css_class(self->media_box, "content-blurred");
    }
    if (GTK_IS_WIDGET(self->embed_box)) {
      gtk_widget_add_css_class(self->embed_box, "content-blurred");
    }
    if (GTK_IS_WIDGET(self->og_preview_container)) {
      gtk_widget_add_css_class(self->og_preview_container, "content-blurred");
    }

    /* Add CSS class to the whole note card for styling */
    gtk_widget_add_css_class(GTK_WIDGET(self), "sensitive-content");
  } else {
    /* Hide the overlay and remove blur classes */
    if (GTK_IS_WIDGET(self->sensitive_content_overlay)) {
      gtk_widget_set_visible(self->sensitive_content_overlay, FALSE);
    }
    if (GTK_IS_WIDGET(self->content_label)) {
      gtk_widget_remove_css_class(self->content_label, "content-blurred");
    }
    if (GTK_IS_WIDGET(self->media_box)) {
      gtk_widget_remove_css_class(self->media_box, "content-blurred");
    }
    if (GTK_IS_WIDGET(self->embed_box)) {
      gtk_widget_remove_css_class(self->embed_box, "content-blurred");
    }
    if (GTK_IS_WIDGET(self->og_preview_container)) {
      gtk_widget_remove_css_class(self->og_preview_container, "content-blurred");
    }
    gtk_widget_remove_css_class(GTK_WIDGET(self), "sensitive-content");
  }
}

/* NIP-36: Check if content is currently blurred (sensitive content hidden) */
gboolean gnostr_note_card_row_is_content_blurred(GnostrNoteCardRow *self) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return FALSE;
  return self->is_sensitive && !self->sensitive_content_revealed;
}

/* NIP-36: Reveal sensitive content (show hidden content) */
void gnostr_note_card_row_reveal_sensitive_content(GnostrNoteCardRow *self) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  /* Mark as revealed and trigger the same logic as clicking the button */
  self->sensitive_content_revealed = TRUE;

  /* Hide the overlay */
  if (GTK_IS_WIDGET(self->sensitive_content_overlay)) {
    gtk_widget_set_visible(self->sensitive_content_overlay, FALSE);
  }

  /* Remove blur CSS class from content */
  if (GTK_IS_WIDGET(self->content_label)) {
    gtk_widget_remove_css_class(self->content_label, "content-blurred");
  }
  if (GTK_IS_WIDGET(self->media_box)) {
    gtk_widget_remove_css_class(self->media_box, "content-blurred");
  }
  if (GTK_IS_WIDGET(self->embed_box)) {
    gtk_widget_remove_css_class(self->embed_box, "content-blurred");
  }
  if (GTK_IS_WIDGET(self->og_preview_container)) {
    gtk_widget_remove_css_class(self->og_preview_container, "content-blurred");
  }
}

/* ===== NIP-32 Label Functions ===== */

/* Helper: Create a label chip widget */
static GtkWidget *create_label_chip(const char *namespace, const char *label) {
  if (!label || !*label) return NULL;

  GtkWidget *chip = gtk_label_new(label);
  gtk_widget_add_css_class(chip, "note-label-chip");

  /* Add namespace-specific styling */
  if (namespace) {
    if (g_str_equal(namespace, NIP32_NS_UGC)) {
      gtk_widget_add_css_class(chip, "ugc");
    } else if (g_str_equal(namespace, "topic")) {
      gtk_widget_add_css_class(chip, "topic");
    } else if (g_str_equal(namespace, NIP32_NS_QUALITY)) {
      gtk_widget_add_css_class(chip, "quality");
    } else if (g_str_equal(namespace, NIP32_NS_REVIEW)) {
      gtk_widget_add_css_class(chip, "review");
    }
  }

  /* Set tooltip with full namespace:label */
  if (namespace && *namespace) {
    char *tooltip = g_strdup_printf("%s:%s", namespace, label);
    gtk_widget_set_tooltip_text(chip, tooltip);
    g_free(tooltip);
  }

  return chip;
}

/* NIP-32: Set labels to display on this note */
void gnostr_note_card_row_set_labels(GnostrNoteCardRow *self, GPtrArray *labels) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!GTK_IS_FLOW_BOX(self->labels_box)) return;

  /* Clear existing labels */
  gnostr_note_card_row_clear_labels(self);

  if (!labels || labels->len == 0) {
    gtk_widget_set_visible(self->labels_box, FALSE);
    return;
  }

  /* Add each label as a chip */
  for (guint i = 0; i < labels->len; i++) {
    GnostrLabel *l = g_ptr_array_index(labels, i);
    if (!l || !l->label) continue;

    char *display_label = gnostr_nip32_format_label(l);
    if (!display_label) continue;

    GtkWidget *chip = create_label_chip(l->namespace, display_label);
    g_free(display_label);

    if (chip) {
      gtk_flow_box_append(GTK_FLOW_BOX(self->labels_box), chip);
    }
  }

  /* Show the labels container if we have labels */
  gtk_widget_set_visible(self->labels_box, TRUE);
}

/* NIP-32: Add a single label to this note's display */
void gnostr_note_card_row_add_label(GnostrNoteCardRow *self, const char *namespace, const char *label) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!GTK_IS_FLOW_BOX(self->labels_box)) return;
  if (!label || !*label) return;

  GtkWidget *chip = create_label_chip(namespace, label);
  if (chip) {
    gtk_flow_box_append(GTK_FLOW_BOX(self->labels_box), chip);
    gtk_widget_set_visible(self->labels_box, TRUE);
  }
}

/* NIP-32: Clear all displayed labels */
void gnostr_note_card_row_clear_labels(GnostrNoteCardRow *self) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!GTK_IS_FLOW_BOX(self->labels_box)) return;

  /* Remove all children from the flow box */
  GtkWidget *child = gtk_widget_get_first_child(self->labels_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->labels_box), child);
    child = next;
  }

  gtk_widget_set_visible(self->labels_box, FALSE);
}

/* ============================================
   Hashtag "t" Tags Implementation
   ============================================ */

/* Callback for hashtag chip clicks */
static void on_hashtag_chip_clicked(GtkButton *btn, gpointer user_data) {
  const char *tag = g_object_get_data(G_OBJECT(btn), "hashtag");
  if (tag && GNOSTR_IS_NOTE_CARD_ROW(user_data)) {
    g_signal_emit(user_data, signals[SIGNAL_SEARCH_HASHTAG], 0, tag);
  }
}

/* Create a hashtag chip button */
static GtkWidget *create_hashtag_chip(const char *hashtag) {
  if (!hashtag || !*hashtag) return NULL;

  GtkWidget *btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
  gtk_widget_add_css_class(btn, "pill");
  gtk_widget_add_css_class(btn, "note-hashtag");

  gchar *label_text = g_strdup_printf("#%s", hashtag);
  gtk_button_set_label(GTK_BUTTON(btn), label_text);
  g_free(label_text);

  /* GTK4: Ensure widget is visible */
  gtk_widget_set_visible(btn, TRUE);

  return btn;
}

/* Set hashtags from "t" tags to display on this note */
void gnostr_note_card_row_set_hashtags(GnostrNoteCardRow *self, const char * const *hashtags) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!GTK_IS_FLOW_BOX(self->hashtags_box)) return;

  /* Clear existing hashtags */
  GtkWidget *child = gtk_widget_get_first_child(self->hashtags_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->hashtags_box), child);
    child = next;
  }

  if (!hashtags || !hashtags[0]) {
    gtk_widget_set_visible(self->hashtags_box, FALSE);
    return;
  }

  /* Add each hashtag as a chip */
  for (int i = 0; hashtags[i]; i++) {
    const char *tag = hashtags[i];
    if (!tag || !*tag) continue;

    GtkWidget *chip = create_hashtag_chip(tag);
    if (chip) {
      g_object_set_data_full(G_OBJECT(chip), "hashtag", g_strdup(tag), g_free);
      g_signal_connect(chip, "clicked", G_CALLBACK(on_hashtag_chip_clicked), self);
      gtk_flow_box_append(GTK_FLOW_BOX(self->hashtags_box), chip);
    }
  }

  /* Show the hashtags container if we have hashtags */
  gtk_widget_set_visible(self->hashtags_box, TRUE);
}

/* ============================================
   NIP-23 Long-form Content Implementation
   ============================================ */

/* Helper: compute reading time from content */
static gchar *compute_article_reading_time(const char *content) {
  if (!content || !*content) return NULL;

  int word_count = 0;
  gboolean in_word = FALSE;

  for (const char *p = content; *p; p++) {
    if (g_ascii_isspace(*p)) {
      in_word = FALSE;
    } else if (!in_word) {
      in_word = TRUE;
      word_count++;
    }
  }

  int minutes = (word_count + 199) / 200; /* 200 WPM average */
  if (minutes < 1) minutes = 1;

  return g_strdup_printf(_("%d min read"), minutes);
}

/* Helper: format publication date for articles */
static gchar *format_article_date(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup(_("Unknown date"));

  GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
  if (!dt) return g_strdup(_("Unknown date"));

  gchar *result = g_date_time_format(dt, "%B %d, %Y");
  g_date_time_unref(dt);
  return result;
}

#ifdef HAVE_SOUP3
/* Callback for article header image loading */
static void on_article_image_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);

  if (!GNOSTR_IS_NOTE_CARD_ROW(self) || self->disposed) return;

  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), res, &error);

  if (!bytes || error) {
    if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("NIP-23: Failed to load article image: %s", error->message);
    }
    if (error) g_error_free(error);
    return;
  }

  /* Re-check disposed before accessing widget members */
  if (self->disposed) {
    g_bytes_unref(bytes);
    return;
  }

  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, &error);
  g_bytes_unref(bytes);

  if (!texture || error) {
    if (error) {
      g_debug("NIP-23: Failed to create texture: %s", error->message);
      g_error_free(error);
    }
    return;
  }

  if (!self->disposed && GTK_IS_PICTURE(self->article_image)) {
    gtk_picture_set_paintable(GTK_PICTURE(self->article_image), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(self->article_image_box, TRUE);
  }

  g_object_unref(texture);
}

/* Load article header image asynchronously */
static void load_article_header_image(GnostrNoteCardRow *self, const char *url) {
  if (!url || !*url) return;

  /* Cancel any previous image fetch */
  if (self->article_image_cancellable) {
    g_cancellable_cancel(self->article_image_cancellable);
    g_clear_object(&self->article_image_cancellable);
  }

  self->article_image_cancellable = g_cancellable_new();

  SoupMessage *msg = soup_message_new("GET", url);
  if (!msg) return;

  soup_session_send_and_read_async(
    gnostr_get_shared_soup_session(),
    msg,
    G_PRIORITY_LOW,
    self->article_image_cancellable,
    on_article_image_loaded,
    self
  );

  g_object_unref(msg);
}
#endif

/* Create article hashtag chip widget */
static GtkWidget *create_article_hashtag_chip(const char *hashtag) {
  if (!hashtag || !*hashtag) return NULL;

  GtkWidget *btn = gtk_button_new();
  gtk_button_set_has_frame(GTK_BUTTON(btn), FALSE);
  gtk_widget_add_css_class(btn, "article-hashtag");

  gchar *label_text = g_strdup_printf("#%s", hashtag);
  gtk_button_set_label(GTK_BUTTON(btn), label_text);
  g_free(label_text);

  return btn;
}

/* Callback for article hashtag chip clicks */
static void on_article_hashtag_clicked(GtkButton *btn, gpointer user_data) {
  const char *tag = g_object_get_data(G_OBJECT(btn), "hashtag");
  if (tag && GNOSTR_IS_NOTE_CARD_ROW(user_data)) {
    g_signal_emit(user_data, signals[SIGNAL_SEARCH_HASHTAG], 0, tag);
  }
}

/* NIP-23: Set article mode for this note card */
void gnostr_note_card_row_set_article_mode(GnostrNoteCardRow *self,
                                            const char *title,
                                            const char *summary,
                                            const char *image_url,
                                            gint64 published_at,
                                            const char *d_tag,
                                            const char * const *hashtags) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  self->is_article = TRUE;

  /* Store article metadata */
  g_clear_pointer(&self->article_d_tag, g_free);
  g_clear_pointer(&self->article_title, g_free);
  g_clear_pointer(&self->article_image_url, g_free);

  self->article_d_tag = g_strdup(d_tag);
  self->article_title = g_strdup(title);
  self->article_image_url = g_strdup(image_url);
  self->article_published_at = published_at;

  /* Add article CSS class to root */
  if (GTK_IS_WIDGET(self->root)) {
    gtk_widget_add_css_class(self->root, "article-card");
  }

  /* Create article title label if not exists */
  if (!self->article_title_label) {
    self->article_title_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(self->article_title_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->article_title_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(self->article_title_label), 0.0);
    gtk_label_set_lines(GTK_LABEL(self->article_title_label), 3);
    gtk_label_set_ellipsize(GTK_LABEL(self->article_title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(self->article_title_label, "article-title");

    /* Insert title label before content label */
    if (GTK_IS_WIDGET(self->content_label)) {
      GtkWidget *parent = gtk_widget_get_parent(self->content_label);
      if (GTK_IS_BOX(parent)) {
        /* Insert at position before content label */
        GtkWidget *sibling = gtk_widget_get_prev_sibling(self->content_label);
        if (sibling) {
          gtk_box_insert_child_after(GTK_BOX(parent), self->article_title_label, sibling);
        } else {
          gtk_box_prepend(GTK_BOX(parent), self->article_title_label);
        }
      }
    }
  }

  /* Set title text */
  if (GTK_IS_LABEL(self->article_title_label)) {
    gtk_label_set_text(GTK_LABEL(self->article_title_label),
                       (title && *title) ? title : _("Untitled Article"));
    gtk_widget_set_visible(self->article_title_label, TRUE);
  }

  /* Create header image container if not exists */
  if (!self->article_image_box && image_url && *image_url) {
    self->article_image_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(self->article_image_box, "article-header-image");
    gtk_widget_set_visible(self->article_image_box, FALSE);

    self->article_image = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->article_image), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_size_request(self->article_image, -1, 180);
    gtk_widget_add_css_class(self->article_image, "article-header-image");
    gtk_box_append(GTK_BOX(self->article_image_box), self->article_image);

    /* Insert image at the top of the content area */
    if (GTK_IS_WIDGET(self->content_label)) {
      GtkWidget *parent = gtk_widget_get_parent(self->content_label);
      if (GTK_IS_BOX(parent)) {
        gtk_box_prepend(GTK_BOX(parent), self->article_image_box);
      }
    }

#ifdef HAVE_SOUP3
    load_article_header_image(self, image_url);
#endif
  }

  /* Set summary as content (with markdown conversion) */
  if (GTK_IS_LABEL(self->content_label)) {
    if (summary && *summary) {
      gchar *pango_summary = markdown_to_pango_summary(summary, 300);
      gtk_label_set_markup(GTK_LABEL(self->content_label), pango_summary);
      g_free(pango_summary);
    } else {
      gtk_label_set_text(GTK_LABEL(self->content_label), _("No summary available"));
    }
    gtk_widget_add_css_class(self->content_label, "article-summary");
  }

  /* Update timestamp to show publication date */
  if (published_at > 0 && GTK_IS_LABEL(self->lbl_timestamp)) {
    gchar *date_str = format_article_date(published_at);
    gtk_label_set_text(GTK_LABEL(self->lbl_timestamp), date_str);
    g_free(date_str);
  }

  /* Create hashtags box if we have hashtags */
  if (hashtags && hashtags[0]) {
    if (!self->article_hashtags_box) {
      self->article_hashtags_box = gtk_flow_box_new();
      gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->article_hashtags_box),
                                       GTK_SELECTION_NONE);
      gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->article_hashtags_box), 8);
      gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->article_hashtags_box), 1);
      gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->article_hashtags_box), 4);
      gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->article_hashtags_box), 6);
      gtk_widget_add_css_class(self->article_hashtags_box, "article-hashtags");

      /* Insert after content label */
      if (GTK_IS_WIDGET(self->content_label)) {
        GtkWidget *parent = gtk_widget_get_parent(self->content_label);
        if (GTK_IS_BOX(parent)) {
          gtk_box_insert_child_after(GTK_BOX(parent), self->article_hashtags_box,
                                      self->content_label);
        }
      }
    }

    /* Clear existing hashtags */
    GtkWidget *child = gtk_widget_get_first_child(self->article_hashtags_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_flow_box_remove(GTK_FLOW_BOX(self->article_hashtags_box), child);
      child = next;
    }

    /* Add hashtag chips */
    for (int i = 0; hashtags[i]; i++) {
      GtkWidget *chip = create_article_hashtag_chip(hashtags[i]);
      if (chip) {
        /* Connect click handler to emit search-hashtag signal */
        g_object_set_data_full(G_OBJECT(chip), "hashtag",
                               g_strdup(hashtags[i]), g_free);
        g_signal_connect(chip, "clicked",
          G_CALLBACK(on_article_hashtag_clicked), self);

        gtk_flow_box_append(GTK_FLOW_BOX(self->article_hashtags_box), chip);
      }
    }
    gtk_widget_set_visible(self->article_hashtags_box, TRUE);
  }

  /* Hide reply/repost/like buttons for articles - they use different actions */
  /* Keep zap and bookmark, add share */
  if (GTK_IS_WIDGET(self->btn_reply)) {
    gtk_widget_set_visible(self->btn_reply, FALSE);
  }
  if (GTK_IS_WIDGET(self->btn_repost)) {
    gtk_widget_set_visible(self->btn_repost, FALSE);
  }

  g_debug("NIP-23: Set article mode - title='%s' d_tag='%s'",
          title ? title : "(null)", d_tag ? d_tag : "(null)");
}

/* NIP-23: Check if this card is displaying an article */
gboolean gnostr_note_card_row_is_article(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), FALSE);
  return self->is_article;
}

/* NIP-23: Get the article's d-tag identifier */
const char *gnostr_note_card_row_get_article_d_tag(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->article_d_tag;
}

/* NIP-71: Helper to show video player and hide thumbnail overlay */
static void video_show_player(GnostrNoteCardRow *self) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!self->video_player || !self->video_url) return;

  /* Hide thumbnail overlay */
  if (GTK_IS_WIDGET(self->video_overlay)) {
    gtk_widget_set_visible(self->video_overlay, FALSE);
  }

  /* Show and start the video player */
  gtk_widget_set_visible(self->video_player, TRUE);
  gnostr_video_player_set_uri(GNOSTR_VIDEO_PLAYER(self->video_player), self->video_url);

  self->video_player_shown = TRUE;
  g_debug("NIP-71: Playing video: %s", self->video_url);
}

/* NIP-71: Play button clicked callback */
static void on_video_play_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnostrNoteCardRow *self = GNOSTR_NOTE_CARD_ROW(user_data);
  video_show_player(self);
}

/* Context struct for video thumbnail loading - prevents use-after-free */
typedef struct {
  GtkWidget *picture;  /* Weak reference to the picture widget */
  char *url;           /* URL being fetched (for debugging) */
} VideoThumbCtx;

static void on_video_thumb_picture_destroyed(gpointer data, GObject *where_the_object_was) {
  VideoThumbCtx *ctx = (VideoThumbCtx *)data;
  (void)where_the_object_was;
  if (ctx) ctx->picture = NULL;
}

static void video_thumb_ctx_free(VideoThumbCtx *ctx) {
  if (!ctx) return;
  if (ctx->picture) {
    g_object_weak_unref(G_OBJECT(ctx->picture), on_video_thumb_picture_destroyed, ctx);
  }
  g_free(ctx->url);
  g_free(ctx);
}

#ifdef HAVE_SOUP3
/* NIP-71: Async thumbnail image loader - uses context struct with weak reference */
static void on_video_thumb_bytes_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
  VideoThumbCtx *ctx = (VideoThumbCtx *)user_data;
  GError *error = NULL;
  GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &error);

  if (error) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_debug("NIP-71: Thumbnail load error: %s", error->message);
    }
    g_error_free(error);
    video_thumb_ctx_free(ctx);
    return;
  }

  if (!bytes || g_bytes_get_size(bytes) == 0) {
    if (bytes) g_bytes_unref(bytes);
    video_thumb_ctx_free(ctx);
    return;
  }

  /* Check if widget was destroyed via weak reference */
  if (!ctx->picture || !GTK_IS_PICTURE(ctx->picture)) {
    g_bytes_unref(bytes);
    video_thumb_ctx_free(ctx);
    return;
  }

  /* Load image from bytes */
  GdkTexture *texture = gdk_texture_new_from_bytes(bytes, NULL);
  g_bytes_unref(bytes);

  if (texture && GTK_IS_PICTURE(ctx->picture)) {
    gtk_picture_set_paintable(GTK_PICTURE(ctx->picture), GDK_PAINTABLE(texture));
    gtk_widget_set_visible(ctx->picture, TRUE);
    g_object_unref(texture);
  } else if (texture) {
    g_object_unref(texture);
  }

  video_thumb_ctx_free(ctx);
}

static void load_video_thumbnail(GnostrNoteCardRow *self, const char *thumb_url) {
  if (!self || !thumb_url || !*thumb_url) return;
  if (!GTK_IS_PICTURE(self->video_thumb_picture)) return;

  /* Cancel any previous fetch */
  if (self->video_thumb_cancellable) {
    g_cancellable_cancel(self->video_thumb_cancellable);
    g_clear_object(&self->video_thumb_cancellable);
  }

  self->video_thumb_cancellable = g_cancellable_new();

  /* Use shared session - do NOT create per-request session as it gets disposed
   * while the async request is still in flight, causing libsoup crashes */
  SoupMessage *msg = soup_message_new("GET", thumb_url);
  if (!msg) {
    return;
  }

  /* Create context with weak reference to picture widget */
  VideoThumbCtx *ctx = g_new0(VideoThumbCtx, 1);
  ctx->picture = self->video_thumb_picture;
  ctx->url = g_strdup(thumb_url);
  g_object_weak_ref(G_OBJECT(ctx->picture), on_video_thumb_picture_destroyed, ctx);

  soup_session_send_and_read_async(gnostr_get_shared_soup_session(), msg, G_PRIORITY_DEFAULT,
                                    self->video_thumb_cancellable,
                                    on_video_thumb_bytes_ready, ctx);
  g_object_unref(msg);
}
#endif

/* NIP-71: Create a hashtag chip button (reuse article pattern) */
static GtkWidget *create_video_hashtag_chip(const char *hashtag) {
  if (!hashtag || !*hashtag) return NULL;

  GtkWidget *btn = gtk_button_new();
  gtk_widget_add_css_class(btn, "pill");
  gtk_widget_add_css_class(btn, "video-hashtag");

  gchar *label_text = g_strdup_printf("#%s", hashtag);
  gtk_button_set_label(GTK_BUTTON(btn), label_text);
  g_free(label_text);

  return btn;
}

/* NIP-71: Set video mode for this note card */
void gnostr_note_card_row_set_video_mode(GnostrNoteCardRow *self,
                                          const char *video_url,
                                          const char *thumb_url,
                                          const char *title,
                                          const char *summary,
                                          gint64 duration,
                                          gboolean is_vertical,
                                          const char *d_tag,
                                          const char * const *hashtags) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!video_url || !*video_url) return;

  self->is_video = TRUE;
  self->video_player_shown = FALSE;

  /* Store video metadata */
  g_clear_pointer(&self->video_d_tag, g_free);
  g_clear_pointer(&self->video_url, g_free);
  g_clear_pointer(&self->video_thumb_url, g_free);
  g_clear_pointer(&self->video_title, g_free);

  self->video_d_tag = g_strdup(d_tag);
  self->video_url = g_strdup(video_url);
  self->video_thumb_url = g_strdup(thumb_url);
  self->video_title = g_strdup(title);
  self->video_duration = duration;
  self->video_is_vertical = is_vertical;

  /* Add video CSS class to root */
  if (GTK_IS_WIDGET(self->root)) {
    gtk_widget_add_css_class(self->root, "video-card");
    if (is_vertical) {
      gtk_widget_add_css_class(self->root, "video-vertical");
    } else {
      gtk_widget_add_css_class(self->root, "video-horizontal");
    }
  }

  /* Create video title label if title provided */
  if (title && *title && !self->video_title_label) {
    self->video_title_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(self->video_title_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(self->video_title_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(self->video_title_label), 0.0);
    gtk_label_set_lines(GTK_LABEL(self->video_title_label), 2);
    gtk_label_set_ellipsize(GTK_LABEL(self->video_title_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(self->video_title_label, "video-title");

    /* Insert title label before content label */
    if (GTK_IS_WIDGET(self->content_label)) {
      GtkWidget *parent = gtk_widget_get_parent(self->content_label);
      if (GTK_IS_BOX(parent)) {
        GtkWidget *sibling = gtk_widget_get_prev_sibling(self->content_label);
        if (sibling) {
          gtk_box_insert_child_after(GTK_BOX(parent), self->video_title_label, sibling);
        } else {
          gtk_box_prepend(GTK_BOX(parent), self->video_title_label);
        }
      }
    }
  }

  /* Set title text */
  if (GTK_IS_LABEL(self->video_title_label)) {
    gtk_label_set_text(GTK_LABEL(self->video_title_label),
                       (title && *title) ? title : _("Untitled Video"));
    gtk_widget_set_visible(self->video_title_label, TRUE);
  }

  /* Create video overlay (thumbnail + play button) */
  if (!self->video_overlay) {
    self->video_overlay = gtk_overlay_new();
    gtk_widget_add_css_class(self->video_overlay, "video-thumbnail-overlay");

    /* Calculate height based on orientation */
    int thumb_height = is_vertical ? 400 : 220;
    gtk_widget_set_size_request(self->video_overlay, -1, thumb_height);

    /* Create thumbnail picture */
    self->video_thumb_picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(self->video_thumb_picture), GTK_CONTENT_FIT_COVER);
    gtk_widget_add_css_class(self->video_thumb_picture, "video-thumbnail");
    gtk_overlay_set_child(GTK_OVERLAY(self->video_overlay), self->video_thumb_picture);

    /* Create play button overlay */
    self->video_play_overlay_btn = gtk_button_new_from_icon_name("media-playback-start-symbolic");
    gtk_widget_add_css_class(self->video_play_overlay_btn, "video-play-btn");
    gtk_widget_add_css_class(self->video_play_overlay_btn, "circular");
    gtk_widget_add_css_class(self->video_play_overlay_btn, "osd");
    gtk_widget_set_halign(self->video_play_overlay_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(self->video_play_overlay_btn, GTK_ALIGN_CENTER);
    gtk_overlay_add_overlay(GTK_OVERLAY(self->video_overlay), self->video_play_overlay_btn);

    g_signal_connect(self->video_play_overlay_btn, "clicked",
                     G_CALLBACK(on_video_play_clicked), self);

    /* Create duration badge if duration > 0 */
    if (duration > 0) {
      gchar *dur_str = gnostr_video_format_duration(duration);
      self->video_duration_badge = gtk_label_new(dur_str);
      g_free(dur_str);
      gtk_widget_add_css_class(self->video_duration_badge, "video-duration-badge");
      gtk_widget_set_halign(self->video_duration_badge, GTK_ALIGN_END);
      gtk_widget_set_valign(self->video_duration_badge, GTK_ALIGN_END);
      gtk_widget_set_margin_end(self->video_duration_badge, 8);
      gtk_widget_set_margin_bottom(self->video_duration_badge, 8);
      gtk_overlay_add_overlay(GTK_OVERLAY(self->video_overlay), self->video_duration_badge);
    }

    /* Insert overlay at the top of content area (or use media_box if available) */
    if (GTK_IS_WIDGET(self->media_box)) {
      gtk_box_prepend(GTK_BOX(self->media_box), self->video_overlay);
      gtk_widget_set_visible(self->media_box, TRUE);
    } else if (GTK_IS_WIDGET(self->content_label)) {
      GtkWidget *parent = gtk_widget_get_parent(self->content_label);
      if (GTK_IS_BOX(parent)) {
        /* Insert after title if present, else prepend */
        if (GTK_IS_WIDGET(self->video_title_label)) {
          gtk_box_insert_child_after(GTK_BOX(parent), self->video_overlay, self->video_title_label);
        } else {
          gtk_box_prepend(GTK_BOX(parent), self->video_overlay);
        }
      }
    }

    gtk_widget_set_visible(self->video_overlay, TRUE);
  }

  /* Create video player (hidden initially) */
  if (!self->video_player) {
    self->video_player = GTK_WIDGET(gnostr_video_player_new());
    int player_height = is_vertical ? 400 : 300;
    gtk_widget_set_size_request(self->video_player, -1, player_height);
    gtk_widget_add_css_class(self->video_player, "note-media-video");
    gtk_widget_set_visible(self->video_player, FALSE);

    /* Insert player after overlay */
    if (GTK_IS_WIDGET(self->video_overlay)) {
      GtkWidget *parent = gtk_widget_get_parent(self->video_overlay);
      if (GTK_IS_BOX(parent)) {
        gtk_box_insert_child_after(GTK_BOX(parent), self->video_player, self->video_overlay);
      }
    }
  }

  /* Load thumbnail if available */
  if (thumb_url && *thumb_url) {
#ifdef HAVE_SOUP3
    load_video_thumbnail(self, thumb_url);
#endif
  } else {
    /* Use a placeholder or show video icon */
    gtk_widget_add_css_class(self->video_thumb_picture, "video-no-thumbnail");
  }

  /* Set summary as content if provided */
  if (GTK_IS_LABEL(self->content_label)) {
    if (summary && *summary) {
      gtk_label_set_text(GTK_LABEL(self->content_label), summary);
      gtk_widget_add_css_class(self->content_label, "video-summary");
    } else {
      /* Hide content label if no summary */
      gtk_widget_set_visible(self->content_label, FALSE);
    }
  }

  /* Create hashtags box if we have hashtags */
  if (hashtags && hashtags[0]) {
    if (!self->video_hashtags_box) {
      self->video_hashtags_box = gtk_flow_box_new();
      gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->video_hashtags_box), GTK_SELECTION_NONE);
      gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->video_hashtags_box), 8);
      gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(self->video_hashtags_box), 1);
      gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(self->video_hashtags_box), 4);
      gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(self->video_hashtags_box), 6);
      gtk_widget_add_css_class(self->video_hashtags_box, "video-hashtags");

      /* Insert after video player */
      if (GTK_IS_WIDGET(self->video_player)) {
        GtkWidget *parent = gtk_widget_get_parent(self->video_player);
        if (GTK_IS_BOX(parent)) {
          gtk_box_insert_child_after(GTK_BOX(parent), self->video_hashtags_box, self->video_player);
        }
      }
    }

    /* Clear existing hashtags */
    GtkWidget *child = gtk_widget_get_first_child(self->video_hashtags_box);
    while (child) {
      GtkWidget *next = gtk_widget_get_next_sibling(child);
      gtk_flow_box_remove(GTK_FLOW_BOX(self->video_hashtags_box), child);
      child = next;
    }

    /* Add hashtag chips */
    for (int i = 0; hashtags[i]; i++) {
      GtkWidget *chip = create_video_hashtag_chip(hashtags[i]);
      if (chip) {
        g_object_set_data_full(G_OBJECT(chip), "hashtag", g_strdup(hashtags[i]), g_free);
        g_signal_connect(chip, "clicked",
          G_CALLBACK(on_article_hashtag_clicked), self);
        gtk_flow_box_append(GTK_FLOW_BOX(self->video_hashtags_box), chip);
      }
    }
    gtk_widget_set_visible(self->video_hashtags_box, TRUE);
  }

  /* Adjust action buttons for video - hide reply/repost since they're not typical for video events */
  if (GTK_IS_WIDGET(self->btn_reply)) {
    gtk_widget_set_visible(self->btn_reply, FALSE);
  }
  if (GTK_IS_WIDGET(self->btn_repost)) {
    gtk_widget_set_visible(self->btn_repost, FALSE);
  }

  g_debug("NIP-71: Set video mode - url='%s' title='%s' d_tag='%s' vertical=%d",
          video_url, title ? title : "(null)", d_tag ? d_tag : "(null)", is_vertical);
}

/* NIP-71: Check if this card is displaying a video */
gboolean gnostr_note_card_row_is_video(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), FALSE);
  return self->is_video;
}

/* NIP-71: Get the video's d-tag identifier */
const char *gnostr_note_card_row_get_video_d_tag(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->video_d_tag;
}

/* NIP-71: Get the video URL */
const char *gnostr_note_card_row_get_video_url(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->video_url;
}

/* NIP-84: Enable text selection mode for highlighting */
void gnostr_note_card_row_enable_text_selection(GnostrNoteCardRow *self, gboolean enable) {
  g_return_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self));

  if (GTK_IS_LABEL(self->content_label)) {
    gtk_label_set_selectable(GTK_LABEL(self->content_label), enable);
    if (enable) {
      gtk_widget_set_cursor_from_name(self->content_label, "text");
    } else {
      gtk_widget_set_cursor_from_name(self->content_label, "default");
    }
  }
}

/* NIP-84: Get the note's content text (for context extraction) */
const char *gnostr_note_card_row_get_content_text(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->content_text;
}

/* NIP-84: Get the note's event ID */
const char *gnostr_note_card_row_get_event_id(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->id_hex;
}

/* NIP-84: Get the note author's pubkey */
const char *gnostr_note_card_row_get_pubkey(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->pubkey_hex;
}

/* ============================================
   NIP-03 OpenTimestamps Implementation
   ============================================ */

/* Helper to create the OTS badge widget */
static GtkWidget *create_ots_badge(GnostrOtsStatus status, gint64 verified_timestamp, guint block_height) {
  GtkWidget *badge = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(badge, "ots-badge");
  gtk_widget_add_css_class(badge, gnostr_nip03_status_css_class(status));

  /* Icon */
  GtkWidget *icon = gtk_image_new_from_icon_name(gnostr_nip03_status_icon(status));
  gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_NORMAL);
  gtk_box_append(GTK_BOX(badge), icon);

  /* Label */
  const char *status_str = gnostr_nip03_status_string(status);
  GtkWidget *label = gtk_label_new(status_str);
  gtk_widget_add_css_class(label, "ots-status-label");
  gtk_box_append(GTK_BOX(badge), label);

  /* Build tooltip with details */
  GString *tooltip = g_string_new(NULL);
  g_string_append(tooltip, "OpenTimestamps Proof\n");

  switch (status) {
    case NIP03_OTS_STATUS_VERIFIED:
      if (verified_timestamp > 0) {
        char *ts_str = gnostr_nip03_format_timestamp(verified_timestamp);
        if (ts_str) {
          g_string_append_printf(tooltip, "%s\n", ts_str);
          g_free(ts_str);
        }
      }
      if (block_height > 0) {
        g_string_append_printf(tooltip, "Bitcoin block: %u", block_height);
      }
      break;
    case NIP03_OTS_STATUS_PENDING:
      g_string_append(tooltip, "Waiting for Bitcoin confirmation");
      break;
    case NIP03_OTS_STATUS_INVALID:
      g_string_append(tooltip, "Proof verification failed");
      break;
    default:
      g_string_append(tooltip, "Status unknown");
      break;
  }

  gtk_widget_set_tooltip_text(badge, tooltip->str);
  g_string_free(tooltip, TRUE);

  return badge;
}

/* NIP-03: Set OTS proof from event tags */
void gnostr_note_card_row_set_ots_proof(GnostrNoteCardRow *self, const char *tags_json) {
  g_return_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self));

  if (!tags_json || !*tags_json) return;

  /* Parse OTS tag */
  GnostrOtsProof *proof = gnostr_nip03_parse_ots_tag(tags_json, self->id_hex);
  if (!proof) {
    /* No OTS tag in event */
    self->has_ots_proof = FALSE;
    if (self->ots_badge) {
      gtk_widget_set_visible(self->ots_badge, FALSE);
    }
    return;
  }

  /* Store OTS state */
  self->has_ots_proof = TRUE;
  self->ots_status = proof->status;
  self->ots_verified_timestamp = proof->verified_timestamp;
  self->ots_block_height = proof->block_height;

  /* Create or update badge */
  GtkWidget *badge = create_ots_badge(proof->status, proof->verified_timestamp, proof->block_height);

  /* Find the header box to insert badge (next to timestamp) */
  if (GTK_IS_LABEL(self->lbl_timestamp)) {
    GtkWidget *parent = gtk_widget_get_parent(self->lbl_timestamp);
    if (GTK_IS_BOX(parent)) {
      /* Remove old badge if exists */
      if (self->ots_badge) {
        gtk_box_remove(GTK_BOX(parent), self->ots_badge);
      }

      /* Insert badge before timestamp */
      gtk_box_insert_child_after(GTK_BOX(parent), badge, self->lbl_handle);
      self->ots_badge = badge;
      gtk_widget_set_visible(self->ots_badge, TRUE);
    }
  }

  /* Cache the result */
  gnostr_nip03_cache_result(proof);
  gnostr_ots_proof_free(proof);

  g_debug("[NIP-03] Set OTS proof for event %s - status=%d block=%u",
          self->id_hex ? self->id_hex : "(null)",
          self->ots_status, self->ots_block_height);
}

/* NIP-03: Set OTS status directly */
void gnostr_note_card_row_set_ots_status(GnostrNoteCardRow *self,
                                          gint status,
                                          gint64 verified_timestamp,
                                          guint block_height) {
  g_return_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self));

  self->has_ots_proof = TRUE;
  self->ots_status = status;
  self->ots_verified_timestamp = verified_timestamp;
  self->ots_block_height = block_height;

  /* Create or update badge */
  GtkWidget *badge = create_ots_badge(status, verified_timestamp, block_height);

  /* Find the header box to insert badge */
  if (GTK_IS_LABEL(self->lbl_timestamp)) {
    GtkWidget *parent = gtk_widget_get_parent(self->lbl_timestamp);
    if (GTK_IS_BOX(parent)) {
      /* Remove old badge if exists */
      if (self->ots_badge) {
        gtk_box_remove(GTK_BOX(parent), self->ots_badge);
      }

      /* Insert badge before timestamp */
      gtk_box_insert_child_after(GTK_BOX(parent), badge, self->lbl_handle);
      self->ots_badge = badge;
      gtk_widget_set_visible(self->ots_badge, TRUE);
    }
  }
}

/* NIP-03: Check if note has OTS proof */
gboolean gnostr_note_card_row_has_ots_proof(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), FALSE);
  return self->has_ots_proof;
}

/* NIP-03: Get the verification timestamp */
gint64 gnostr_note_card_row_get_ots_timestamp(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), 0);
  return self->ots_verified_timestamp;
}

/* ============================================================================
 * NIP-48 Proxy Tags - Bridged content from external protocols
 * ============================================================================ */

/* NIP-48: Set proxy information for bridged content */
void gnostr_note_card_row_set_proxy_info(GnostrNoteCardRow *self,
                                          const char *proxy_id,
                                          const char *protocol) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  /* Clear existing proxy state */
  g_clear_pointer(&self->proxy_id, g_free);
  g_clear_pointer(&self->proxy_protocol, g_free);

  if (!proxy_id || !*proxy_id || !protocol || !*protocol) {
    /* Hide proxy indicator if present */
    if (GTK_IS_WIDGET(self->proxy_indicator_box)) {
      gtk_widget_set_visible(self->proxy_indicator_box, FALSE);
    }
    return;
  }

  self->proxy_id = g_strdup(proxy_id);
  self->proxy_protocol = g_strdup(protocol);

  /* Parse the protocol to get display info */
  GnostrProxyProtocol proto_enum = gnostr_proxy_parse_protocol(protocol);
  const char *display_name = gnostr_proxy_get_display_name(proto_enum);
  const char *icon_name = gnostr_proxy_get_icon_name(proto_enum);
  gboolean is_linkable = gnostr_proxy_is_url(proxy_id);

  /* Create proxy indicator box if it doesn't exist */
  if (!self->proxy_indicator_box && self->root && GTK_IS_WIDGET(self->root)) {
    self->proxy_indicator_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(self->proxy_indicator_box, "proxy-indicator");
    gtk_widget_add_css_class(self->proxy_indicator_box, "dim-label");
    gtk_widget_set_margin_start(self->proxy_indicator_box, 52);  /* Align with content */
    gtk_widget_set_margin_bottom(self->proxy_indicator_box, 2);
    gtk_widget_set_margin_top(self->proxy_indicator_box, 2);

    /* Insert at the top of the card (after repost indicator if present) */
    if (GTK_IS_BOX(self->root)) {
      /* Insert after the first child (repost indicator) if it exists,
         otherwise prepend */
      GtkWidget *first = gtk_widget_get_first_child(self->root);
      if (first && self->repost_indicator_box && first == self->repost_indicator_box) {
        gtk_box_insert_child_after(GTK_BOX(self->root), self->proxy_indicator_box, first);
      } else {
        gtk_box_prepend(GTK_BOX(self->root), self->proxy_indicator_box);
      }
    }
  }

  /* Clear existing children */
  if (GTK_IS_BOX(self->proxy_indicator_box)) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(self->proxy_indicator_box)) != NULL) {
      gtk_box_remove(GTK_BOX(self->proxy_indicator_box), child);
    }
  }

  /* Add protocol icon */
  GtkWidget *icon = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 12);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(self->proxy_indicator_box), icon);

  /* Add "via Protocol" text */
  gchar *source_text = g_strdup_printf("via %s", display_name);

  if (is_linkable) {
    /* Make it a clickable link */
    GtkWidget *link = gtk_link_button_new_with_label(proxy_id, source_text);
    gtk_widget_add_css_class(link, "flat");
    gtk_widget_add_css_class(link, "caption");
    gtk_widget_add_css_class(link, "proxy-link");
    gtk_box_append(GTK_BOX(self->proxy_indicator_box), link);
  } else {
    GtkWidget *label = gtk_label_new(source_text);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_widget_add_css_class(label, "caption");
    gtk_box_append(GTK_BOX(self->proxy_indicator_box), label);
  }

  g_free(source_text);

  /* Set tooltip with full source ID */
  gchar *tooltip = g_strdup_printf("Bridged from: %s", proxy_id);
  gtk_widget_set_tooltip_text(self->proxy_indicator_box, tooltip);
  g_free(tooltip);

  /* Show the indicator */
  gtk_widget_set_visible(self->proxy_indicator_box, TRUE);

  g_debug("NIP-48: Set proxy info - protocol=%s, id=%s", protocol, proxy_id);
}

/* NIP-48: Set proxy information from event tags JSON */
void gnostr_note_card_row_set_proxy_from_tags(GnostrNoteCardRow *self,
                                               const char *tags_json) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;

  if (!tags_json || !*tags_json) {
    /* No tags, hide proxy indicator */
    if (GTK_IS_WIDGET(self->proxy_indicator_box)) {
      gtk_widget_set_visible(self->proxy_indicator_box, FALSE);
    }
    return;
  }

  /* Parse proxy tag from JSON */
  GnostrProxyInfo *info = gnostr_proxy_parse_tags_json(tags_json);
  if (info) {
    gnostr_note_card_row_set_proxy_info(self, info->id, info->protocol_str);
    gnostr_proxy_info_free(info);
  } else {
    /* No proxy tag found, hide indicator */
    if (GTK_IS_WIDGET(self->proxy_indicator_box)) {
      gtk_widget_set_visible(self->proxy_indicator_box, FALSE);
    }
  }
}

/* NIP-48: Check if this note is bridged from another protocol */
gboolean gnostr_note_card_row_is_proxied(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), FALSE);
  return self->proxy_protocol != NULL && self->proxy_id != NULL;
}

/* NIP-48: Get the proxy source protocol */
const char *gnostr_note_card_row_get_proxy_protocol(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->proxy_protocol;
}

/* NIP-48: Get the proxy source ID/URL */
const char *gnostr_note_card_row_get_proxy_id(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->proxy_id;
}

/* ============================================
   NIP-73 External Content IDs Implementation
   ============================================ */

/* NIP-73: Set external content IDs from event tags */
void gnostr_note_card_row_set_external_ids(GnostrNoteCardRow *self, const char *tags_json) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!GTK_IS_FLOW_BOX(self->external_ids_box)) return;

  /* Clear existing external IDs */
  gnostr_note_card_row_clear_external_ids(self);

  if (!tags_json || !*tags_json) {
    return;
  }

  /* Parse external content IDs from tags */
  GPtrArray *content_ids = gnostr_nip73_parse_ids_from_tags_json(tags_json);
  if (!content_ids || content_ids->len == 0) {
    if (content_ids) g_ptr_array_unref(content_ids);
    return;
  }

  /* Store the content IDs */
  self->external_ids = content_ids;

  /* Add badges for each content ID */
  for (guint i = 0; i < content_ids->len; i++) {
    GnostrExternalContentId *content_id = g_ptr_array_index(content_ids, i);
    if (!content_id) continue;

    GtkWidget *badge = gnostr_nip73_create_badge(content_id);
    if (badge) {
      gtk_flow_box_append(GTK_FLOW_BOX(self->external_ids_box), badge);
    }
  }

  /* Show the container if we have badges */
  gtk_widget_set_visible(self->external_ids_box, TRUE);
}

/* NIP-73: Check if note has external content IDs */
gboolean gnostr_note_card_row_has_external_ids(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), FALSE);
  return self->external_ids != NULL && self->external_ids->len > 0;
}

/* NIP-73: Clear all external ID badges */
void gnostr_note_card_row_clear_external_ids(GnostrNoteCardRow *self) {
  if (!GNOSTR_IS_NOTE_CARD_ROW(self)) return;
  if (!GTK_IS_FLOW_BOX(self->external_ids_box)) return;

  /* Remove all children from the flow box */
  GtkWidget *child = gtk_widget_get_first_child(self->external_ids_box);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_flow_box_remove(GTK_FLOW_BOX(self->external_ids_box), child);
    child = next;
  }

  /* Clear the stored content IDs */
  if (self->external_ids) {
    g_ptr_array_unref(self->external_ids);
    self->external_ids = NULL;
  }

  gtk_widget_set_visible(self->external_ids_box, FALSE);
}

/**
 * gnostr_note_card_row_get_cancellable:
 *
 * Returns the shared cancellable for all async operations on this note card.
 */
GCancellable *gnostr_note_card_row_get_cancellable(GnostrNoteCardRow *self) {
  g_return_val_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self), NULL);
  return self->async_cancellable;
}

/**
 * gnostr_note_card_row_prepare_for_bind:
 *
 * Prepares the row for binding to a new list item. This resets the disposed
 * flag and reinitializes async state so the row can be safely reused.
 *
 * CRITICAL: Call this from factory_bind_cb BEFORE populating the row with data.
 * After prepare_for_unbind cancelled the cancellables, we need fresh ones for
 * the new binding.
 */
void gnostr_note_card_row_prepare_for_bind(GnostrNoteCardRow *self) {
  g_return_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self));

  /* Reset disposed flag - widget is being reused */
  self->disposed = FALSE;

  /* Create fresh cancellable since the old one was cancelled during unbind.
   * GCancellable cannot be reused after cancellation. */
  if (self->async_cancellable) {
    g_object_unref(self->async_cancellable);
  }
  self->async_cancellable = g_cancellable_new();

  /* Reset legacy cancellables too */
  if (self->nip05_cancellable) {
    g_object_unref(self->nip05_cancellable);
    self->nip05_cancellable = NULL;
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_object_unref(self->avatar_cancellable);
    self->avatar_cancellable = NULL;
  }
  if (self->article_image_cancellable) {
    g_object_unref(self->article_image_cancellable);
    self->article_image_cancellable = NULL;
  }
  /* Clear media cancellables - they'll be created on demand */
  if (self->media_cancellables) {
    g_hash_table_remove_all(self->media_cancellables);
  }
#endif
}

/**
 * gnostr_note_card_row_prepare_for_unbind:
 *
 * Prepares the row for unbinding from a list item. This cancels all async
 * operations and marks the row as disposed to prevent callbacks from
 * corrupting Pango state during the unbind/dispose process.
 *
 * CRITICAL: Also clears the avatar image's paintable to prevent
 * gtk_image_definition_unref crashes during rapid widget recycling.
 */
void gnostr_note_card_row_prepare_for_unbind(GnostrNoteCardRow *self) {
  g_return_if_fail(GNOSTR_IS_NOTE_CARD_ROW(self));

  /* Mark as disposed FIRST to prevent any async callbacks from running */
  self->disposed = TRUE;

  /* Do NOT call gtk_picture_set_paintable(NULL) here - it can trigger
   * gtk_image_definition_unref crashes if the GtkPicture is already in
   * an invalid state during rapid widget recycling. Let GTK handle
   * cleanup automatically during disposal. */

  /* NIP-71: Stop ALL video players IMMEDIATELY to prevent GStreamer from
   * accessing Pango layouts or other widget memory while the widget is being
   * disposed. This is CRITICAL - if dispose() is called with disposed==TRUE
   * (from a prior prepare_for_unbind call), it will jump to do_template_dispose
   * and skip the video player stopping code, causing memory corruption and crashes
   * like gtk_image_definition_unref when GStreamer writes to freed memory.
   * nostrc-7lv: Fix ASAN crash when clicking "New Notes" toast. */
  if (self->video_player && GNOSTR_IS_VIDEO_PLAYER(self->video_player)) {
    gnostr_video_player_stop(GNOSTR_VIDEO_PLAYER(self->video_player));
  }
  /* Stop any video players in media_box (inline videos from note content) */
  if (self->media_box && GTK_IS_BOX(self->media_box)) {
    GtkWidget *child = gtk_widget_get_first_child(self->media_box);
    while (child) {
      if (GNOSTR_IS_VIDEO_PLAYER(child)) {
        gnostr_video_player_stop(GNOSTR_VIDEO_PLAYER(child));
      }
      child = gtk_widget_get_next_sibling(child);
    }
  }

  /* OG Preview: Cancel async operations and mark as disposed to prevent
   * callbacks from accessing widget memory during disposal. Same pattern as
   * video player fix. nostrc-ofq: Fix crash during scroll.
   * NOTE: Do NOT use type-checking macros (OG_IS_PREVIEW_WIDGET) here - they
   * dereference the pointer which crashes if it contains garbage. The pointer
   * may be stale if the widget was destroyed or the row is being recycled.
   * Just check for NULL and trust initialization/cleanup sets it properly. */
  if (self->og_preview != NULL) {
    og_preview_widget_prepare_for_unbind(self->og_preview);
    self->og_preview = NULL;  /* Clear to prevent double-call */
  }

  /* Note Embed: Same pattern - cancel async operations and mark as disposed.
   * nostrc-ofq: Prevent crashes during scroll with embedded notes.
   * Same safety note: no type-checking macros on potentially stale pointers. */
  if (self->note_embed != NULL) {
    gnostr_note_embed_prepare_for_unbind(self->note_embed);
    self->note_embed = NULL;  /* Clear to prevent double-call */
  }

  /* Cancel all async operations immediately */
  if (self->async_cancellable) {
    g_cancellable_cancel(self->async_cancellable);
  }

  /* Cancel legacy cancellables */
  if (self->nip05_cancellable) {
    g_cancellable_cancel(self->nip05_cancellable);
  }

#ifdef HAVE_SOUP3
  if (self->avatar_cancellable) {
    g_cancellable_cancel(self->avatar_cancellable);
  }
  if (self->article_image_cancellable) {
    g_cancellable_cancel(self->article_image_cancellable);
  }
  /* Cancel all media fetches */
  if (self->media_cancellables) {
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, self->media_cancellables);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
      GCancellable *cancellable = G_CANCELLABLE(value);
      if (cancellable) g_cancellable_cancel(cancellable);
    }
  }
#endif

  /* Remove timestamp timer to prevent it from firing during disposal */
  if (self->timestamp_timer_id > 0) {
    g_source_remove(self->timestamp_timer_id);
    self->timestamp_timer_id = 0;
  }
}
