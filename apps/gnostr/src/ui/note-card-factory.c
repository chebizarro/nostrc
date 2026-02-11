/* note-card-factory.c
 *
 * Unified factory for NoteCardRow creation and lifecycle management.
 * nostrc-o7pp: Ensures consistent bind/unbind handling across all views.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "note-card-factory.h"
#include "note_card_row.h"
#include "../model/gn-nostr-event-item.h"

struct _NoteCardFactory {
  GObject parent_instance;

  GtkSignalListItemFactory *gtk_factory;
  NoteCardBindFlags bind_flags;
  NoteCardFactorySignalFlags signal_flags;
  gpointer user_data;

  /* Custom bind callback (optional - if set, used instead of default binding) */
  NoteCardBindCallback bind_cb;
  gpointer bind_cb_data;

  /* Custom signal handlers */
  GCallback open_profile_cb;
  gpointer open_profile_data;
  GCallback view_thread_cb;
  gpointer view_thread_data;
  GCallback reply_cb;
  gpointer reply_data;
  GCallback search_hashtag_cb;
  gpointer search_hashtag_data;
  GCallback repost_cb;
  gpointer repost_data;
  GCallback quote_cb;
  gpointer quote_data;
  GCallback like_cb;
  gpointer like_data;
  GCallback zap_cb;
  gpointer zap_data;
  GCallback mute_user_cb;
  gpointer mute_user_data;
  GCallback mute_thread_cb;
  gpointer mute_thread_data;
  GCallback bookmark_cb;
  gpointer bookmark_data;
  GCallback pin_cb;
  gpointer pin_data;
  GCallback delete_cb;
  gpointer delete_data;
  GCallback navigate_cb;
  gpointer navigate_data;
};

G_DEFINE_TYPE(NoteCardFactory, note_card_factory, G_TYPE_OBJECT)

/* Forward declarations */
static void factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);
static void factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);
static void factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);
static void factory_teardown_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data);

/* Signal relay handlers - relay signals from NoteCardRow to custom callbacks */
static void on_open_profile_relay(GnostrNoteCardRow *row, const char *pubkey, gpointer user_data);
static void on_view_thread_relay(GnostrNoteCardRow *row, const char *root_id, gpointer user_data);
static void on_reply_requested_relay(GnostrNoteCardRow *row, const char *id, const char *root, const char *pubkey, gpointer user_data);
static void on_search_hashtag_relay(GnostrNoteCardRow *row, const char *hashtag, gpointer user_data);
static void on_repost_relay(GnostrNoteCardRow *row, const char *id, const char *json, gpointer user_data);
static void on_quote_relay(GnostrNoteCardRow *row, const char *id, const char *content, gpointer user_data);
static void on_like_relay(GnostrNoteCardRow *row, const char *id, const char *pubkey, gint kind, const char *reaction, gpointer user_data);
static void on_zap_relay(GnostrNoteCardRow *row, const char *id, const char *pubkey, const char *lud16, gpointer user_data);
static void on_mute_user_relay(GnostrNoteCardRow *row, const char *pubkey, gpointer user_data);
static void on_mute_thread_relay(GnostrNoteCardRow *row, const char *root_id, gpointer user_data);
static void on_bookmark_relay(GnostrNoteCardRow *row, const char *id, gboolean bookmarked, gpointer user_data);
static void on_pin_relay(GnostrNoteCardRow *row, const char *id, gboolean pinned, gpointer user_data);
static void on_delete_relay(GnostrNoteCardRow *row, const char *id, const char *pubkey, gpointer user_data);
static void on_navigate_relay(GnostrNoteCardRow *row, const char *note_id, gpointer user_data);

static void
note_card_factory_dispose(GObject *object)
{
  NoteCardFactory *self = NOTE_CARD_FACTORY(object);

  g_clear_object(&self->gtk_factory);

  G_OBJECT_CLASS(note_card_factory_parent_class)->dispose(object);
}

static void
note_card_factory_class_init(NoteCardFactoryClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = note_card_factory_dispose;
}

