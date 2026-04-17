/**
 * gnostr-timeline-action-relay.c — Centralized action handler for timeline rows
 *
 * nostrc-hqtn: Moves 17 action-relay static functions out of
 * gnostr-timeline-view-app-factory.c into a named GObject.
 *
 * Each handler locates the main window at invocation time via
 * gtk_widget_get_ancestor(). If the row is not rooted (e.g. during
 * teardown), the handler silently no-ops.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gnostr-timeline-action-relay.h"
#include "gnostr-main-window.h"
#include "gnostr-zap-dialog.h"
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include "../util/bookmarks.h"
#include "../util/pin_list.h"
/* nip32_labels.h not needed — label handler delegates to main window */

/* ──────────────────────────── GObject boilerplate ──────────────────────────── */

struct _GnostrTimelineActionRelay {
  GObject parent_instance;
  /* No state — the relay discovers the main window at handler time. */
};

G_DEFINE_FINAL_TYPE(GnostrTimelineActionRelay,
                    gnostr_timeline_action_relay,
                    G_TYPE_OBJECT)

static void
gnostr_timeline_action_relay_class_init(GnostrTimelineActionRelayClass *klass)
{
  (void)klass;
}

static void
gnostr_timeline_action_relay_init(GnostrTimelineActionRelay *self)
{
  (void)self;
}

GnostrTimelineActionRelay *
gnostr_timeline_action_relay_new(void)
{
  return g_object_new(GNOSTR_TYPE_TIMELINE_ACTION_RELAY, NULL);
}

/* ──────────────────────────── Helper ──────────────────────────── */

/**
 * Find the GnostrMainWindow ancestor of @row.
 * Returns NULL if the row is not rooted or the ancestor is not a main window.
 */
static GnostrMainWindow *
find_main_window(NostrGtkNoteCardRow *row)
{
  GtkWidget *ancestor = gtk_widget_get_ancestor(GTK_WIDGET(row),
                                                 GNOSTR_TYPE_MAIN_WINDOW);
  return ancestor ? GNOSTR_MAIN_WINDOW(ancestor) : NULL;
}

/* ──────────────────── Action relay handlers ──────────────────── */

static void
on_relay_open_profile(NostrGtkNoteCardRow *row,
                      const char *pubkey_hex,
                      gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_open_profile(GTK_WIDGET(win), pubkey_hex);
}

static void
on_relay_reply_requested(NostrGtkNoteCardRow *row,
                         const char *id_hex,
                         const char *root_id,
                         const char *pubkey_hex,
                         gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_reply(GTK_WIDGET(win), id_hex, root_id, pubkey_hex);
}

static void
on_relay_repost_requested(NostrGtkNoteCardRow *row,
                          const char *id_hex,
                          const char *pubkey_hex,
                          gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_repost(GTK_WIDGET(win), id_hex, pubkey_hex);
}

static void
on_relay_quote_requested(NostrGtkNoteCardRow *row,
                         const char *id_hex,
                         const char *pubkey_hex,
                         gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_quote(GTK_WIDGET(win), id_hex, pubkey_hex);
}

static void
on_relay_like_requested(NostrGtkNoteCardRow *row,
                        const char *id_hex,
                        const char *pubkey_hex,
                        gint event_kind,
                        const char *reaction_content,
                        gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_like(GTK_WIDGET(win), id_hex, pubkey_hex,
                                     event_kind, reaction_content, row);
}

static void
on_relay_comment_requested(NostrGtkNoteCardRow *row,
                           const char *id_hex,
                           int kind,
                           const char *pubkey_hex,
                           gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_comment(GTK_WIDGET(win), id_hex, kind, pubkey_hex);
}

