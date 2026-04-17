#include "gnostr-timeline-view-app-factory.h"
#include "gnostr-avatar-cache.h"
#include <nostr-gtk-1.0/gn-timeline-tabs.h>
/* nostrc-hqtn: gnostr-main-window.h moved to gnostr-timeline-action-relay.c */
#include <nostr-gtk-1.0/nostr-note-card-row.h>
/* nostrc-hqtn: gnostr-zap-dialog.h moved to gnostr-timeline-action-relay.c */
/* nostrc-hqtn: nostr_profile_provider.h moved to gnostr-timeline-action-relay.c */
#include "../model/gn-nostr-event-item.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include "nostr-event.h"
#include "nostr-json.h"
/* nostrc-hqtn: nostr_nip19.h, gnostr-relays.h moved to gnostr-timeline-action-relay.c */
#include "../util/utils.h"
#include "../util/bookmarks.h"
#include "../util/pin_list.h"
/* nostrc-hqtn: nip32_labels.h moved to gnostr-timeline-action-relay.c */
#include "../util/nip23.h"
#include "../util/nip34.h"
#include "../util/nip71.h"
#include "nostr-filter.h"
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <json-glib/json-glib.h>
#ifdef HAVE_SOUP3
#include <libsoup/soup.h>
#endif
#include "gnostr-timeline-embed-private.h"
#include "gnostr-timeline-metadata-controller.h"
#include "gnostr-timeline-action-relay.h"

/* Widget template now loaded from nostr-gtk library resource */

/* gnostr_avatar_metrics_log() now comes from avatar_cache. */

/* Avatar cache helpers are implemented in avatar_cache.c */

/* nostrc-lkoa: `TimelineItem` is library-internal (nostr-gtk).
 * This app standardizes on `GnNostrEventItem` for all timeline rows.
 * The previous app-side helpers and legacy `TimelineItem` bind/child-model
 * paths were removed along with the private-header include. If a future
 * consumer outside nostr-gtk needs `TimelineItem`, it must go through the
 * public API declared in <nostr-gtk-1.0/gnostr-timeline-view.h>. */

/* NostrGtkTimelineView type, struct, signals, dispose/finalize, and tab handler
 * all defined in nostr-gtk library (gnostr-timeline-view.c). */

/* Setup: load row UI from resource and set as child. Cache subwidgets on the row. */
/* nostrc-hqtn: 17 action-relay handlers moved to
 * gnostr-timeline-action-relay.{h,c}. The relay GObject is
 * created in gnostr_timeline_view_setup_app_factory() and
 * connected to each row via gnostr_timeline_action_relay_connect_row(). */



static NostrGtkNoteCardRow *get_bound_note_card_row_for_item(gpointer user_data, GObject *expected_item);

/* Callback when profile is loaded for an event item - show the row */
static void on_event_item_profile_changed(GObject *event_item, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;

  if (!event_item || !G_IS_OBJECT(event_item))
    return;

  NostrGtkNoteCardRow *card_row = get_bound_note_card_row_for_item(user_data, event_item);
  if (!card_row)
    return;

  GtkWidget *row = GTK_WIDGET(card_row);

  /* Check if profile is now available */
  GObject *profile = NULL;
  g_object_get(event_item, "profile", &profile, NULL);

  if (profile) {
    /* Profile loaded - show the row and update author info */
    gchar *display = NULL, *handle = NULL, *avatar_url = NULL, *nip05 = NULL;
    g_object_get(profile,
                 "display-name", &display,
                 "name",         &handle,
                 "picture-url",  &avatar_url,
                 "nip05",        &nip05,
                 NULL);

    if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
      nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row), display, handle, avatar_url);
      gtk_widget_set_visible(row, TRUE);

      /* NIP-05: Set verification identifier if available */
      if (nip05 && *nip05) {
        gchar *pubkey = NULL;
        g_object_get(event_item, "pubkey", &pubkey, NULL);
        if (pubkey && strlen(pubkey) == 64) {
          nostr_gtk_note_card_row_set_nip05(NOSTR_GTK_NOTE_CARD_ROW(row), nip05, pubkey);
        }
        g_free(pubkey);
      }
    }

    g_free(display);
    g_free(handle);
    g_free(avatar_url);
    g_free(nip05);
    g_object_unref(profile);
  }
}

/* Handler for search-hashtag signal from note card rows */
static void on_note_card_search_hashtag(NostrGtkNoteCardRow *row, const char *hashtag, gpointer user_data) {
  (void)row;
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(user_data);
  if (!self || !hashtag || !*hashtag) return;

  g_debug("timeline_view: search-hashtag signal received for #%s", hashtag);

  /* Add a new hashtag tab */
  nostr_gtk_timeline_view_add_hashtag_tab(self, hashtag);
}

static GtkWidget *create_timeline_note_card_row(NostrGtkTimelineView *self) {
  GtkWidget *row = GTK_WIDGET(nostr_gtk_note_card_row_new());

  /* nostrc-hqtn: Connect all 17 main-window-bound action signals via the
   * relay GObject. The relay is attached to the view via qdata in
   * gnostr_timeline_view_setup_app_factory() and is used as user_data
   * for all action signals, enabling single-call disconnect in
   * cleanup_bound_row() via g_signal_handlers_disconnect_by_data(). */
  GnostrTimelineActionRelay *relay =
      g_object_get_data(G_OBJECT(self), "gnostr-action-relay");
  if (relay) {
    gnostr_timeline_action_relay_connect_row(relay, NOSTR_GTK_NOTE_CARD_ROW(row));
    /* Store a borrowed (non-owning) pointer on the row so cleanup_bound_row()
     * can disconnect all relay signals via g_signal_handlers_disconnect_by_data(). */
    g_object_set_data(G_OBJECT(row), "gnostr-action-relay", relay);
  }

  /* search-hashtag uses the timeline view as user_data (not the relay)
   * because it calls nostr_gtk_timeline_view_add_hashtag_tab(). */
  if (self) {
    g_signal_connect(row, "search-hashtag", G_CALLBACK(on_note_card_search_hashtag), self);
  }

  return row;
}

static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f;
  (void)item;
  (void)data;
}

/* Avatar loading now handled by centralized gnostr-avatar-cache module */
/* nostrc-hqtn: Dead try_set_avatar() removed — the factory uses
 * nostr_gtk_note_card_row_set_author() for avatar rendering. */