static void
note_card_factory_init(NoteCardFactory *self)
{
  self->gtk_factory = GTK_SIGNAL_LIST_ITEM_FACTORY(gtk_signal_list_item_factory_new());
  self->bind_flags = NOTE_CARD_BIND_BASIC;
  self->signal_flags = NOTE_CARD_SIGNAL_NONE;
  self->user_data = NULL;

  g_signal_connect(self->gtk_factory, "setup", G_CALLBACK(factory_setup_cb), self);
  g_signal_connect(self->gtk_factory, "bind", G_CALLBACK(factory_bind_cb), self);
  g_signal_connect(self->gtk_factory, "unbind", G_CALLBACK(factory_unbind_cb), self);
  g_signal_connect(self->gtk_factory, "teardown", G_CALLBACK(factory_teardown_cb), self);
}

NoteCardFactory *
note_card_factory_new(NoteCardBindFlags bind_flags,
                      NoteCardFactorySignalFlags signal_flags)
{
  NoteCardFactory *self = g_object_new(NOTE_CARD_TYPE_FACTORY, NULL);
  self->bind_flags = bind_flags;
  self->signal_flags = signal_flags;
  return self;
}

GtkListItemFactory *
note_card_factory_get_gtk_factory(NoteCardFactory *self)
{
  g_return_val_if_fail(NOTE_CARD_IS_FACTORY(self), NULL);
  return GTK_LIST_ITEM_FACTORY(self->gtk_factory);
}

void
note_card_factory_set_user_data(NoteCardFactory *self, gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->user_data = user_data;
}

void
note_card_factory_connect_open_profile(NoteCardFactory *self,
                                       GCallback callback,
                                       gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->open_profile_cb = callback;
  self->open_profile_data = user_data;
}

void
note_card_factory_connect_view_thread(NoteCardFactory *self,
                                      GCallback callback,
                                      gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->view_thread_cb = callback;
  self->view_thread_data = user_data;
}

void
note_card_factory_connect_reply(NoteCardFactory *self,
                                GCallback callback,
                                gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->reply_cb = callback;
  self->reply_data = user_data;
}

void
note_card_factory_connect_search_hashtag(NoteCardFactory *self,
                                         GCallback callback,
                                         gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->search_hashtag_cb = callback;
  self->search_hashtag_data = user_data;
}

void
note_card_factory_connect_repost(NoteCardFactory *self,
                                  GCallback callback,
                                  gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->repost_cb = callback;
  self->repost_data = user_data;
}

void
note_card_factory_connect_quote(NoteCardFactory *self,
                                 GCallback callback,
                                 gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->quote_cb = callback;
  self->quote_data = user_data;
}

void
note_card_factory_connect_like(NoteCardFactory *self,
                                GCallback callback,
                                gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->like_cb = callback;
  self->like_data = user_data;
}

void
note_card_factory_connect_zap(NoteCardFactory *self,
                               GCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->zap_cb = callback;
  self->zap_data = user_data;
}

void
note_card_factory_connect_mute_user(NoteCardFactory *self,
                                     GCallback callback,
                                     gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->mute_user_cb = callback;
  self->mute_user_data = user_data;
}

void
note_card_factory_connect_mute_thread(NoteCardFactory *self,
                                       GCallback callback,
                                       gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->mute_thread_cb = callback;
  self->mute_thread_data = user_data;
}

void
note_card_factory_connect_bookmark(NoteCardFactory *self,
                                    GCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->bookmark_cb = callback;
  self->bookmark_data = user_data;
}

void
note_card_factory_connect_pin(NoteCardFactory *self,
                               GCallback callback,
                               gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->pin_cb = callback;
  self->pin_data = user_data;
}

void
note_card_factory_connect_delete(NoteCardFactory *self,
                                  GCallback callback,
                                  gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->delete_cb = callback;
  self->delete_data = user_data;
}

void
note_card_factory_connect_navigate(NoteCardFactory *self,
                                    GCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->navigate_cb = callback;
  self->navigate_data = user_data;
}