static void
on_relay_zap_requested(NostrGtkNoteCardRow *row,
                       const char *id_hex,
                       const char *pubkey_hex,
                       const char *lud16,
                       gpointer user_data)
{
  (void)user_data;

  if (!id_hex || !pubkey_hex) {
    g_warning("[TIMELINE] Zap requested but missing id or pubkey");
    return;
  }

  if (!lud16 || !*lud16) {
    g_message("[TIMELINE] Zap requested but user has no lightning address");
    return;
  }

  /* Find parent window for the dialog */
  GtkWidget *parent_w = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_WINDOW);
  GtkWindow *parent = parent_w ? GTK_WINDOW(parent_w) : NULL;

  GnostrZapDialog *dialog = gnostr_zap_dialog_new(parent);

  /* Look up display name from profile cache, fall back to npub prefix */
  g_autofree gchar *display_name = NULL;
  GnostrProfileMeta *profile = gnostr_profile_provider_get(pubkey_hex);
  if (profile) {
    if (profile->display_name && *profile->display_name) {
      display_name = g_strdup(profile->display_name);
    } else if (profile->name && *profile->name) {
      display_name = g_strdup(profile->name);
    }
    gnostr_profile_meta_free(profile);
  }

  if (!display_name && pubkey_hex) {
    g_autoptr(GNostrNip19) npub_n19 = gnostr_nip19_encode_npub(pubkey_hex, NULL);
    if (npub_n19) {
      const char *npub = gnostr_nip19_get_bech32(npub_n19);
      if (npub) {
        display_name = g_strdup_printf("%.12s...", npub);
      }
    }
  }

  gnostr_zap_dialog_set_recipient(dialog, pubkey_hex, display_name, lud16);
  gnostr_zap_dialog_set_event(dialog, id_hex, 1);

  GPtrArray *relay_arr = gnostr_get_write_relay_urls();
  const gchar **relay_strs = g_new0(const gchar *, relay_arr->len + 1);
  for (guint i = 0; i < relay_arr->len; i++) {
    relay_strs[i] = g_ptr_array_index(relay_arr, i);
  }
  gnostr_zap_dialog_set_relays(dialog, relay_strs);
  g_free(relay_strs);
  g_ptr_array_unref(relay_arr);

  gtk_window_present(GTK_WINDOW(dialog));
  g_message("[TIMELINE] Zap dialog opened for id=%s lud16=%s", id_hex, lud16);
}

static void
on_relay_view_thread_requested(NostrGtkNoteCardRow *row,
                               const char *root_event_id,
                               gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (!win) return;

  /* nostrc-a2zd: Fetch from nostrdb to avoid race condition. */
  char *event_json = NULL;
  int json_len = 0;
  storage_ndb_get_note_by_id_nontxn(root_event_id, &event_json, &json_len);
  gnostr_main_window_view_thread_with_json(GTK_WIDGET(win), root_event_id, event_json);
  if (event_json) free(event_json);
}

static void
on_relay_navigate_to_note(NostrGtkNoteCardRow *row,
                          const char *event_id,
                          gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (!win) return;

  /* nostrc-a2zd: Fetch from nostrdb to avoid race condition. */
  char *event_json = NULL;
  int json_len = 0;
  storage_ndb_get_note_by_id_nontxn(event_id, &event_json, &json_len);
  gnostr_main_window_view_thread_with_json(GTK_WIDGET(win), event_id, event_json);
  if (event_json) free(event_json);
}

static void
on_relay_mute_user_requested(NostrGtkNoteCardRow *row,
                             const char *pubkey_hex,
                             gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_mute_user(GTK_WIDGET(win), pubkey_hex);
}

static void
on_relay_mute_thread_requested(NostrGtkNoteCardRow *row,
                               const char *event_id_hex,
                               gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_mute_thread(GTK_WIDGET(win), event_id_hex);
}

static void
on_relay_show_toast(NostrGtkNoteCardRow *row,
                    const char *message,
                    gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_show_toast(GTK_WIDGET(win), message);
}

static void
on_relay_pin_toggled(NostrGtkNoteCardRow *row,
                     const char *event_id,
                     gboolean is_pinned,
                     gpointer user_data)
{
  (void)user_data;
  (void)row;

  if (!event_id || strlen(event_id) != 64) {
    g_warning("[PIN] Invalid event ID for pin toggle");
    return;
  }

  GnostrPinList *pin_list = gnostr_pin_list_get_default();
  if (!pin_list) {
    g_warning("[PIN] Failed to get pin list instance");
    return;
  }

  if (is_pinned) {
    gnostr_pin_list_add(pin_list, event_id, NULL);
  } else {
    gnostr_pin_list_remove(pin_list, event_id);
  }

  gnostr_pin_list_save_async(pin_list, NULL, NULL);
  g_message("[PIN] Pin %s for event %s", is_pinned ? "added" : "removed", event_id);
}