static NostrGtkNoteCardRow *
get_bound_note_card_row_for_item(gpointer user_data, GObject *expected_item)
{
  if (!user_data || !GTK_IS_LIST_ITEM(user_data))
    return NULL;

  GtkListItem *list_item = GTK_LIST_ITEM(user_data);
  GObject *current_item = gtk_list_item_get_item(list_item);
  if (expected_item && current_item != expected_item)
    return NULL;

  GtkWidget *row = gtk_list_item_get_child(list_item);
  if (!NOSTR_GTK_IS_NOTE_CARD_ROW(row))
    return NULL;
  if (nostr_gtk_note_card_row_is_disposed(NOSTR_GTK_NOTE_CARD_ROW(row)))
    return NULL;
  if (!nostr_gtk_note_card_row_is_bound(NOSTR_GTK_NOTE_CARD_ROW(row)))
    return NULL;

  return NOSTR_GTK_NOTE_CARD_ROW(row);
}

/* Notify handlers to react to GnNostrEventItem property changes after initial bind */
static void on_item_notify_display_name(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, NULL);
  nostr_gtk_note_card_row_set_author(row, display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url);
}

static void on_item_notify_handle(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  gchar *display = NULL, *handle = NULL, *avatar_url = NULL;
  g_object_get(obj, "display-name", &display, "handle", &handle, "avatar-url", &avatar_url, NULL);
  nostr_gtk_note_card_row_set_author(row, display, handle, avatar_url);
  g_free(display); g_free(handle); g_free(avatar_url);
}

static void on_item_notify_avatar_url(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  gchar *url = NULL, *display = NULL, *handle = NULL;
  g_object_get(obj, "avatar-url", &url, "display-name", &display, "handle", &handle, NULL);
  nostr_gtk_note_card_row_set_author(row, display, handle, url);
  g_free(url); g_free(display); g_free(handle);
}

/* NIP-25: Notify handler for like count changes */
static void on_item_notify_like_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  guint like_count = 0;
  g_object_get(obj, "like-count", &like_count, NULL);
  nostr_gtk_note_card_row_set_like_count(row, like_count);
}

/* NIP-18: Notify handler for repost count changes */
static void on_item_notify_repost_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  guint repost_count = 0;
  g_object_get(obj, "repost-count", &repost_count, NULL);
  nostr_gtk_note_card_row_set_repost_count(row, repost_count);
}

/* NIP-25: Notify handler for is_liked changes */
static void on_item_notify_is_liked(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  gboolean is_liked = FALSE;
  g_object_get(obj, "is-liked", &is_liked, NULL);
  nostr_gtk_note_card_row_set_liked(row, is_liked);
}

/* NIP-57: Notify handler for zap count changes */
static void on_item_notify_zap_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  guint zap_count = 0;
  gint64 total_msat = 0;
  g_object_get(obj, "zap-count", &zap_count, "zap-total-msat", &total_msat, NULL);
  nostr_gtk_note_card_row_set_zap_stats(row, zap_count, total_msat);
}

/* hq-vvmzu: Notify handler for reply count changes */
static void on_item_notify_reply_count(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  guint reply_count = 0;
  g_object_get(obj, "reply-count", &reply_count, NULL);
  nostr_gtk_note_card_row_set_reply_count(row, reply_count);
}

/* NIP-57: Notify handler for zap total changes */
static void on_item_notify_zap_total_msat(GObject *obj, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  if (!obj || !G_IS_OBJECT(obj)) return;
  NostrGtkNoteCardRow *row = get_bound_note_card_row_for_item(user_data, obj);
  if (!row) return;
  guint zap_count = 0;
  gint64 total_msat = 0;
  g_object_get(obj, "zap-count", &zap_count, "zap-total-msat", &total_msat, NULL);
  nostr_gtk_note_card_row_set_zap_stats(row, zap_count, total_msat);
}

static void
cleanup_bound_row(GtkWidget *row)
{
  if (!row || !GTK_IS_WIDGET(row))
    return;

  /* nostrc-hqtn: Disconnect all relay-connected action signals in one call.
   * The relay pointer is borrowed (non-owning) from the timeline view qdata;
   * the relay's lifetime exceeds any individual row. */
  GnostrTimelineActionRelay *relay =
      g_object_get_data(G_OBJECT(row), "gnostr-action-relay");
  if (relay)
    g_signal_handlers_disconnect_by_data(row, relay);

  g_signal_handlers_disconnect_by_func(row, G_CALLBACK(gnostr_timeline_embed_on_row_request_embed), NULL);

  gulong map_id = (gulong)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "tv-tier2-map-id"));
  if (map_id > 0 && g_signal_handler_is_connected(G_OBJECT(row), map_id)) {
    g_signal_handler_disconnect(row, map_id);
  }
  g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GSIZE_TO_POINTER(0));

  gnostr_timeline_embed_inflight_detach_row(row);

  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
    nostr_gtk_note_card_row_prepare_for_unbind(NOSTR_GTK_NOTE_CARD_ROW(row));
  }
}

/* Unbind cleanup: disconnect signal handlers and detach inflight operations. */
static void factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = gtk_list_item_get_child(item);
  gpointer item_ptr = gtk_list_item_get_item(item);

  /* Disconnect handlers from the source object bound during the last bind.
   * Use the saved object, not gtk_list_item_get_item(), because GTK may already
   * have cleared the live item pointer by teardown time. */
  GObject *bound_item = g_object_get_data(G_OBJECT(item), "tv-bound-item");
  if (bound_item && G_IS_OBJECT(bound_item)) {
    g_signal_handlers_disconnect_by_data(bound_item, item);
    if (row && GTK_IS_WIDGET(row))
      g_signal_handlers_disconnect_by_data(bound_item, row);
  }
  g_object_set_data(G_OBJECT(item), "tv-bound-item", NULL);

  cleanup_bound_row(row);
}

/* Context for hashtag extraction callback */
typedef struct {
  GPtrArray *hashtags;
} HashtagExtractContext;

