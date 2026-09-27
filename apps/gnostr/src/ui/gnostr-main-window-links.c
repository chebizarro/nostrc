/*
 * gnostr-main-window-links.c — open nostr: links and events handed over by
 * nostr-dispatcher (nostrc-prqu.3).
 *
 * Entry points:
 *   gnostr_main_window_open_nostr_uri()   GApplication::open (`gnostr nostr:…`)
 *   gnostr_main_window_open_nostr_event() org.nostr.Handler1.OpenEvent
 *   note cards (nostrc-46h7)              every NostrGtkNoteCardRow's
 *                                         "open-nostr-target" (event and
 *                                         address links in note content;
 *                                         npub/nprofile use "open-profile")
 *
 * The kind -> view table and all validation live in
 * util/gnostr-nostr-target.c. Every event shown here has passed id and
 * signature verification and matches the link it was fetched for, so a
 * relay cannot substitute another event.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define G_LOG_DOMAIN "gnostr-links"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-article-reader.h"
#include "../util/gnostr-nostr-target.h"
#include "../util/utils.h"

#include <nostr-gtk-1.0/gnostr-thread-view.h>
#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-filter.h>
#include <nostr-json.h>
#include <glib/gi18n.h>
#include <string.h>

/* Relay hints taken from a link, as nostr-dispatcher does. */
#define MAX_HINT_RELAYS 3

/* ---- views ------------------------------------------------------------ */

static void
show_thread(GnostrMainWindow *self, const char *event_id_hex, const char *event_json)
{
  GtkWidget *tv = self->session_view
    ? gnostr_session_view_get_thread_view(self->session_view) : NULL;
  if (!tv || !NOSTR_GTK_IS_THREAD_VIEW(tv)) {
    gnostr_main_window_show_toast_internal(self, _("Thread view not available"));
    return;
  }
  if (event_json)
    nostr_gtk_thread_view_set_focus_event_with_json(NOSTR_GTK_THREAD_VIEW(tv),
                                                    event_id_hex, event_json);
  else
    nostr_gtk_thread_view_set_focus_event(NOSTR_GTK_THREAD_VIEW(tv), event_id_hex, NULL);
  gnostr_main_window_show_thread_panel_internal(self);
}

static void
show_messages(GnostrMainWindow *self)
{
  /* Never render a gift wrap or rumor from a link: the inbox decrypts
   * through the signer and shows the conversation. */
  if (self->session_view)
    gnostr_session_view_show_page(self->session_view, "messages");
}

/* Route a validated event. */
static void
route_event(GnostrMainWindow *self, const GnostrNostrEventInfo *info, const char *event_json)
{
  GnostrNostrView view = gnostr_nostr_view_for_kind(info->kind);
  g_debug("route event %.16s kind %d -> view %d", info->id_hex, info->kind, view);
  switch (view) {
    case GNOSTR_NOSTR_VIEW_PROFILE:
      gnostr_main_window_open_profile(GTK_WIDGET(self), info->pubkey_hex);
      break;
    case GNOSTR_NOSTR_VIEW_MESSAGES:
      show_messages(self);
      break;
    case GNOSTR_NOSTR_VIEW_ARTICLE: {
      GtkWidget *reader = self->session_view
        ? gnostr_session_view_get_article_reader(self->session_view) : NULL;
      if (reader && GNOSTR_IS_ARTICLE_READER(reader)) {
        gnostr_article_reader_load_event_json(GNOSTR_ARTICLE_READER(reader),
                                              info->id_hex, event_json);
        gnostr_main_window_show_article_panel_internal(self);
      } else {
        show_thread(self, info->id_hex, event_json);
      }
      break;
    }
    case GNOSTR_NOSTR_VIEW_THREAD:
    default:
      show_thread(self, info->id_hex, event_json);
      break;
  }
}

/* Keep a validated event that came from outside the local store. */
static void
remember_event(const char *event_json)
{
  if (storage_ndb_ingest_event_json(event_json, NULL) != 0)
    g_debug("could not store linked event locally");
}

/* ---- lookups ------------------------------------------------------------ */

static NostrFilter *
target_filter(const GnostrNostrTarget *t)
{
  NostrFilter *f = nostr_filter_new();
  if (t->type == GNOSTR_NOSTR_TARGET_EVENT) {
    const char *ids[1] = { t->event_id_hex };
    nostr_filter_set_ids(f, ids, 1);
    nostr_filter_set_limit(f, 1);
  } else {
    int kinds[1] = { t->kind };
    const char *authors[1] = { t->pubkey_hex };
    nostr_filter_set_kinds(f, kinds, 1);
    nostr_filter_set_authors(f, authors, 1);
    nostr_filter_tags_append(f, "d", t->d_tag ? t->d_tag : "", NULL);
  }
  return f;
}