static void
on_relay_bookmark_toggled(NostrGtkNoteCardRow *row,
                          const char *event_id,
                          gboolean is_bookmarked,
                          gpointer user_data)
{
  (void)user_data;
  (void)row;

  if (!event_id || strlen(event_id) != 64) {
    g_warning("[BOOKMARK] Invalid event ID for bookmark toggle");
    return;
  }

  GnostrBookmarks *bookmarks = gnostr_bookmarks_get_default();
  if (!bookmarks) {
    g_warning("[BOOKMARK] Failed to get bookmarks instance");
    return;
  }

  if (is_bookmarked) {
    gnostr_bookmarks_add(bookmarks, event_id, NULL, FALSE);
  } else {
    gnostr_bookmarks_remove(bookmarks, event_id);
  }

  gnostr_bookmarks_save_async(bookmarks, NULL, NULL);
  g_message("[BOOKMARK] Bookmark %s for event %s",
            is_bookmarked ? "added" : "removed", event_id);
}

static void
on_relay_delete_note_requested(NostrGtkNoteCardRow *row,
                               const char *id_hex,
                               const char *pubkey_hex,
                               gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_delete_note(GTK_WIDGET(win), id_hex, pubkey_hex);
}

static void
on_relay_report_note_requested(NostrGtkNoteCardRow *row,
                               const char *id_hex,
                               const char *pubkey_hex,
                               gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_report_note(GTK_WIDGET(win), id_hex, pubkey_hex);
}

static void
on_relay_label_note_requested(NostrGtkNoteCardRow *row,
                              const char *id_hex,
                              const char *namespace,
                              const char *label,
                              const char *pubkey_hex,
                              gpointer user_data)
{
  (void)user_data;
  GnostrMainWindow *win = find_main_window(row);
  if (win)
    gnostr_main_window_request_label_note(GTK_WIDGET(win), id_hex, namespace,
                                           label, pubkey_hex);
}

/* ──────────────────────── connect_row ──────────────────────── */

void
gnostr_timeline_action_relay_connect_row(GnostrTimelineActionRelay *self,
                                          NostrGtkNoteCardRow       *row)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_ACTION_RELAY(self));
  g_return_if_fail(NOSTR_GTK_IS_NOTE_CARD_ROW(row));

  g_signal_connect(row, "open-profile",
                   G_CALLBACK(on_relay_open_profile), self);
  g_signal_connect(row, "reply-requested",
                   G_CALLBACK(on_relay_reply_requested), self);
  g_signal_connect(row, "repost-requested",
                   G_CALLBACK(on_relay_repost_requested), self);
  g_signal_connect(row, "quote-requested",
                   G_CALLBACK(on_relay_quote_requested), self);
  g_signal_connect(row, "like-requested",
                   G_CALLBACK(on_relay_like_requested), self);
  g_signal_connect(row, "comment-requested",
                   G_CALLBACK(on_relay_comment_requested), self);
  g_signal_connect(row, "zap-requested",
                   G_CALLBACK(on_relay_zap_requested), self);
  g_signal_connect(row, "view-thread-requested",
                   G_CALLBACK(on_relay_view_thread_requested), self);
  g_signal_connect(row, "navigate-to-note",
                   G_CALLBACK(on_relay_navigate_to_note), self);
  g_signal_connect(row, "mute-user-requested",
                   G_CALLBACK(on_relay_mute_user_requested), self);
  g_signal_connect(row, "mute-thread-requested",
                   G_CALLBACK(on_relay_mute_thread_requested), self);
  g_signal_connect(row, "show-toast",
                   G_CALLBACK(on_relay_show_toast), self);
  g_signal_connect(row, "pin-toggled",
                   G_CALLBACK(on_relay_pin_toggled), self);
  g_signal_connect(row, "bookmark-toggled",
                   G_CALLBACK(on_relay_bookmark_toggled), self);
  g_signal_connect(row, "delete-note-requested",
                   G_CALLBACK(on_relay_delete_note_requested), self);
  g_signal_connect(row, "report-note-requested",
                   G_CALLBACK(on_relay_report_note_requested), self);
  g_signal_connect(row, "label-note-requested",
                   G_CALLBACK(on_relay_label_note_requested), self);
}
