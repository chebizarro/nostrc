#ifndef GN_NOSTR_EVENT_MODEL_H
#define GN_NOSTR_EVENT_MODEL_H

#include "gn-nostr-event-item.h"
#include "gn-timeline-query.h"
#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GN_TYPE_NOSTR_EVENT_MODEL (gn_nostr_event_model_get_type())
G_DECLARE_FINAL_TYPE(GnNostrEventModel, gn_nostr_event_model, GN, NOSTR_EVENT_MODEL, GObject)

/*
 * GnNostrEventModel
 *
 * Subscription-driven GListModel over nostrdb note keys.
 *
 * Key behaviors:
 * - Maintains lifetime subscriptions to kinds {0,1,5,6} via gn-ndb-sub-dispatcher.
 * - Exposes a windowed list of kind {1,6} notes whose authors have kind 0 metadata in DB.
 * - Emits "need-profile" (string pubkey_hex) when a kind {1,6} arrives without a kind 0 profile in DB.
 *
 * Note: The "need-profile" signal is registered on the GObject type; consumers should connect via
 *   g_signal_connect(model, "need-profile", ...).
 */

typedef struct {
    gint *kinds;
    gsize n_kinds;
    char **authors;
    gsize n_authors;
    gint64 since;
    gint64 until;
    guint limit;
} GnNostrQueryParams;

GnNostrEventModel *gn_nostr_event_model_new(void);

/* New API using GnTimelineQuery (preferred) */
GnNostrEventModel *gn_nostr_event_model_new_with_query(GnTimelineQuery *query);
void gn_nostr_event_model_set_timeline_query(GnNostrEventModel *self, GnTimelineQuery *query);
GnTimelineQuery *gn_nostr_event_model_get_timeline_query(GnNostrEventModel *self);

/* Legacy API using GnNostrQueryParams (deprecated, use GnTimelineQuery instead) */
void gn_nostr_event_model_set_query(GnNostrEventModel *self, const GnNostrQueryParams *params);
void gn_nostr_event_model_set_thread_root(GnNostrEventModel *self, const char *root_event_id);
void gn_nostr_event_model_refresh(GnNostrEventModel *self);
void gn_nostr_event_model_refresh_async(GnNostrEventModel *self);
void gn_nostr_event_model_clear(GnNostrEventModel *self);
void gn_nostr_event_model_update_profile(GObject *model, const char *pubkey_hex, const char *content_json);
void gn_nostr_event_model_check_pending_for_profile(GnNostrEventModel *self, const char *pubkey);

/* Compatibility APIs (deprecated by subscription-driven updates):
 * - add_event_json/add_live_event are kept for build compatibility, but normal operation should ingest
 *   into storage_ndb and rely on subscriptions for UI updates.
 */
void gn_nostr_event_model_add_event_json(GnNostrEventModel *self, const char *event_json);
void gn_nostr_event_model_add_live_event(GnNostrEventModel *self, void *nostr_event);

gboolean gn_nostr_event_model_get_is_thread_view(GnNostrEventModel *self);
const char *gn_nostr_event_model_get_root_event_id(GnNostrEventModel *self);

/* Sliding window pagination: load older events before the current oldest.
 * Returns the number of events added. */
guint gn_nostr_event_model_load_older(GnNostrEventModel *self, guint count);

/* Sliding window pagination: load newer events after the current newest.
 * Returns the number of events added. */
guint gn_nostr_event_model_load_newer(GnNostrEventModel *self, guint count);

/* Async sliding window pagination: NDB query + parse on worker thread.
 * max_items: trim model to this size after load (0 = no trim). */
void gn_nostr_event_model_load_older_async(GnNostrEventModel *self, guint count, guint max_items);
void gn_nostr_event_model_load_newer_async(GnNostrEventModel *self, guint count, guint max_items);
gboolean gn_nostr_event_model_is_async_loading(GnNostrEventModel *self);

/* Get the timestamp of the oldest event in the model (0 if empty). */
gint64 gn_nostr_event_model_get_oldest_timestamp(GnNostrEventModel *self);

/* Get the timestamp of the newest event in the model (0 if empty). */
gint64 gn_nostr_event_model_get_newest_timestamp(GnNostrEventModel *self);

/* Trim newer events from the top of the model to keep memory bounded.
 * Called automatically when scrolling down and loading older events. */
void gn_nostr_event_model_trim_newer(GnNostrEventModel *self, guint keep_count);

/* Trim older events from the bottom of the model to keep memory bounded.
 * Called automatically when scrolling up and loading newer events. */
void gn_nostr_event_model_trim_older(GnNostrEventModel *self, guint keep_count);

/* nostrc-7o7: Update visible range for animation skip tracking.
 * Items added outside the visible range will skip their fade-in animation. */
void gn_nostr_event_model_set_visible_range(GnNostrEventModel *self, guint start, guint end);

/* nostrc-yi2: Calm timeline - scroll position awareness.
 * When user is scrolled down reading, new items are deferred to avoid jarring auto-scroll.
 * A "N new notes" indicator is shown instead; clicking it flushes deferred items. */
void gn_nostr_event_model_set_user_at_top(GnNostrEventModel *self, gboolean at_top);
guint gn_nostr_event_model_get_pending_count(GnNostrEventModel *self);
void gn_nostr_event_model_flush_pending(GnNostrEventModel *self);

/* Associate a widget for frame-synced insertion buffer drain (tick callback).
 * Typically the GtkListView or its parent timeline view. */
void gn_nostr_event_model_set_view_widget(GnNostrEventModel *self, GtkWidget *widget);

G_END_DECLS

#endif