/* Newest matching event in the local store, validated. */
static char *
lookup_local(const GnostrNostrTarget *t)
{
  if (t->type == GNOSTR_NOSTR_TARGET_EVENT) {
    char *json = NULL;
    int len = 0;
    if (storage_ndb_get_note_by_id_nontxn(t->event_id_hex, &json, &len) != 0 || !json)
      return NULL;
    const char *one[1] = { json };
    char *picked = gnostr_nostr_pick_event_for_target(t, one, 1);
    free(json);
    return picked;
  }

  NostrFilter *f = target_filter(t);
  g_autofree char *filter = nostr_filter_serialize(f);
  nostr_filter_free(f);
  if (!filter)
    return NULL;
  g_autofree char *filters = g_strdup_printf("[%s]", filter);
  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn)
    return NULL;
  char **arr = NULL;
  int n = 0;
  char *picked = NULL;
  if (storage_ndb_query(txn, filters, &arr, &n, NULL) == 0 && arr && n > 0)
    picked = gnostr_nostr_pick_event_for_target(t, (const char *const *)arr, (gsize)n);
  storage_ndb_free_results(arr, n);
  storage_ndb_end_query(txn);
  return picked;
}

static gboolean
relay_url_usable(const char *url)
{
  if (!url)
    return FALSE;
  if (g_str_has_prefix(url, "wss://"))
    return TRUE;
  /* ws:// only to loopback, as nostr-dispatcher allows. */
  return g_str_has_prefix(url, "ws://localhost") || g_str_has_prefix(url, "ws://127.") ||
         g_str_has_prefix(url, "ws://[::1]");
}

typedef struct {
  GWeakRef window;
  GnostrNostrTarget *target;
} FetchCtx;

static void
fetch_ctx_free(FetchCtx *ctx)
{
  g_weak_ref_clear(&ctx->window);
  gnostr_nostr_target_free(ctx->target);
  g_free(ctx);
}

static void
on_link_fetch_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
  FetchCtx *ctx = user_data;
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) events = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);
  g_autoptr(GnostrMainWindow) self = g_weak_ref_get(&ctx->window);

  g_autofree char *json = NULL;
  if (events && events->len > 0)
    json = gnostr_nostr_pick_event_for_target(ctx->target,
                                              (const char *const *)events->pdata, events->len);
  g_autoptr(GnostrNostrEventInfo) info = json ? gnostr_nostr_event_parse(json, NULL) : NULL;

  if (self && info) {
    remember_event(json);
    route_event(self, info, json);
  } else if (self && ctx->target->type == GNOSTR_NOSTR_TARGET_EVENT) {
    /* Not found yet: the thread view keeps looking by id on its own. */
    show_thread(self, ctx->target->event_id_hex, NULL);
  } else if (self) {
    g_debug("naddr fetch failed: %s", error ? error->message : "not found");
    gnostr_main_window_show_toast_internal(self, _("Could not find the linked item on any relay"));
  }
  fetch_ctx_free(ctx);
}

/* Look the target up on its relay hints plus the user's relays. */
static void
fetch_from_relays(GnostrMainWindow *self, GnostrNostrTarget *target /* transfer full */)
{
  GNostrPool *pool = gnostr_get_shared_query_pool();
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  guint hints = 0;
  for (gsize i = 0; target->relays[i] && hints < MAX_HINT_RELAYS; i++) {
    if (relay_url_usable(target->relays[i])) {
      g_ptr_array_add(urls, g_strdup(target->relays[i]));
      hints++;
    }
  }
  gnostr_load_relays_into(urls);
  if (!pool || urls->len == 0) {
    if (target->type == GNOSTR_NOSTR_TARGET_EVENT)
      show_thread(self, target->event_id_hex, NULL);
    else
      gnostr_main_window_show_toast_internal(self, _("No relays configured to look up this link"));
    gnostr_nostr_target_free(target);
    return;
  }

  FetchCtx *ctx = g_new0(FetchCtx, 1);
  g_weak_ref_init(&ctx->window, self);
  ctx->target = target;
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *f = target_filter(target);
  nostr_filters_add(filters, f); /* moves the contents */
  nostr_filter_free(f);
  gnostr_pool_query_urls_async(pool, (const gchar **)urls->pdata, urls->len, filters,
                               NULL, on_link_fetch_done, ctx);
}

/* ---- note cards (nostrc-46h7) ------------------------------------------ */

/* The window a card belongs to, or the application's main window when the
 * card sits in another toplevel (a dialog, a popover's surface). */
static GnostrMainWindow *
main_window_for(GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root(widget);
  if (GNOSTR_IS_MAIN_WINDOW(root))
    return GNOSTR_MAIN_WINDOW(root);
  GApplication *app = g_application_get_default();
  if (!GTK_IS_APPLICATION(app))
    return NULL;
  GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (GNOSTR_IS_MAIN_WINDOW(active))
    return GNOSTR_MAIN_WINDOW(active);
  for (GList *l = gtk_application_get_windows(GTK_APPLICATION(app)); l; l = l->next)
    if (GNOSTR_IS_MAIN_WINDOW(l->data))
      return GNOSTR_MAIN_WINDOW(l->data);
  return NULL;
}