void
note_card_factory_set_bind_callback(NoteCardFactory *self,
                                    NoteCardBindCallback callback,
                                    gpointer user_data)
{
  g_return_if_fail(NOTE_CARD_IS_FACTORY(self));
  self->bind_cb = callback;
  self->bind_cb_data = user_data;
}

/* ============================================================================
 * Factory Callbacks
 * ============================================================================ */

static void
factory_setup_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  (void)f;
  NoteCardFactory *self = NOTE_CARD_FACTORY(data);
  GtkWidget *row = GTK_WIDGET(gnostr_note_card_row_new());

  /* Connect signals based on signal_flags */
  if (self->signal_flags & NOTE_CARD_SIGNAL_OPEN_PROFILE) {
    g_signal_connect(row, "open-profile", G_CALLBACK(on_open_profile_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_VIEW_THREAD) {
    g_signal_connect(row, "view-thread-requested", G_CALLBACK(on_view_thread_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_REPLY) {
    g_signal_connect(row, "reply-requested", G_CALLBACK(on_reply_requested_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_HASHTAG) {
    g_signal_connect(row, "search-hashtag", G_CALLBACK(on_search_hashtag_relay), self);
  }

  if (self->signal_flags & NOTE_CARD_SIGNAL_REPOST) {
    g_signal_connect(row, "repost-requested", G_CALLBACK(on_repost_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_QUOTE) {
    g_signal_connect(row, "quote-requested", G_CALLBACK(on_quote_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_LIKE) {
    g_signal_connect(row, "like-requested", G_CALLBACK(on_like_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_ZAP) {
    g_signal_connect(row, "zap-requested", G_CALLBACK(on_zap_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_MUTE) {
    g_signal_connect(row, "mute-user-requested", G_CALLBACK(on_mute_user_relay), self);
    g_signal_connect(row, "mute-thread-requested", G_CALLBACK(on_mute_thread_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_BOOKMARK) {
    g_signal_connect(row, "bookmark-toggled", G_CALLBACK(on_bookmark_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_PIN) {
    g_signal_connect(row, "pin-toggled", G_CALLBACK(on_pin_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_DELETE) {
    g_signal_connect(row, "delete-note-requested", G_CALLBACK(on_delete_relay), self);
  }
  if (self->signal_flags & NOTE_CARD_SIGNAL_NAVIGATE) {
    g_signal_connect(row, "navigate-to-note", G_CALLBACK(on_navigate_relay), self);
  }

  gtk_list_item_set_child(item, row);
}

/* Callback for when profile property changes on a bound item.
 * nostrc-NEW: Updates card when profile is fetched asynchronously. */
static void
on_item_profile_changed(GObject *obj, GParamSpec *pspec, gpointer user_data)
{
  (void)pspec;
  GtkWidget *row = GTK_WIDGET(user_data);

  if (!GNOSTR_IS_NOTE_CARD_ROW(row)) return;
  if (gnostr_note_card_row_is_disposed(GNOSTR_NOTE_CARD_ROW(row))) return;

  /* Get updated profile from model item */
  GObject *profile = NULL;
  g_object_get(obj, "profile", &profile, NULL);

  if (profile) {
    gchar *display_name = NULL, *handle = NULL, *avatar_url = NULL, *nip05 = NULL;
    g_object_get(profile,
                 "display-name", &display_name,
                 "name", &handle,
                 "picture-url", &avatar_url,
                 "nip05", &nip05,
                 NULL);
    g_object_unref(profile);

    /* Update card with new profile data */
    gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row),
                                     display_name, handle, avatar_url);

    /* Update NIP-05 if available */
    if (nip05) {
      gchar *pubkey = NULL;
      g_object_get(obj, "pubkey", &pubkey, NULL);
      if (pubkey) {
        gnostr_note_card_row_set_nip05(GNOSTR_NOTE_CARD_ROW(row), nip05, pubkey);
        g_free(pubkey);
      }
    }

    g_free(display_name);
    g_free(handle);
    g_free(avatar_url);
    g_free(nip05);
  }
}

static void
factory_bind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  (void)f;
  NoteCardFactory *self = NOTE_CARD_FACTORY(data);
  GObject *obj = gtk_list_item_get_item(item);

  if (!obj) return;

  GtkWidget *row = gtk_list_item_get_child(item);
  if (!GNOSTR_IS_NOTE_CARD_ROW(row)) return;

  /* CRITICAL: Prepare row for binding - resets disposed flag, assigns binding_id,
   * and creates fresh cancellable. Must be called BEFORE populating the row. */
  gnostr_note_card_row_prepare_for_bind(GNOSTR_NOTE_CARD_ROW(row));

  /* nostrc-NEW: Connect to profile changes for async profile updates.
   * Store signal handler ID on the row for disconnection during unbind. */
  gulong handler_id = g_signal_connect(obj, "notify::profile",
                                        G_CALLBACK(on_item_profile_changed), row);
  g_object_set_data(G_OBJECT(row), "profile-handler-id", GUINT_TO_POINTER(handler_id));
  g_object_set_data(G_OBJECT(row), "bound-item", obj);

  /* If custom bind callback is set, use it instead of default binding */
  if (self->bind_cb) {
    self->bind_cb(GNOSTR_NOTE_CARD_ROW(row), obj, self->bind_cb_data);
    gtk_widget_set_visible(row, TRUE);
    return;
  }

  /* Extract data from the model item */
  gchar *id_hex = NULL, *pubkey = NULL, *content = NULL;
  gchar *display_name = NULL, *handle = NULL, *avatar_url = NULL;
  gchar *root_id = NULL, *parent_id = NULL, *nip05 = NULL;
  gint64 created_at = 0;
  guint depth = 0;
  gboolean is_reply = FALSE;

  /* Check if this is a GnNostrEventItem */
  if (GN_IS_NOSTR_EVENT_ITEM(obj)) {
    GnNostrEventItem *event_item = GN_NOSTR_EVENT_ITEM(obj);

    g_object_get(obj,
                 "event-id", &id_hex,
                 "pubkey", &pubkey,
                 "created-at", &created_at,
                 "content", &content,
                 "thread-root-id", &root_id,
                 "parent-id", &parent_id,
                 "reply-depth", &depth,
                 "is-reply", &is_reply,
                 NULL);

    /* Get profile information */
    GObject *profile = NULL;
    g_object_get(obj, "profile", &profile, NULL);
    if (profile) {
      g_object_get(profile,
                   "display-name", &display_name,
                   "name", &handle,
                   "picture-url", &avatar_url,
                   "nip05", &nip05,
                   NULL);
      g_object_unref(profile);
    }
  }

  /* Fallback: try to get properties generically */
  if (!id_hex && G_IS_OBJECT(obj)) {
    GObjectClass *klass = G_OBJECT_GET_CLASS(obj);
    if (g_object_class_find_property(klass, "id"))
      g_object_get(obj, "id", &id_hex, NULL);
    if (g_object_class_find_property(klass, "pubkey"))
      g_object_get(obj, "pubkey", &pubkey, NULL);
    if (g_object_class_find_property(klass, "content"))
      g_object_get(obj, "content", &content, NULL);
    if (g_object_class_find_property(klass, "created-at"))
      g_object_get(obj, "created-at", &created_at, NULL);
    if (g_object_class_find_property(klass, "display-name"))
      g_object_get(obj, "display-name", &display_name, NULL);
    if (g_object_class_find_property(klass, "handle"))
      g_object_get(obj, "handle", &handle, NULL);
    if (g_object_class_find_property(klass, "avatar-url"))
      g_object_get(obj, "avatar-url", &avatar_url, NULL);
  }

  /* Use pubkey prefix as fallback display name */
  gchar *display_fallback = NULL;
  if (!display_name && !handle && pubkey && strlen(pubkey) >= 8) {
    display_fallback = g_strdup_printf("%.8s...", pubkey);
  }

  /* Basic binding (always performed) */
  gnostr_note_card_row_set_author(GNOSTR_NOTE_CARD_ROW(row),
                                   display_name ? display_name : display_fallback,
                                   handle, avatar_url);
  gnostr_note_card_row_set_timestamp(GNOSTR_NOTE_CARD_ROW(row), created_at, NULL);
  /* nostrc-dqwq.1: Use cached render result to avoid re-rendering on rebind */
  if (GN_IS_NOSTR_EVENT_ITEM(obj)) {
    const GnContentRenderResult *cached = gn_nostr_event_item_get_render_result(GN_NOSTR_EVENT_ITEM(obj));
    if (cached) {
      gnostr_note_card_row_set_content_rendered(GNOSTR_NOTE_CARD_ROW(row), content, cached);
    } else {
      gnostr_note_card_row_set_content(GNOSTR_NOTE_CARD_ROW(row), content);
    }
  } else {
    gnostr_note_card_row_set_content(GNOSTR_NOTE_CARD_ROW(row), content);
  }
  gnostr_note_card_row_set_ids(GNOSTR_NOTE_CARD_ROW(row), id_hex, root_id, pubkey);
  gnostr_note_card_row_set_depth(GNOSTR_NOTE_CARD_ROW(row), depth);

  /* Thread info (if enabled) */
  if (self->bind_flags & NOTE_CARD_BIND_THREAD_INFO) {
    gnostr_note_card_row_set_thread_info(GNOSTR_NOTE_CARD_ROW(row),
                                          root_id, parent_id, NULL, is_reply);
  }

  /* NIP-05 verification (if available) */
  if (nip05 && pubkey) {
    gnostr_note_card_row_set_nip05(GNOSTR_NOTE_CARD_ROW(row), nip05, pubkey);
  }

  /* Note: Login state should be set by the view's custom bind callback
   * since is_user_logged_in() is defined locally in each view file. */

  /* Cleanup */
  g_free(id_hex);
  g_free(pubkey);
  g_free(content);
  g_free(display_name);
  g_free(handle);
  g_free(avatar_url);
  g_free(root_id);
  g_free(parent_id);
  g_free(nip05);
  g_free(display_fallback);

  gtk_widget_set_visible(row, TRUE);
}

static void
factory_unbind_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  (void)f; (void)data;
  GtkWidget *row = gtk_list_item_get_child(item);

  /* nostrc-NEW: Disconnect profile change handler to prevent callbacks to stale row */
  if (row) {
    gulong handler_id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "profile-handler-id"));
    GObject *bound_item = g_object_get_data(G_OBJECT(row), "bound-item");
    if (handler_id > 0 && bound_item && G_IS_OBJECT(bound_item)) {
      g_signal_handler_disconnect(bound_item, handler_id);
    }
    g_object_set_data(G_OBJECT(row), "profile-handler-id", NULL);
    g_object_set_data(G_OBJECT(row), "bound-item", NULL);
  }

  /* CRITICAL: Prepare row for unbinding BEFORE GTK disposes it.
   * This cancels all async operations, clears binding_id, and sets disposed flag
   * to prevent callbacks from corrupting Pango state. nostrc-534d */
  if (GNOSTR_IS_NOTE_CARD_ROW(row)) {
    gnostr_note_card_row_prepare_for_unbind(GNOSTR_NOTE_CARD_ROW(row));
  }
}

static void
factory_teardown_cb(GtkSignalListItemFactory *f, GtkListItem *item, gpointer data)
{
  (void)f; (void)data;
  /* nostrc-5g2n: Call prepare_for_unbind as a safety net during teardown.
   * During g_list_store_remove_all, GTK may teardown rows whose unbind
   * already ran (prepare_for_unbind is idempotent via the disposed flag).
   * But if teardown fires without a prior unbind (edge case during rapid
   * model changes), this prevents Pango SEGV from uncleaned PangoLayouts. */
  GtkWidget *row = gtk_list_item_get_child(item);
  if (row && GNOSTR_IS_NOTE_CARD_ROW(row)) {
    gnostr_note_card_row_prepare_for_unbind(GNOSTR_NOTE_CARD_ROW(row));
  }
}

/* ============================================================================
 * Signal Relay Handlers
 * ============================================================================ */

static void
on_open_profile_relay(GnostrNoteCardRow *row, const char *pubkey, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->open_profile_cb) {
    ((void (*)(const char *, gpointer))self->open_profile_cb)(pubkey, self->open_profile_data);
  }
}

static void
on_view_thread_relay(GnostrNoteCardRow *row, const char *root_id, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->view_thread_cb) {
    ((void (*)(const char *, gpointer))self->view_thread_cb)(root_id, self->view_thread_data);
  }
}

static void
on_reply_requested_relay(GnostrNoteCardRow *row, const char *id, const char *root, const char *pubkey, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->reply_cb) {
    ((void (*)(const char *, const char *, const char *, gpointer))self->reply_cb)(id, root, pubkey, self->reply_data);
  }
}

static void
on_search_hashtag_relay(GnostrNoteCardRow *row, const char *hashtag, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->search_hashtag_cb) {
    ((void (*)(const char *, gpointer))self->search_hashtag_cb)(hashtag, self->search_hashtag_data);
  }
}

static void
on_repost_relay(GnostrNoteCardRow *row, const char *id, const char *json, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->repost_cb) {
    ((void (*)(const char *, const char *, gpointer))self->repost_cb)(id, json, self->repost_data);
  }
}

static void
on_quote_relay(GnostrNoteCardRow *row, const char *id, const char *content, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->quote_cb) {
    ((void (*)(const char *, const char *, gpointer))self->quote_cb)(id, content, self->quote_data);
  }
}

static void
on_like_relay(GnostrNoteCardRow *row, const char *id, const char *pubkey, gint kind, const char *reaction, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->like_cb) {
    ((void (*)(const char *, const char *, gint, const char *, gpointer))self->like_cb)(id, pubkey, kind, reaction, self->like_data);
  }
}

static void
on_zap_relay(GnostrNoteCardRow *row, const char *id, const char *pubkey, const char *lud16, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->zap_cb) {
    ((void (*)(const char *, const char *, const char *, gpointer))self->zap_cb)(id, pubkey, lud16, self->zap_data);
  }
}

static void
on_mute_user_relay(GnostrNoteCardRow *row, const char *pubkey, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->mute_user_cb) {
    ((void (*)(const char *, gpointer))self->mute_user_cb)(pubkey, self->mute_user_data);
  }
}

static void
on_mute_thread_relay(GnostrNoteCardRow *row, const char *root_id, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->mute_thread_cb) {
    ((void (*)(const char *, gpointer))self->mute_thread_cb)(root_id, self->mute_thread_data);
  }
}

static void
on_bookmark_relay(GnostrNoteCardRow *row, const char *id, gboolean bookmarked, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->bookmark_cb) {
    ((void (*)(const char *, gboolean, gpointer))self->bookmark_cb)(id, bookmarked, self->bookmark_data);
  }
}

static void
on_pin_relay(GnostrNoteCardRow *row, const char *id, gboolean pinned, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->pin_cb) {
    ((void (*)(const char *, gboolean, gpointer))self->pin_cb)(id, pinned, self->pin_data);
  }
}

static void
on_delete_relay(GnostrNoteCardRow *row, const char *id, const char *pubkey, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->delete_cb) {
    ((void (*)(const char *, const char *, gpointer))self->delete_cb)(id, pubkey, self->delete_data);
  }
}

static void
on_navigate_relay(GnostrNoteCardRow *row, const char *note_id, gpointer user_data)
{
  (void)row;
  NoteCardFactory *self = NOTE_CARD_FACTORY(user_data);
  if (self->navigate_cb) {
    ((void (*)(const char *, gpointer))self->navigate_cb)(note_id, self->navigate_data);
  }
}