/* Callback for extracting hashtags */
static gboolean extract_hashtag_callback(gsize index, const gchar *tag_json, gpointer user_data) {
  (void)index;
  HashtagExtractContext *ctx = (HashtagExtractContext *)user_data;
  if (!tag_json || !ctx) return true;

  if (!gnostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(tag_json, NULL, 0, NULL);
  if (!tag_name) {
    return true;
  }

  if (g_strcmp0(tag_name, "t") != 0) {
    free(tag_name);
    return true;
  }
  free(tag_name);

  char *hashtag = NULL;
  if ((hashtag = gnostr_json_get_array_string(tag_json, NULL, 1, NULL)) != NULL && hashtag && *hashtag) {
    g_ptr_array_add(ctx->hashtags, g_strdup(hashtag));
  }
  free(hashtag);

  return true;
}

/**
 * parse_hashtags_from_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses the tags array to find all "t" (hashtag) tags.
 * Format: ["t", "hashtag"]
 *
 * Returns: (transfer full) (nullable): NULL-terminated array of hashtag strings,
 *          or NULL if none found. Free with g_strfreev().
 */
static gchar **parse_hashtags_from_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;
  if (!gnostr_json_is_array_str(tags_json)) return NULL;

  HashtagExtractContext ctx = { .hashtags = g_ptr_array_new() };
  gnostr_json_array_foreach_root(tags_json, extract_hashtag_callback, &ctx);

  if (ctx.hashtags->len == 0) {
    g_ptr_array_free(ctx.hashtags, TRUE);
    return NULL;
  }

  g_ptr_array_add(ctx.hashtags, NULL);  /* NULL-terminate */
  return (gchar **)g_ptr_array_free(ctx.hashtags, FALSE);
}

/* Context for content-warning extraction callback */
typedef struct {
  gchar *reason;
} ContentWarningContext;

/* Callback for extracting content-warning tag */
static gboolean extract_content_warning_callback(gsize index, const gchar *tag_json, gpointer user_data) {
  (void)index;
  ContentWarningContext *ctx = (ContentWarningContext *)user_data;
  if (!tag_json || !ctx || ctx->reason) return true; /* Stop if already found */

  if (!gnostr_json_is_array_str(tag_json)) return true;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(tag_json, NULL, 0, NULL);
  if (!tag_name) {
    return true;
  }

  /* NIP-36: Look for "content-warning" tag */
  if (g_strcmp0(tag_name, "content-warning") != 0) {
    free(tag_name);
    return true;
  }
  free(tag_name);

  /* Get reason if present */
  char *reason_str = NULL;
  reason_str = gnostr_json_get_array_string(tag_json, NULL, 1, NULL);
  if (reason_str) {
    ctx->reason = g_strdup(reason_str);
    free(reason_str);
  } else {
    ctx->reason = g_strdup("");  /* Tag exists but no reason provided */
  }

  return false; /* Stop iteration - found content-warning */
}

/**
 * parse_content_warning_from_tags_json:
 * @tags_json: JSON array string of event tags
 *
 * Parses the tags array to find a "content-warning" tag (NIP-36).
 * Format: ["content-warning", "optional reason"]
 *
 * Returns: (transfer full) (nullable): The content-warning reason string,
 *          empty string if tag exists without reason, or NULL if not present.
 */
static gchar *parse_content_warning_from_tags_json(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;
  if (!gnostr_json_is_array_str(tags_json)) return NULL;

  ContentWarningContext ctx = { .reason = NULL };
  gnostr_json_array_foreach_root(tags_json, extract_content_warning_callback, &ctx);

  return ctx.reason;
}

/* nostrc-lig9: NIP-65 reaction relay fetching removed.
 * Reaction counts are handled entirely by nostrdb's ndb_note_meta (O(1) lookup).
 * The relay-fetch path was O(A*R) network overhead for negligible marginal reactions. */

/* nostrc-qff / nostrc-hiei: Batch metadata loading.
 *
 * The batching machinery (worker-thread NDB queries, debounced idle
 * dispatch, result application) now lives in GnostrTimelineMetadataController
 * (apps/gnostr/src/ui/gnostr-timeline-metadata-controller.{c,h}). The
 * controller is attached to the view via qdata in
 * gnostr_timeline_view_setup_app_factory() so that this wrapper only
 * needs a fast qdata lookup in the bind hot path. A future refactor
 * (nostrc-hqtn) may fold this into an explicit adapter object.
 *
 * The item is narrowed to GnNostrEventItem: schedule_metadata_batch is
 * only called from the GnNostrEventItem bind branch of factory_bind_cb,
 * and the controller's result application applies only to that type. */
static void schedule_metadata_batch(NostrGtkTimelineView *self, GnNostrEventItem *item)
{
  GnostrTimelineMetadataController *ctrl =
      gnostr_timeline_metadata_controller_ensure(G_OBJECT(self));
  gnostr_timeline_metadata_controller_schedule(ctrl, item);
}

/*
 * Tier 2 map handler for timeline view: creates embeds/media/OG when the row
 * becomes visible. During Tier 1 (bind), only Pango markup is set via
 * set_content_markup_only(). This avoids creating NoteEmbed widgets with
 * synchronous NDB queries during the factory bind callback, which was blocking
 * the main thread and causing freezes when scrolling to events with embeds.
 * One-shot: disconnects itself after first run.
 */
static void
on_tv_row_mapped_tier2(GtkWidget *widget, gpointer user_data)
{
  GtkListItem *list_item = GTK_LIST_ITEM(user_data);
  if (!GTK_IS_LIST_ITEM(list_item)) return;

  GtkWidget *row = gtk_list_item_get_child(list_item);
  if (!NOSTR_GTK_IS_NOTE_CARD_ROW(row) || row != widget) return;
  if (nostr_gtk_note_card_row_is_disposed(NOSTR_GTK_NOTE_CARD_ROW(row))) return;
  if (!nostr_gtk_note_card_row_is_bound(NOSTR_GTK_NOTE_CARD_ROW(row))) return;

  GObject *obj = gtk_list_item_get_item(list_item);
  if (!obj) return;

  extern GType gn_nostr_event_item_get_type(void);
  if (!G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) return;

  GnNostrEventItem *item = GN_NOSTR_EVENT_ITEM(obj);
  const GnContentRenderResult *cached = gn_nostr_event_item_get_render_result(item);
  const char *tags_json = gn_nostr_event_item_get_tags_json(item);
  const char *content = gn_nostr_event_item_get_content(item);

  if (cached) {
    nostr_gtk_note_card_row_apply_deferred_content(NOSTR_GTK_NOTE_CARD_ROW(row), cached);
  }

  if (tags_json && *tags_json) {
    nostr_gtk_note_card_row_apply_deferred_tag_metadata(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);

    const char * const *item_hashtags = gn_nostr_event_item_get_hashtags(item);
    if (item_hashtags && item_hashtags[0]) {
      nostr_gtk_note_card_row_set_hashtags(NOSTR_GTK_NOTE_CARD_ROW(row), item_hashtags);
    } else {
      gchar **hashtags = parse_hashtags_from_tags_json(tags_json);
      if (hashtags) {
        nostr_gtk_note_card_row_set_hashtags(NOSTR_GTK_NOTE_CARD_ROW(row), (const char * const *)hashtags);
        g_strfreev(hashtags);
      }
    }

    gchar *content_warning = parse_content_warning_from_tags_json(tags_json);
    if (content_warning) {
      nostr_gtk_note_card_row_set_content_warning(NOSTR_GTK_NOTE_CARD_ROW(row), content_warning);
      g_free(content_warning);
    }
  }

  GNostrProfile *profile = gn_nostr_event_item_get_profile(item);
  if (profile) {
    const char *avatar_url = gnostr_profile_get_picture_url(profile);
    nostr_gtk_note_card_row_set_avatar(NOSTR_GTK_NOTE_CARD_ROW(row), avatar_url);
  }

  /* One-shot: disconnect after first run.
   * nostrc-icn: Check handler is still connected before disconnecting. */
  gulong map_id = (gulong)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(row), "tv-tier2-map-id"));
  if (map_id > 0 && g_signal_handler_is_connected(G_OBJECT(row), map_id)) {
    g_signal_handler_disconnect(row, map_id);
  }
  g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GSIZE_TO_POINTER(0));
}