/* Cards are created in many views (timeline, search, communities, repos,
 * and nostr-gtk's thread view and profile pane), and none of them handled
 * this signal. One emission hook routes them all; views must therefore not
 * also connect "open-nostr-target" to open the link themselves. */
static gboolean
on_note_card_open_nostr_target(GSignalInvocationHint *hint, guint n_params,
                               const GValue *params, gpointer data)
{
  (void)hint;
  (void)data;
  if (n_params < 2)
    return TRUE;
  GObject *row = g_value_get_object(&params[0]);
  const char *target = g_value_get_string(&params[1]);
  if (!GTK_IS_WIDGET(row) || !target || !*target)
    return TRUE;
  GnostrMainWindow *win = main_window_for(GTK_WIDGET(row));
  if (!win)
    return TRUE;
  /* Content links may be bare bech32 ("note1…"); the router takes NIP-21. */
  gboolean has_scheme = g_ascii_strncasecmp(target, "nostr:", 6) == 0 ||
                        g_ascii_strncasecmp(target, "web+nostr:", 10) == 0;
  g_autofree char *uri = has_scheme ? g_strdup(target) : g_strconcat("nostr:", target, NULL);
  gnostr_main_window_open_nostr_uri(win, uri);
  return TRUE; /* stay installed */
}

void
gnostr_main_window_links_install_note_hook_internal(void)
{
  static gsize once = 0;
  if (!g_once_init_enter(&once))
    return;
  /* The signal is created in the class's class_init: keep it loaded. */
  g_type_class_ref(NOSTR_GTK_TYPE_NOTE_CARD_ROW);
  guint id = g_signal_lookup("open-nostr-target", NOSTR_GTK_TYPE_NOTE_CARD_ROW);
  if (id)
    g_signal_add_emission_hook(id, 0, on_note_card_open_nostr_target, NULL, NULL);
  else
    g_warning("NostrGtkNoteCardRow has no open-nostr-target signal; note links stay inert");
  g_once_init_leave(&once, 1);
}

/* ---- public ------------------------------------------------------------ */

void
gnostr_main_window_open_nostr_uri(GnostrMainWindow *self, const char *uri)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  g_autoptr(GError) error = NULL;
  GnostrNostrTarget *target = gnostr_nostr_target_parse(uri, &error);
  if (!target) {
    g_message("not opening %s: %s", uri ? uri : "(null)", error->message);
    gnostr_main_window_show_toast_internal(self, error->message);
    return;
  }

  g_debug("open %s link (kind %d)",
          target->type == GNOSTR_NOSTR_TARGET_PROFILE ? "profile" :
          target->type == GNOSTR_NOSTR_TARGET_EVENT ? "event" : "address", target->kind);

  if (target->type == GNOSTR_NOSTR_TARGET_PROFILE) {
    gnostr_main_window_open_profile(GTK_WIDGET(self), target->pubkey_hex);
    gnostr_nostr_target_free(target);
    return;
  }

  /* The link's kind decides the view without a lookup, except where the
   * view needs the event itself (articles). */
  if (target->type == GNOSTR_NOSTR_TARGET_EVENT && target->kind >= 0) {
    GnostrNostrView view = gnostr_nostr_view_for_kind(target->kind);
    if (view == GNOSTR_NOSTR_VIEW_MESSAGES) {
      show_messages(self);
      gnostr_nostr_target_free(target);
      return;
    }
    if (view == GNOSTR_NOSTR_VIEW_THREAD) {
      show_thread(self, target->event_id_hex, NULL);
      gnostr_nostr_target_free(target);
      return;
    }
  }

  g_autofree char *json = lookup_local(target);
  g_autoptr(GnostrNostrEventInfo) info = json ? gnostr_nostr_event_parse(json, NULL) : NULL;
  if (info) {
    route_event(self, info, json);
    gnostr_nostr_target_free(target);
    return;
  }
  fetch_from_relays(self, target);
}

gboolean
gnostr_main_window_open_nostr_event(GnostrMainWindow *self,
                                    guint kind,
                                    const char *event_json,
                                    const char *const *relays,
                                    GError **error)
{
  (void)relays; /* hints for follow-up queries; the views use the user's relays */
  g_return_val_if_fail(GNOSTR_IS_MAIN_WINDOW(self), FALSE);

  /* The dispatcher validated the event, but it is still untrusted input
   * from any session-bus peer: check it again. It is shown from this JSON
   * and not stored: an unsolicited event must not seed later local lookups. */
  g_autoptr(GnostrNostrEventInfo) info = gnostr_nostr_event_parse(event_json, error);
  if (!info)
    return FALSE;
  if ((guint)info->kind != kind) {
    g_set_error(error, GNOSTR_NOSTR_TARGET_ERROR, GNOSTR_NOSTR_TARGET_ERROR_INVALID,
                "Event kind %d does not match the declared kind %u", info->kind, kind);
    return FALSE;
  }
  route_event(self, info, event_json);
  return TRUE;
}