static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f;
  NostrGtkTimelineView *self = NOSTR_GTK_TIMELINE_VIEW(data);
  GObject *obj = gtk_list_item_get_item(item);
  
  if (!obj) {
    return;
  }

  /* nostrc-lkoa: The gnostr app factory exclusively handles GnNostrEventItem.
   * The library still offers nostr_gtk_timeline_view_prepend()/set_tree_roots()
   * which insert TimelineItems, but those are library-internal helpers that
   * this factory is not wired for. Fail loudly in debug rather than silently
   * rendering an empty row if a caller mixes item types. */
  if (!GN_IS_NOSTR_EVENT_ITEM(obj)) {
    g_critical("factory_bind_cb: unexpected item type %s — only GnNostrEventItem is supported",
               G_OBJECT_TYPE_NAME(obj));
    return;
  }

  g_object_set_data_full(G_OBJECT(item), "tv-bound-item",
                         g_object_ref(obj), g_object_unref);
  
  gchar *display = NULL, *handle = NULL, *ts = NULL, *content = NULL, *root_id = NULL, *avatar_url = NULL;
  gchar *pubkey = NULL, *id_hex = NULL, *parent_id = NULL, *parent_pubkey = NULL, *nip05 = NULL;
  guint depth = 0; gboolean is_reply = FALSE; gint64 created_at = 0;
  
  /* Check if this is a GnNostrEventItem (new model) */
  extern GType gn_nostr_event_item_get_type(void);
  if (G_IS_OBJECT(obj) && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
    /* NEW: GnNostrEventItem binding */
    gboolean item_is_reply = FALSE;
    g_object_get(obj,
                 "event-id",      &id_hex,
                 "pubkey",        &pubkey,
                 "created-at",    &created_at,
                 "content",       &content,
                 "thread-root-id", &root_id,
                 "parent-id",     &parent_id,
                 "reply-depth",   &depth,
                 "is-reply",      &item_is_reply,
                 NULL);

    is_reply = item_is_reply || (parent_id != NULL);

    /* Get profile information */
    GObject *profile = NULL;
    g_object_get(obj, "profile", &profile, NULL);
    if (profile) {
      g_object_get(profile,
                   "display-name", &display,
                   "name",         &handle,
                   "picture-url",  &avatar_url,
                   "nip05",        &nip05,
                   NULL);
      g_object_unref(profile);
    }

    /* Format timestamp */
    if (created_at > 0) {
      time_t t = (time_t)created_at;
      struct tm *tm_info = localtime(&t);
      char buf[64];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm_info);
      ts = g_strdup(buf);
    }
    /* Debug logging removed - too verbose for per-item binding */
  }
  /* nostrc-lkoa: Legacy `TimelineItem` bind fallback was removed here.
   * The app only binds `GnNostrEventItem` rows; anything else is a
   * programmer error and will fall through to an unpopulated row. */
  GtkWidget *row = gtk_list_item_get_child(item);
  if (GTK_IS_WIDGET(row)) {
    cleanup_bound_row(row);
  }

  row = create_timeline_note_card_row(self);
  gtk_list_item_set_child(item, row);

  if (!GTK_IS_WIDGET(row)) return;
  if (NOSTR_GTK_IS_NOTE_CARD_ROW(row)) {
    /* CRITICAL: Prepare row for binding - resets disposed flag and creates fresh
     * cancellable. Must be called BEFORE populating the row with data.
     * nostrc-o7pp: Matches NoteCardFactory pattern. */
    nostr_gtk_note_card_row_prepare_for_bind(NOSTR_GTK_NOTE_CARD_ROW(row));

    /* Use pubkey prefix as fallback if no profile info available */
    g_autofree gchar *display_fallback = NULL;
    if (!display && !handle && pubkey && strlen(pubkey) >= 8) {
      display_fallback = g_strdup_printf("%.8s...", pubkey);
    }

    nostr_gtk_note_card_row_set_author_name_only(NOSTR_GTK_NOTE_CARD_ROW(row),
                                               display ? display : display_fallback,
                                               handle);
    nostr_gtk_note_card_row_set_timestamp(NOSTR_GTK_NOTE_CARD_ROW(row), created_at, ts);

    /* Connect embed request signal EARLY — before content setting, because
     * the Tier 2 map handler may fire immediately during bind (if row is
     * already mapped) and emit request-embed before we reach the signal
     * connection point at the end of bind.  Must be connected before
     * any code path that creates NoteEmbed widgets.
     * Disconnected in factory_unbind_cb to prevent accumulation. */
    g_signal_connect(row, "request-embed", G_CALLBACK(gnostr_timeline_embed_on_row_request_embed), NULL);

    /* NIP-92: Use imeta-aware setter if this is a GnNostrEventItem */
    const char *tags_json = NULL;
    gint event_kind = 1;  /* Default to kind 1 text note */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      tags_json = gn_nostr_event_item_get_tags_json(GN_NOSTR_EVENT_ITEM(obj));
      g_object_get(obj, "kind", &event_kind, NULL);
    }

    /* NIP-23: Handle long-form content (kind 30023) */
    if (gnostr_article_is_article(event_kind) && tags_json) {
      GnostrArticleMeta *article_meta = gnostr_article_parse_tags(tags_json);
      if (article_meta) {
        /* Use summary if available, otherwise use content excerpt */
        const char *summary = article_meta->summary;
        if (!summary && content) {
          /* Extract first ~300 chars as summary */
          summary = content;
        }

        nostr_gtk_note_card_row_set_article_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                               article_meta->title,
                                               summary,
                                               article_meta->image,
                                               article_meta->published_at > 0 ? article_meta->published_at : created_at,
                                               article_meta->d_tag,
                                               (const char * const *)article_meta->hashtags);

        gnostr_article_meta_free(article_meta);
        /* Debug logging removed - too verbose */
      } else {
        /* Fallback to regular note display if parsing fails */
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-71: Handle video events (kind 34235/34236) */
    else if (gnostr_video_is_video(event_kind) && tags_json) {
      GnostrVideoMeta *video_meta = gnostr_video_parse_tags(tags_json, event_kind);
      if (video_meta) {
        nostr_gtk_note_card_row_set_video_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                             video_meta->url,
                                             video_meta->thumb_url,
                                             video_meta->title,
                                             video_meta->summary,
                                             video_meta->duration,
                                             video_meta->orientation == GNOSTR_VIDEO_VERTICAL,
                                             video_meta->d_tag,
                                             (const char * const *)video_meta->hashtags);

        gnostr_video_meta_free(video_meta);
        /* Debug logging removed - too verbose */
      } else {
        /* Fallback to regular note display if parsing fails */
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-34: Handle git repository events (kind 30617) */
    else if (gnostr_nip34_is_repo(event_kind) && tags_json) {
      GnostrRepoMeta *repo_meta = gnostr_repo_parse_tags(tags_json);
      if (repo_meta) {
        nostr_gtk_note_card_row_set_git_repo_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                repo_meta->name,
                                                repo_meta->description,
                                                (const char *const *)repo_meta->clone_urls,
                                                (const char *const *)repo_meta->web_urls,
                                                (const char *const *)repo_meta->topics,
                                                repo_meta->maintainers_count,
                                                repo_meta->license);
        gnostr_repo_meta_free(repo_meta);
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-34: Handle git patch events (kind 1617) */
    else if (gnostr_nip34_is_patch(event_kind) && tags_json) {
      GnostrPatchMeta *patch_meta = gnostr_patch_parse_tags(tags_json, content);
      if (patch_meta) {
        gchar *repo_name = gnostr_nip34_get_repo_identifier(patch_meta->repo_a_tag);
        nostr_gtk_note_card_row_set_git_patch_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                 patch_meta->title,
                                                 repo_name,
                                                 patch_meta->commit_id);
        g_free(repo_name);
        gnostr_patch_meta_free(patch_meta);
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }
    /* NIP-34: Handle git issue events (kind 1621) */
    else if (gnostr_nip34_is_issue(event_kind) && tags_json) {
      GnostrIssueMeta *issue_meta = gnostr_issue_parse_tags(tags_json, content);
      if (issue_meta) {
        gchar *repo_name = gnostr_nip34_get_repo_identifier(issue_meta->repo_a_tag);
        nostr_gtk_note_card_row_set_git_issue_mode(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                 issue_meta->title,
                                                 repo_name,
                                                 issue_meta->is_open,
                                                 (const char *const *)issue_meta->labels);
        g_free(repo_name);
        gnostr_issue_meta_free(issue_meta);
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    }

    if (tags_json) {
      if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
        const GnContentRenderResult *cached = gn_nostr_event_item_get_render_result(GN_NOSTR_EVENT_ITEM(obj));
        if (cached) {
          nostr_gtk_note_card_row_set_content_tagged_markup_only(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json, cached);
          gulong map_id = g_signal_connect(row, "map",
                                            G_CALLBACK(on_tv_row_mapped_tier2), item);
          g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GSIZE_TO_POINTER((gsize)map_id));
          if (gtk_widget_get_mapped(row)) {
            on_tv_row_mapped_tier2(row, item);
          }
        } else {
          nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
        }
      } else {
        nostr_gtk_note_card_row_set_content_with_imeta(NOSTR_GTK_NOTE_CARD_ROW(row), content, tags_json);
      }
    } else {
      /* Tier 1: markup only — defer embeds/media/OG to Tier 2 map handler.
       * set_content_rendered and set_content create NoteEmbed widgets with
       * synchronous NDB queries during bind, blocking the main thread. */
      if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
        const GnContentRenderResult *cached = gn_nostr_event_item_get_render_result(GN_NOSTR_EVENT_ITEM(obj));
        if (cached) {
          nostr_gtk_note_card_row_set_content_markup_only(NOSTR_GTK_NOTE_CARD_ROW(row), content, cached);
          gulong map_id = g_signal_connect(row, "map",
                                            G_CALLBACK(on_tv_row_mapped_tier2), item);
          g_object_set_data(G_OBJECT(row), "tv-tier2-map-id", GSIZE_TO_POINTER((gsize)map_id));
          if (gtk_widget_get_mapped(row)) {
            on_tv_row_mapped_tier2(row, item);
          }
        } else {
          /* No cache: fall back to full render (first bind) */
          nostr_gtk_note_card_row_set_content(NOSTR_GTK_NOTE_CARD_ROW(row), content);
        }
      } else {
        nostr_gtk_note_card_row_set_content(NOSTR_GTK_NOTE_CARD_ROW(row), content);
      }
    }
    nostr_gtk_note_card_row_set_depth(NOSTR_GTK_NOTE_CARD_ROW(row), depth);
    nostr_gtk_note_card_row_set_ids(NOSTR_GTK_NOTE_CARD_ROW(row), id_hex, root_id, pubkey);

    /* Set NIP-10 thread info (reply indicator, view thread button) */
    nostr_gtk_note_card_row_set_thread_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                          root_id,
                                          parent_id,
                                          NULL, /* parent_author_name - will be resolved asynchronously if needed */
                                          is_reply);

    /* NIP-18: Handle GnNostrEventItem kind 6 reposts and q-tag quote reposts.
     * nostrc-lkoa: Repost state sourced from GnNostrEventItem.
     * nostrc-cj8p: Quote repost state (q-tag) now modelled on GnNostrEventItem. */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      gboolean is_repost = gn_nostr_event_item_get_is_repost(GN_NOSTR_EVENT_ITEM(obj));
      if (is_repost) {
        /* Get the referenced event ID from the repost's tags */
        char *reposted_id = gn_nostr_event_item_get_reposted_event_id(GN_NOSTR_EVENT_ITEM(obj));
        if (reposted_id) {
          /* Mark this as a repost and set reposter info */
          nostr_gtk_note_card_row_set_is_repost(NOSTR_GTK_NOTE_CARD_ROW(row), TRUE);
          nostr_gtk_note_card_row_set_repost_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                pubkey,   /* reposter pubkey */
                                                display ? display : (handle ? handle : NULL), /* reposter name */
                                                created_at); /* repost timestamp */

          /* Try to fetch the original note from local storage */
          char *orig_json = NULL;
          int orig_len = 0;
          if (storage_ndb_get_note_by_id_nontxn(reposted_id, &orig_json, &orig_len) == 0 && orig_json) {
            /* Parse the original event to get author and content */
            NostrEvent *orig_evt = nostr_event_new();
            if (orig_evt && nostr_event_deserialize(orig_evt, orig_json) == 0) {
              const char *orig_content = nostr_event_get_content(orig_evt);
              const char *orig_pubkey = nostr_event_get_pubkey(orig_evt);
              gint64 orig_created_at = (gint64)nostr_event_get_created_at(orig_evt);

              /* Update the card with original note's content */
              if (orig_content) {
                nostr_gtk_note_card_row_set_content(NOSTR_GTK_NOTE_CARD_ROW(row), orig_content);
              }

              /* Update timestamp to original note's time */
              if (orig_created_at > 0) {
                time_t t = (time_t)orig_created_at;
                struct tm *tm_info = localtime(&t);
                char orig_ts_buf[64];
                strftime(orig_ts_buf, sizeof(orig_ts_buf), "%Y-%m-%d %H:%M", tm_info);
                nostr_gtk_note_card_row_set_timestamp(NOSTR_GTK_NOTE_CARD_ROW(row), orig_created_at, orig_ts_buf);
              }

              /* Try to get original author's profile */
              if (orig_pubkey && strlen(orig_pubkey) == 64) {
                void *txn = NULL;
                if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
                  unsigned char pk_bytes[32];
                  if (gnostr_timeline_embed_hex32_from_string(orig_pubkey, pk_bytes)) {
                    char *profile_json = NULL;
                    int profile_len = 0;
                    if (storage_ndb_get_profile_by_pubkey(txn, pk_bytes, &profile_json, &profile_len, NULL) == 0 && profile_json) {
                      /* Parse profile JSON to get display name */
                      if (gnostr_json_is_valid(profile_json)) {
                        /* Profile is stored as event - need to parse content */
                        char *profile_content = NULL;
                        profile_content = gnostr_json_get_string(profile_json, "content", NULL);
                        if (profile_content) {
                          if (gnostr_json_is_valid(profile_content)) {
                            char *orig_name = NULL;
                            char *orig_display = NULL;
                            char *orig_avatar = NULL;
                            char *orig_nip05_str = NULL;

                            orig_display = gnostr_json_get_string(profile_content, "display_name", NULL);
                            orig_name = gnostr_json_get_string(profile_content, "name", NULL);
                            orig_avatar = gnostr_json_get_string(profile_content, "picture", NULL);
                            orig_nip05_str = gnostr_json_get_string(profile_content, "nip05", NULL);

                            /* Update author display with original author */
                            nostr_gtk_note_card_row_set_author(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                             orig_display && *orig_display ? orig_display : orig_name,
                                                             orig_name,
                                                             orig_avatar);

                            /* Update IDs to use original note's pubkey for actions */
                            nostr_gtk_note_card_row_set_ids(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                          reposted_id, root_id, (char*)orig_pubkey);

                            /* Update NIP-05 if available */
                            if (orig_nip05_str && *orig_nip05_str) {
                              nostr_gtk_note_card_row_set_nip05(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                              orig_nip05_str, orig_pubkey);
                            }

                            free(orig_name);
                            free(orig_display);
                            free(orig_avatar);
                            free(orig_nip05_str);
                          }
                          free(profile_content);
                        }
                      }
                    }
                  }
                  storage_ndb_end_query(txn);
                }
              }
            }
            if (orig_evt) nostr_event_free(orig_evt);
          } else {
            /* Original note not in local storage - request embed fetch */
            g_autofree gchar *nostr_uri = g_strdup_printf("nostr:note1%s", reposted_id);
            g_signal_emit_by_name(row, "request-embed", nostr_uri);
          }
          g_free(reposted_id);
        }
      }
    }

    /* NIP-18 (nostrc-cj8p): Handle q-tag quote reposts.
     * Any note (not just kind 6) can contain a "q" tag referencing a quoted note.
     * Fetch the quoted note from local storage and render inline.
     * Skip when is_repost (kind 6) — the repost block above already handles those. */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type()) &&
        !gn_nostr_event_item_get_is_repost(GN_NOSTR_EVENT_ITEM(obj))) {
      const char *quoted_id = gn_nostr_event_item_get_quoted_event_id(GN_NOSTR_EVENT_ITEM(obj));
      if (quoted_id) {
        char *quoted_json = NULL;
        int quoted_len = 0;
        const char *quoted_content = NULL;
        const char *quoted_author = NULL;

        if (storage_ndb_get_note_by_id_nontxn(quoted_id, &quoted_json, &quoted_len) == 0 && quoted_json) {
          NostrEvent *quoted_evt = nostr_event_new();
          if (quoted_evt && nostr_event_deserialize(quoted_evt, quoted_json) == 0) {
            quoted_content = nostr_event_get_content(quoted_evt);
            const char *quoted_pk = nostr_event_get_pubkey(quoted_evt);

            /* Try to get quoted author's display name from profile */
            if (quoted_pk && strlen(quoted_pk) == 64) {
              void *txn = NULL;
              if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
                unsigned char pk_bytes[32];
                if (gnostr_timeline_embed_hex32_from_string(quoted_pk, pk_bytes)) {
                  char *profile_json = NULL;
                  int profile_len = 0;
                  if (storage_ndb_get_profile_by_pubkey(txn, pk_bytes, &profile_json, &profile_len, NULL) == 0 && profile_json) {
                    if (gnostr_json_is_valid(profile_json)) {
                      char *profile_content = gnostr_json_get_string(profile_json, "content", NULL);
                      if (profile_content && gnostr_json_is_valid(profile_content)) {
                        char *qname = gnostr_json_get_string(profile_content, "display_name", NULL);
                        if (!qname || !*qname) {
                          free(qname);
                          qname = gnostr_json_get_string(profile_content, "name", NULL);
                        }
                        if (qname && *qname) {
                          quoted_author = qname; /* owned — freed after set_quote_info call */
                        } else {
                          free(qname);
                        }
                        free(profile_content);
                      }
                    }
                    free(profile_json);
                  }
                }
                storage_ndb_end_query(txn);
              }
            }

            nostr_gtk_note_card_row_set_quote_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                   quoted_id, quoted_content, quoted_author);
            if (quoted_author) free((char *)quoted_author);
          }
          if (quoted_evt) nostr_event_free(quoted_evt);
          free(quoted_json);
        } else {
          /* Quoted note not in local storage — request async fetch via embed mechanism */
          g_autofree gchar *nostr_uri = g_strdup_printf("nostr:note1%s", quoted_id);
          g_signal_emit_by_name(row, "request-embed", nostr_uri);
        }
      }
    }

    /* NIP-57: Handle zap receipt events (kind 9735) */
    if (event_kind == 9735 && G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      /* For zap receipts, mark them as such and set up the indicator */
      nostr_gtk_note_card_row_set_is_zap_receipt(NOSTR_GTK_NOTE_CARD_ROW(row), TRUE);

      /* Get zap stats from the event item if available */
      gint64 zap_total = gn_nostr_event_item_get_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj));

      /* Simple zap display - show "⚡ Zap" with amount if available */
      nostr_gtk_note_card_row_set_zap_receipt_info(NOSTR_GTK_NOTE_CARD_ROW(row),
                                                 pubkey,    /* sender is the event pubkey for now */
                                                 display,   /* sender name */
                                                 NULL,      /* recipient - parsed from tags if needed */
                                                 NULL,      /* recipient name */
                                                 NULL,      /* target event - parsed from tags if needed */
                                                 zap_total > 0 ? zap_total : 21000); /* Default 21 sats if unknown */
    }

    /* NIP-05: Set verification identifier for async verification badge */
    if (nip05 && *nip05 && pubkey && strlen(pubkey) == 64) {
      nostr_gtk_note_card_row_set_nip05(NOSTR_GTK_NOTE_CARD_ROW(row), nip05, pubkey);
    }

    /* NIP-51: Set bookmark and pin state from local cache */
    if (id_hex && strlen(id_hex) == 64) {
      GnostrBookmarks *bookmarks = gnostr_bookmarks_get_default();
      if (bookmarks) {
        gboolean is_bookmarked = gnostr_bookmarks_is_bookmarked(bookmarks, id_hex);
        nostr_gtk_note_card_row_set_bookmarked(NOSTR_GTK_NOTE_CARD_ROW(row), is_bookmarked);
      }
      GnostrPinList *pin_list = gnostr_pin_list_get_default();
      if (pin_list) {
        gboolean is_pinned = gnostr_pin_list_is_pinned(pin_list, id_hex);
        nostr_gtk_note_card_row_set_pinned(NOSTR_GTK_NOTE_CARD_ROW(row), is_pinned);
      }
    }

    /* NIP-09: Check if this is the current user's own note (enables delete option) */
    /* Also set login state for authentication-required buttons */
    gchar *user_pubkey = gnostr_timeline_embed_get_current_user_pubkey_hex();
    gboolean is_logged_in = (user_pubkey != NULL);
    nostr_gtk_note_card_row_set_logged_in(NOSTR_GTK_NOTE_CARD_ROW(row), is_logged_in);
    if (pubkey && strlen(pubkey) == 64 && user_pubkey) {
      gboolean is_own = (g_ascii_strcasecmp(pubkey, user_pubkey) == 0);
      nostr_gtk_note_card_row_set_is_own_note(NOSTR_GTK_NOTE_CARD_ROW(row), is_own);
    } else {
      nostr_gtk_note_card_row_set_is_own_note(NOSTR_GTK_NOTE_CARD_ROW(row), FALSE);
    }

    /* nostrc-7o7: Apply no-animation class if item was added outside visible viewport */
    if (G_TYPE_CHECK_INSTANCE_TYPE(obj, gn_nostr_event_item_get_type())) {
      gboolean skip_anim = gn_nostr_event_item_get_skip_animation(GN_NOSTR_EVENT_ITEM(obj));
      if (skip_anim) {
        gtk_widget_add_css_class(row, "no-animation");
      } else {
        gtk_widget_remove_css_class(row, "no-animation");
      }

      /* NIP-25: Set reaction count and liked state from model or local storage */
      guint like_count = gn_nostr_event_item_get_like_count(GN_NOSTR_EVENT_ITEM(obj));
      gboolean is_liked = gn_nostr_event_item_get_is_liked(GN_NOSTR_EVENT_ITEM(obj));
      guint repost_count = gn_nostr_event_item_get_repost_count(GN_NOSTR_EVENT_ITEM(obj));
      guint reply_count = gn_nostr_event_item_get_reply_count(GN_NOSTR_EVENT_ITEM(obj));
      guint zap_count = gn_nostr_event_item_get_zap_count(GN_NOSTR_EVENT_ITEM(obj));
      gint64 zap_total = gn_nostr_event_item_get_zap_total_msat(GN_NOSTR_EVENT_ITEM(obj));

      /* nostrc-nke8: Skip expensive DB lookups and network fetches for off-screen items
       * Defer if: (1) fast scrolling OR (2) item position is outside visible range */
      guint item_position = gtk_list_item_get_position(item);
      gboolean is_visible = nostr_gtk_timeline_view_is_item_visible(self, item_position);
      gboolean defer_metadata = self && (nostr_gtk_timeline_view_is_fast_scrolling(self) || !is_visible);

      /* nostrc-qff: Batch metadata loading. Instead of 3 individual DB queries
       * per item (N+1 pattern), schedule for batch processing. The idle callback
       * fires after all bind calls in this main loop iteration complete, doing
       * 3 total queries instead of 3*N. */
      if (!defer_metadata) {
        if ((like_count == 0 || zap_count == 0) && id_hex && strlen(id_hex) == 64) {
          schedule_metadata_batch(self, GN_NOSTR_EVENT_ITEM(obj));
        }
        gtk_widget_remove_css_class(row, "needs-metadata-refresh");
      } else {
        /* Mark for deferred refresh when scroll stops or item becomes visible */
        gtk_widget_add_css_class(row, "needs-metadata-refresh");
        g_debug("[SCROLL] Deferring metadata load for item position=%u (fast=%s visible=%s)",
                item_position,
                nostr_gtk_timeline_view_is_fast_scrolling(self) ? "Y" : "N",
                is_visible ? "Y" : "N");
      }

      nostr_gtk_note_card_row_set_like_count(NOSTR_GTK_NOTE_CARD_ROW(row), like_count);
      nostr_gtk_note_card_row_set_liked(NOSTR_GTK_NOTE_CARD_ROW(row), is_liked);
      nostr_gtk_note_card_row_set_repost_count(NOSTR_GTK_NOTE_CARD_ROW(row), repost_count);
      nostr_gtk_note_card_row_set_reply_count(NOSTR_GTK_NOTE_CARD_ROW(row), reply_count);
      nostr_gtk_note_card_row_set_zap_stats(NOSTR_GTK_NOTE_CARD_ROW(row), zap_count, zap_total);
    }

    g_free(user_pubkey);

    /* Always show row - use fallback display if no profile */
    gtk_widget_set_visible(row, TRUE);

    /* Connect to profile change notification to update author when profile loads.
     * Use plain g_signal_connect (NOT g_signal_connect_object) because unbind
     * explicitly disconnects via disconnect_by_data. Combining explicit disconnect
     * with g_signal_connect_object causes double-removal of invalidation notifiers
     * → "unable to remove uninstalled invalidation notifier" → SIGABRT. */
    if (!display && !handle) {
      g_signal_connect(obj, "notify::profile",
                       G_CALLBACK(on_event_item_profile_changed), item);
    }

    /* request-embed signal connected early (before content setting) — see above. */
  }

  /* Debug logging removed - too verbose for per-item binding */
  g_free(display); g_free(handle); g_free(ts); g_free(content); g_free(root_id); g_free(id_hex);
  g_free(avatar_url); g_free(parent_id); g_free(parent_pubkey); g_free(nip05);

  /* Model-level profile gating handles profile fetching; no bind-time enqueue here. */
  g_free(pubkey);

  /* Connect reactive updates so that later metadata changes update UI.
   * Use the GtkListItem as the per-bind cookie and disconnect by that same
   * cookie in unbind/teardown. This avoids mixing g_signal_connect_object()
   * invalidation notifiers with explicit disconnects on recycled rows. */
  if (obj && G_IS_OBJECT(obj) && GTK_IS_WIDGET(row)) {
    g_signal_connect(obj, "notify::display-name", G_CALLBACK(on_item_notify_display_name), item);
    g_signal_connect(obj, "notify::handle",       G_CALLBACK(on_item_notify_handle),       item);
    g_signal_connect(obj, "notify::avatar-url",   G_CALLBACK(on_item_notify_avatar_url),   item);

    /* NIP-25: Connect reaction count/state change handlers */
    g_signal_connect(obj, "notify::like-count",   G_CALLBACK(on_item_notify_like_count),   item);
    g_signal_connect(obj, "notify::is-liked",     G_CALLBACK(on_item_notify_is_liked),     item);

    /* NIP-18: Connect repost count change handler */
    g_signal_connect(obj, "notify::repost-count", G_CALLBACK(on_item_notify_repost_count), item);

    /* NIP-57: Connect zap stats change handlers */
    g_signal_connect(obj, "notify::zap-count",      G_CALLBACK(on_item_notify_zap_count),      item);
    g_signal_connect(obj, "notify::zap-total-msat", G_CALLBACK(on_item_notify_zap_total_msat), item);

    /* hq-vvmzu: Connect reply count change handler */
    g_signal_connect(obj, "notify::reply-count",    G_CALLBACK(on_item_notify_reply_count),    item);
  }
}

/* nostrc-heap-fix: Teardown handler to ensure signal handlers are disconnected
 * when items are removed from the model. GTK may call teardown without unbind
 * during rapid model changes, causing heap corruption in closure_array_destroy_all. */
static void factory_teardown_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data) {
  (void)f; (void)data;
  GtkWidget *row = gtk_list_item_get_child(item);

  /* Disconnect from the source object saved during bind. */
  GObject *bound_item = g_object_get_data(G_OBJECT(item), "tv-bound-item");
  if (bound_item && G_IS_OBJECT(bound_item)) {
    g_signal_handlers_disconnect_by_data(bound_item, item);
    if (row && GTK_IS_WIDGET(row))
      g_signal_handlers_disconnect_by_data(bound_item, row);
  }
  g_object_set_data(G_OBJECT(item), "tv-bound-item", NULL);

  cleanup_bound_row(row);
}

void gnostr_timeline_view_setup_app_factory(NostrGtkTimelineView *self) {
  g_return_if_fail(NOSTR_GTK_IS_TIMELINE_VIEW(self));

  /* nostrc-hiei: Eagerly create the metadata controller and attach it
   * to the view via qdata so schedule_metadata_batch() in the bind hot
   * path only pays for a qdata lookup (no allocation / NULL-branch). */
  (void)gnostr_timeline_metadata_controller_ensure(G_OBJECT(self));

  /* nostrc-hqtn: Create the action relay and attach to the view.
   * The relay is the central user_data for all 17 NoteCardRow action
   * signals, enabling single-call disconnect in cleanup_bound_row().
   * Destroy notify unrefs the relay when the view disposes. */
  GnostrTimelineActionRelay *relay = gnostr_timeline_action_relay_new();
  g_object_set_data_full(G_OBJECT(self), "gnostr-action-relay",
                         relay, g_object_unref);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(factory_setup_cb), self);
  g_signal_connect(factory, "bind", G_CALLBACK(factory_bind_cb), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(factory_unbind_cb), self);
  g_signal_connect(factory, "teardown", G_CALLBACK(factory_teardown_cb), self);
  nostr_gtk_timeline_view_set_factory(self, factory);
  g_object_unref(factory);

  /* Load app stylesheet for note cards */
  GtkCssProvider *prov = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(prov, "/org/gnostr/ui/ui/styles/gnostr.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(),
      GTK_STYLE_PROVIDER(prov), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(prov);

  g_debug("gnostr_timeline_view_setup_app_factory: factory installed");
}

/* ensure_list_model, scroll tracking, class_init, init, new, and all public API
 * functions are now defined in the nostr-gtk library. Only the factory setup
 * (gnostr_timeline_view_setup_app_factory) remains in the app.
 *
 * nostrc-lkoa: The app no longer references `TimelineItem` internals;
 * the child-model passthrough that required them was removed. */
