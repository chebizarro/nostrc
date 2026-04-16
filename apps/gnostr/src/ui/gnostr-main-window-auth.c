#define G_LOG_DOMAIN "gnostr-main-window-auth"

#include "gnostr-main-window-private.h"

#include "gnostr-session-view.h"
#include "gnostr-dm-service.h"
#include "gnostr-login.h"
#include "gnostr-notifications-view.h"
#include "../ipc/gnostr-signer-service.h"
#include "../notifications/badge_manager.h"
#include "../notifications/desktop_notify.h"
#include "../util/blossom_settings.h"
#include "../util/nip51_settings.h"
#include "../util/profile_event_validation.h"
#include "../sync/gnostr-sync-bridge.h"

#include <nostr-gobject-1.0/gn-ndb-sub-dispatcher.h>
#include <nostr-gobject-1.0/gnostr-relays.h>
#include <nostr-gobject-1.0/gnostr-sync-service.h>
#include <nostr-gobject-1.0/nostr_event.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <nostr-gobject-1.0/nostr_pool.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gtk-1.0/gnostr-profile-pane.h>

#include "nostr-filter.h"
#include "nostr-kinds.h"
#include "nostr/nip46/nip46_client.h"

#include <glib/gi18n.h>
#include <string.h>

static char *
client_settings_get_current_npub_local(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (!settings)
    return NULL;

  char *npub = g_settings_get_string(settings, "current-npub");
  if (npub && !*npub) {
    g_free(npub);
    return NULL;
  }

  return npub;
}

static const char *
desktop_notify_actor_name_local(const char *sender_name,
                                const char *sender_pubkey,
                                char        fallback_buf[32])
{
  if (sender_name && *sender_name)
    return sender_name;

  if (sender_pubkey && strlen(sender_pubkey) >= 8) {
    g_snprintf(fallback_buf, 32, "%.8s...", sender_pubkey);
    return fallback_buf;
  }

  return "Someone";
}

static void
dispatch_desktop_notification_local(GnostrMainWindow         *self,
                                    GnostrNotificationType    type,
                                    const char               *sender_pubkey,
                                    const char               *sender_name,
                                    const char               *content,
                                    const char               *event_id,
                                    const char               *target_note_id,
                                    guint64                   amount_sats)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  GnostrDesktopNotify *desktop = gnostr_desktop_notify_get_default();
  if (!desktop || !gnostr_desktop_notify_is_available())
    return;

  GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
  if (app)
    gnostr_desktop_notify_set_app(desktop, G_APPLICATION(app));

  gnostr_desktop_notify_request_permission(desktop);

  char fallback_name[32] = {0};
  const char *actor_name = desktop_notify_actor_name_local(sender_name, sender_pubkey, fallback_name);

  switch (type) {
    case GNOSTR_NOTIFICATION_DM:
      gnostr_desktop_notify_send_dm(desktop, actor_name, sender_pubkey, content, event_id, target_note_id);
      break;
    case GNOSTR_NOTIFICATION_MENTION:
      gnostr_desktop_notify_send_mention(desktop, actor_name, sender_pubkey, content, event_id, target_note_id);
      break;
    case GNOSTR_NOTIFICATION_REPLY:
      gnostr_desktop_notify_send_reply(desktop, actor_name, sender_pubkey, content, event_id, target_note_id);
      break;
    case GNOSTR_NOTIFICATION_ZAP:
      gnostr_desktop_notify_send_zap(desktop, actor_name, sender_pubkey, amount_sats, content, event_id, target_note_id);
      break;
    case GNOSTR_NOTIFICATION_REPOST:
      gnostr_desktop_notify_send_repost(desktop, actor_name, sender_pubkey, event_id, target_note_id);
      break;
    case GNOSTR_NOTIFICATION_REACTION: {
      g_autofree gchar *title = g_strdup_printf("%s reacted to your note", actor_name);
      gnostr_desktop_notify_send(desktop, type, title, NULL, event_id, target_note_id);
      break;
    }
    case GNOSTR_NOTIFICATION_LIST: {
      g_autofree gchar *title = g_strdup_printf("%s added you to a list", actor_name);
      gnostr_desktop_notify_send(desktop, type, title, NULL, event_id, target_note_id);
      break;
    }
    case GNOSTR_NOTIFICATION_FOLLOWER: {
      g_autofree gchar *title = g_strdup_printf("%s followed you", actor_name);
      gnostr_desktop_notify_send(desktop, type, title, NULL, event_id, target_note_id);
      break;
    }
    default:
      break;
  }
}

void
gnostr_main_window_update_login_ui_state_internal(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (!settings)
    return;

  char *npub = g_settings_get_string(settings, "current-npub");
  gboolean has_npub = (npub && *npub);
  gboolean signer_ready = gnostr_signer_service_is_ready(gnostr_signer_service_get_default());
  gboolean signed_in = has_npub && signer_ready;

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
    gnostr_session_view_set_authenticated(self->session_view, signed_in);

  g_free(npub);
}

char *
gnostr_main_window_get_current_user_pubkey_hex_internal(void)
{
  g_autofree char *npub = client_settings_get_current_npub_local();
  if (!npub)
    return NULL;

  if (strlen(npub) == 64 && !g_str_has_prefix(npub, "npub1")) {
    g_debug("[AUTH] current-npub setting contains raw hex pubkey, using directly");
    return g_strdup(npub);
  }

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
  if (!n19) {
    g_warning("[AUTH] Failed to decode current-npub to pubkey: %.16s...", npub);
    return NULL;
  }

  const char *hex = gnostr_nip19_get_pubkey(n19);
  if (!hex) {
    g_warning("[AUTH] gnostr_nip19_get_pubkey returned NULL for: %.16s...", npub);
    return NULL;
  }

  return g_strdup(hex);
}

static void
on_gift_wrap_batch(uint64_t subid, const uint64_t *note_keys, guint n_keys, gpointer user_data)
{
  (void)subid;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !note_keys || n_keys == 0)
    return;

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_warning("[GIFTWRAP] Failed to begin query transaction");
    return;
  }

  guint processed = 0;
  for (guint i = 0; i < n_keys; i++) {
    uint64_t note_key = note_keys[i];
    storage_ndb_note *note = storage_ndb_get_note_ptr(txn, note_key);
    if (!note)
      continue;

    uint32_t kind = storage_ndb_note_kind(note);
    if (kind != NOSTR_KIND_GIFT_WRAP)
      continue;

    const unsigned char *id32 = storage_ndb_note_id(note);
    if (!id32)
      continue;

    char id_hex[65];
    storage_ndb_hex_encode(id32, id_hex);

    char *json = NULL;
    int json_len = 0;
    if (storage_ndb_get_note_by_id(txn, id32, &json, &json_len, NULL) == 0 && json) {
      if (self->dm_service) {
        gnostr_dm_service_process_gift_wrap(self->dm_service, json);
        processed++;
        g_debug("[GIFTWRAP] Sent gift wrap %.8s... to DM service for decryption", id_hex);
      }
      g_free(json);
    }
  }

  storage_ndb_end_query(txn);

  if (processed > 0)
    g_debug("[GIFTWRAP] Processed %u gift wrap event(s) via DM service", processed);
}

void
gnostr_main_window_start_gift_wrap_subscription_internal(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (self->sub_gift_wrap > 0) {
    g_debug("[GIFTWRAP] Subscription already active (subid=%" G_GUINT64_FORMAT ")",
            (guint64)self->sub_gift_wrap);
    return;
  }

  char *pubkey_hex = gnostr_main_window_get_current_user_pubkey_hex_internal();
  if (!pubkey_hex) {
    g_debug("[GIFTWRAP] No user signed in, skipping gift wrap subscription");
    return;
  }

  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = pubkey_hex;

  if (self->dm_service) {
    gnostr_dm_service_set_user_pubkey(self->dm_service, pubkey_hex);
    g_debug("[DM_SERVICE] Set user pubkey %.8s... on DM service", pubkey_hex);
  }

  g_autofree char *filter_json = g_strdup_printf(
    "{\"kinds\":[%d],\"#p\":[\"%s\"]}",
    NOSTR_KIND_GIFT_WRAP,
    pubkey_hex
  );

  self->sub_gift_wrap = gn_ndb_subscribe(filter_json, on_gift_wrap_batch, self, NULL);

  if (self->sub_gift_wrap > 0) {
    g_debug("[GIFTWRAP] Started subscription for user %.8s... (subid=%" G_GUINT64_FORMAT ")",
            pubkey_hex, (guint64)self->sub_gift_wrap);
  } else {
    g_warning("[GIFTWRAP] Failed to subscribe to gift wrap events");
  }
}

void
gnostr_main_window_stop_gift_wrap_subscription_internal(GnostrMainWindow *self)
{
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  if (self->sub_gift_wrap > 0) {
    gn_ndb_unsubscribe(self->sub_gift_wrap);
    g_debug("[GIFTWRAP] Stopped subscription (subid=%" G_GUINT64_FORMAT ")",
            (guint64)self->sub_gift_wrap);
    self->sub_gift_wrap = 0;
  }

  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = NULL;
}

void
gnostr_main_window_on_notification_event_internal(GnostrBadgeManager *manager,
                                                  GnostrNotificationType type,
                                                  const char *sender_pubkey,
                                                  const char *sender_name,
                                                  const char *content,
                                                  const char *event_id,
                                                  const char *target_note_id,
                                                  guint64 amount_sats,
                                                  gpointer user_data)
{
  (void)manager;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  dispatch_desktop_notification_local(self, type, sender_pubkey, sender_name,
                                      content, event_id, target_note_id, amount_sats);

  if (!self->session_view)
    return;

  GtkWidget *notif_widget = gnostr_session_view_get_notifications_view(self->session_view);
  if (!notif_widget || !GNOSTR_IS_NOTIFICATIONS_VIEW(notif_widget))
    return;

  GnostrNotificationsView *notif_view = GNOSTR_NOTIFICATIONS_VIEW(notif_widget);

  GnostrNotification *notif = g_new0(GnostrNotification, 1);
  notif->id = event_id ? g_strdup(event_id) : g_strdup_printf("notif-%" G_GINT64_FORMAT, g_get_real_time());
  notif->type = type;
  notif->actor_pubkey = sender_pubkey ? g_strdup(sender_pubkey) : NULL;
  notif->actor_name = sender_name ? g_strdup(sender_name) : NULL;
  notif->content_preview = content ? g_strdup(content) : NULL;
  notif->target_note_id = target_note_id ? g_strdup(target_note_id) : NULL;
  notif->created_at = g_get_real_time() / G_USEC_PER_SEC;
  notif->is_read = FALSE;
  notif->zap_amount_msats = amount_sats * 1000;

  gnostr_notifications_view_add_notification(notif_view, notif);
  gnostr_notification_free(notif);

  g_debug("[NOTIFICATIONS] Added notification: type=%d from %.16s...",
          type, sender_pubkey ? sender_pubkey : "(unknown)");
}

static void
on_user_profile_fetched_local(GObject *source, GAsyncResult *res, gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self)) {
    g_object_unref(self);
    return;
  }

  GError *error = NULL;
  GPtrArray *jsons = gnostr_pool_query_finish(GNOSTR_POOL(source), res, &error);

  if (error) {
    g_warning("[AUTH] Profile fetch error: %s", error->message);
    g_clear_error(&error);
  }

  if (jsons && jsons->len > 0) {
    const char *evt_json = g_ptr_array_index(jsons, 0);
    if (evt_json) {
      g_autofree gchar *pk_hex = NULL;
      g_autofree gchar *content_json = NULL;
      g_autofree gchar *reason = NULL;
      gint64 created_at = 0;
      if (!gnostr_profile_event_extract_for_apply(evt_json, &pk_hex, &content_json, &created_at, &reason)) {
        g_warning("[AUTH] Rejecting invalid local profile event: %s", reason ? reason : "unknown");
      } else {
        GPtrArray *b = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(b, g_strdup(evt_json));
        storage_ndb_ingest_events_async(b);

        if (self->user_pubkey_hex && g_strcmp0(self->user_pubkey_hex, pk_hex) == 0 && content_json && *content_json) {
          gnostr_profile_provider_update_if_newer(self->user_pubkey_hex, content_json, created_at);
          GnostrProfileMeta *meta = gnostr_profile_provider_get(self->user_pubkey_hex);
          if (meta) {
            const char *final_name = (meta->display_name && *meta->display_name)
                                     ? meta->display_name : meta->name;
            if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
              gnostr_session_view_set_user_profile(self->session_view,
                                                   self->user_pubkey_hex,
                                                   final_name,
                                                   meta->picture);
            }
            gnostr_profile_meta_free(meta);
          }
        }
      }
    }
  }

  if (jsons)
    g_ptr_array_unref(jsons);

  g_object_unref(self);
}

void
gnostr_main_window_on_nip65_loaded_for_profile_internal(GPtrArray *nip65_relays,
                                                        gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !self->user_pubkey_hex) {
    g_object_unref(self);
    return;
  }

  GnostrProfileMeta *meta = gnostr_profile_provider_get(self->user_pubkey_hex);
  if (meta) {
    const char *final_name = (meta->display_name && *meta->display_name)
                             ? meta->display_name : meta->name;
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_set_user_profile(self->session_view,
                                           self->user_pubkey_hex,
                                           final_name,
                                           meta->picture);
    }
    gnostr_profile_meta_free(meta);
    g_object_unref(self);
    return;
  }

  GPtrArray *relay_urls = gnostr_get_profile_fetch_relay_urls(nip65_relays);

  if (relay_urls->len > 0) {
    const gchar **urls = g_new0(const gchar *, relay_urls->len);
    for (guint i = 0; i < relay_urls->len; i++)
      urls[i] = g_ptr_array_index(relay_urls, i);

    g_autoptr(GNostrPool) profile_pool = gnostr_pool_new();
    gnostr_pool_sync_relays(profile_pool, urls, relay_urls->len);

    NostrFilter *f = nostr_filter_new();
    int kind0 = 0;
    nostr_filter_set_kinds(f, &kind0, 1);
    const char *authors[1] = { self->user_pubkey_hex };
    nostr_filter_set_authors(f, (const char *const *)authors, 1);
    nostr_filter_set_limit(f, 1);
    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, f);
    nostr_filter_free(f);

    g_debug("[AUTH] Fetching profile from %u relays (after NIP-65 load)", relay_urls->len);
    gnostr_pool_query_async(profile_pool, filters, NULL,
                            on_user_profile_fetched_local,
                            g_object_ref(self));

    g_free(urls);
  } else {
    g_object_unref(self);
  }

  g_ptr_array_unref(relay_urls);
}

void
gnostr_main_window_on_user_profile_watch_internal(const char *pubkey_hex,
                                                  const GnostrProfileMeta *meta,
                                                  gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !meta)
    return;

  const char *final_name = (meta->display_name && *meta->display_name)
                           ? meta->display_name : meta->name;
  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    gnostr_session_view_set_user_profile(self->session_view,
                                         pubkey_hex,
                                         final_name,
                                         meta->picture);
  }

  gnostr_main_window_update_login_ui_state_internal(self);
}

static void
on_login_signed_in_local(GnostrLogin *login, const char *npub, gpointer user_data)
{
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  GtkWidget *login_win = gtk_widget_get_ancestor(GTK_WIDGET(login), GTK_TYPE_WINDOW);
  if (login_win && GTK_IS_WINDOW(login_win) && login_win != GTK_WIDGET(self))
    gtk_window_close(GTK_WINDOW(login_win));

  g_debug("[AUTH] User signed in: %s", npub ? npub : "(null)");

  if (self->nip46_session)
    nostr_nip46_session_free(self->nip46_session);
  self->nip46_session = gnostr_login_take_nip46_session(login);

  GnostrSignerService *signer = gnostr_signer_service_get_default();
  gnostr_signer_service_set_nip46_session(signer, self->nip46_session);
  self->nip46_session = NULL;

  if (gnostr_signer_service_get_method(signer) == GNOSTR_SIGNER_METHOD_NIP46)
    g_debug("[AUTH] Using NIP-46 remote signer");
  else if (gnostr_signer_service_get_method(signer) == GNOSTR_SIGNER_METHOD_NIP55L)
    g_debug("[AUTH] Using NIP-55L local signer");

  if (npub && g_str_has_prefix(npub, "npub1")) {
    g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
    if (n19) {
      const char *hex = gnostr_nip19_get_pubkey(n19);
      if (hex) {
        g_free(self->user_pubkey_hex);
        self->user_pubkey_hex = g_strdup(hex);
        gnostr_signer_service_set_pubkey(signer, self->user_pubkey_hex);
      } else {
        g_warning("[AUTH] gnostr_nip19_get_pubkey returned NULL for npub: %.12s...", npub);
      }
    } else {
      g_warning("[AUTH] Failed to decode npub: %.12s...", npub);
    }
  } else if (npub && strlen(npub) == 64) {
    g_free(self->user_pubkey_hex);
    self->user_pubkey_hex = g_strdup(npub);
    gnostr_signer_service_set_pubkey(signer, self->user_pubkey_hex);
    g_debug("[AUTH] Using raw hex pubkey from login: %.16s...", npub);
  } else if (npub) {
    g_warning("[AUTH] Unrecognized pubkey format from login (len=%zu): %.16s...",
              strlen(npub), npub);
  }

  gnostr_main_window_update_login_ui_state_internal(self);

  if (npub && *npub) {
    g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
    if (settings) {
      char **accounts = g_settings_get_strv(settings, "known-accounts");
      gboolean found = FALSE;

      if (accounts) {
        for (int i = 0; accounts[i]; i++) {
          if (g_strcmp0(accounts[i], npub) == 0) {
            found = TRUE;
            break;
          }
        }
      }

      if (!found) {
        guint len = accounts ? g_strv_length(accounts) : 0;
        char **new_accounts = g_new0(char *, len + 2);
        for (guint i = 0; i < len; i++)
          new_accounts[i] = g_strdup(accounts[i]);
        new_accounts[len] = g_strdup(npub);
        g_settings_set_strv(settings, "known-accounts", (const char * const *)new_accounts);
        g_strfreev(new_accounts);
        g_debug("[AUTH] Added npub to known-accounts list");
      }

      g_strfreev(accounts);
    }

    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
      gnostr_session_view_refresh_account_list(GNOSTR_SESSION_VIEW(self->session_view));
  }

  if (self->user_pubkey_hex && self->session_view) {
    GtkWidget *pp = gnostr_session_view_get_profile_pane(self->session_view);
    if (pp && NOSTR_GTK_IS_PROFILE_PANE(pp))
      nostr_gtk_profile_pane_set_own_pubkey(NOSTR_GTK_PROFILE_PANE(pp), self->user_pubkey_hex);
  }

  if (self->user_pubkey_hex) {
    if (self->dm_service) {
      gnostr_dm_service_set_user_pubkey(self->dm_service, self->user_pubkey_hex);
      gnostr_dm_service_start_with_dm_relays(self->dm_service);
      g_debug("[DM_SERVICE] Started live DM subscriptions for user %.16s...", self->user_pubkey_hex);
    }

    {
      GnostrDesktopNotify *desktop = gnostr_desktop_notify_get_default();
      GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
      if (desktop && app)
        gnostr_desktop_notify_set_app(desktop, G_APPLICATION(app));
      if (desktop)
        gnostr_desktop_notify_request_permission(desktop);
    }

    GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
    gnostr_badge_manager_set_user_pubkey(badge_mgr, self->user_pubkey_hex);
    gnostr_badge_manager_set_event_callback(badge_mgr,
                                            gnostr_main_window_on_notification_event_internal,
                                            self,
                                            NULL);
    gnostr_badge_manager_start_subscriptions(badge_mgr);
    g_debug("[AUTH] Started notification subscriptions for user %.16s...", self->user_pubkey_hex);

    GtkWidget *nw = gnostr_session_view_get_notifications_view(self->session_view);
    if (nw && GNOSTR_IS_NOTIFICATIONS_VIEW(nw)) {
      gnostr_notifications_view_set_loading(GNOSTR_NOTIFICATIONS_VIEW(nw), TRUE);
      gnostr_badge_manager_load_history(badge_mgr, GNOSTR_NOTIFICATIONS_VIEW(nw));
    }
  }

  gnostr_main_window_start_gift_wrap_subscription_internal(self);

  if (self->user_pubkey_hex) {
    if (self->profile_watch_id)
      gnostr_profile_provider_unwatch(self->profile_watch_id);
    self->profile_watch_id = gnostr_profile_provider_watch(self->user_pubkey_hex,
                                                           gnostr_main_window_on_user_profile_watch_internal,
                                                           self);

    gnostr_nip65_load_on_login_async(self->user_pubkey_hex,
                                     gnostr_main_window_on_nip65_loaded_for_profile_internal,
                                     g_object_ref(self));

    gnostr_blossom_settings_load_from_relays_async(self->user_pubkey_hex, NULL, NULL);
    gnostr_nip51_settings_auto_sync_on_login(self->user_pubkey_hex);
    gnostr_sync_bridge_set_user_pubkey(self->user_pubkey_hex);
    gnostr_profile_provider_prewarm_async(self->user_pubkey_hex);

    {
      g_autoptr(GSettings) client = g_settings_new("org.gnostr.Client");
      if (g_settings_get_boolean(client, "negentropy-auto-sync")) {
        GNostrSyncService *svc = gnostr_sync_service_get_default();
        if (svc)
          gnostr_sync_service_start(svc);
      }
    }
  }

  gnostr_main_window_show_toast(GTK_WIDGET(self), "Signed in successfully");
}

static void
open_login_dialog_local(GnostrMainWindow *self)
{
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_transient_for(win, GTK_WINDOW(self));
  gtk_window_set_modal(win, TRUE);
  gtk_window_set_default_size(win, 400, 500);
  gtk_window_set_resizable(win, FALSE);
  gtk_window_set_decorated(win, FALSE);

  GnostrLogin *login = gnostr_login_new();
  gtk_window_set_child(win, GTK_WIDGET(login));

  g_signal_connect(login, "signed-in", G_CALLBACK(on_login_signed_in_local), self);
  g_signal_connect_swapped(login, "signed-in", G_CALLBACK(gtk_window_close), win);
  g_signal_connect_swapped(login, "cancelled", G_CALLBACK(gtk_window_close), win);

  gtk_window_present(win);
}

void
gnostr_main_window_on_avatar_login_clicked_internal(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  open_login_dialog_local(self);
}

void
gnostr_main_window_on_signer_state_changed_internal(GnostrSignerService *signer,
                                                    guint old_state,
                                                    guint new_state,
                                                    gpointer user_data)
{
  (void)signer;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  g_debug("[MAIN] Signer state changed: %u -> %u", old_state, new_state);

  gboolean is_connected = (new_state == GNOSTR_SIGNER_STATE_CONNECTED);

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
    if (is_connected) {
      g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
      if (settings) {
        g_autofree char *npub = g_settings_get_string(settings, "current-npub");
        gnostr_session_view_set_authenticated(self->session_view, npub && *npub);
      }
    } else {
      gnostr_session_view_set_authenticated(self->session_view, FALSE);
    }
  }

  /* nostrc-e03f.4: the Following tab's query depends on self->user_pubkey_hex
   * and the NDB contact list. When the signer reaches a stable identity
   * transition (fully connected or fully disconnected), refresh the
   * currently-selected tab's query so the Following tab (if active) picks
   * up the new author set without the user having to re-click it.
   *
   * We intentionally ignore intermediate states (CONNECTING, PAIRING, etc.)
   * because user_pubkey_hex / the NDB contact list won't have settled yet
   * and refreshing on every transition would run the query dispatcher
   * multiple times per login. */
  if (new_state == GNOSTR_SIGNER_STATE_CONNECTED ||
      new_state == GNOSTR_SIGNER_STATE_DISCONNECTED) {
    gnostr_main_window_refresh_current_tab_filter_internal(self);
  }
}

void
gnostr_main_window_on_avatar_logout_clicked_internal(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  gnostr_main_window_stop_gift_wrap_subscription_internal(self);

  if (self->dm_service)
    gnostr_dm_service_stop(self->dm_service);

  GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
  gnostr_badge_manager_stop_subscriptions(badge_mgr);
  gnostr_badge_manager_set_event_callback(badge_mgr, NULL, NULL, NULL);

  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (settings)
    g_settings_set_string(settings, "current-npub", "");

  gnostr_sync_bridge_set_user_pubkey(NULL);

  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = NULL;

  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  gnostr_signer_service_clear(gnostr_signer_service_get_default());
  gnostr_main_window_update_login_ui_state_internal(self);

  if (self->gift_wrap_queue)
    g_ptr_array_set_size(self->gift_wrap_queue, 0);

  gnostr_main_window_show_toast(GTK_WIDGET(self), "Signed out");
}

void
gnostr_main_window_on_view_profile_requested_internal(GnostrSessionView *sv, gpointer user_data)
{
  (void)sv;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self))
    return;

  const char *hex = self->user_pubkey_hex;
  g_autofree char *settings_hex = NULL;
  if (!hex || !*hex) {
    settings_hex = gnostr_main_window_get_current_user_pubkey_hex_internal();
    hex = settings_hex;
  }
  if (!hex || !*hex) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), _("Not signed in"));
    return;
  }

  GtkWidget *pane_w = self->session_view
    ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
  if (!pane_w || !NOSTR_GTK_IS_PROFILE_PANE(pane_w)) {
    gnostr_main_window_show_toast(GTK_WIDGET(self), _("Profile unavailable"));
    return;
  }
  NostrGtkProfilePane *pane = NOSTR_GTK_PROFILE_PANE(pane_w);

  gboolean sidebar_up = gnostr_main_window_is_panel_visible_internal(self)
    && gnostr_session_view_is_showing_profile(self->session_view);
  const char *cur = nostr_gtk_profile_pane_get_current_pubkey(pane);
  if (sidebar_up && cur && strcmp(cur, hex) == 0) {
    gnostr_main_window_hide_panel_internal(self);
    return;
  }

  gnostr_main_window_show_profile_panel_internal(self);
  nostr_gtk_profile_pane_set_pubkey(pane, hex);
}

void
gnostr_main_window_on_account_switch_requested_internal(GnostrSessionView *view,
                                                        const char *npub,
                                                        gpointer user_data)
{
  (void)view;
  GnostrMainWindow *self = GNOSTR_MAIN_WINDOW(user_data);
  if (!GNOSTR_IS_MAIN_WINDOW(self) || !npub || !*npub)
    return;

  g_debug("[AUTH] Account switch requested to: %s", npub);

  gnostr_main_window_stop_gift_wrap_subscription_internal(self);
  if (self->dm_service)
    gnostr_dm_service_stop(self->dm_service);
  GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
  gnostr_badge_manager_stop_subscriptions(badge_mgr);
  gnostr_badge_manager_set_event_callback(badge_mgr, NULL, NULL, NULL);

  g_free(self->user_pubkey_hex);
  self->user_pubkey_hex = NULL;

  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  gnostr_signer_service_clear(gnostr_signer_service_get_default());

  g_autoptr(GSettings) settings = g_settings_new("org.gnostr.Client");
  if (settings)
    g_settings_set_string(settings, "current-npub", npub);

  gnostr_sync_bridge_set_user_pubkey(NULL);

  gnostr_main_window_update_login_ui_state_internal(self);
  open_login_dialog_local(self);
  gnostr_main_window_show_toast(GTK_WIDGET(self), "Please sign in to switch accounts");
}

void
gnostr_main_window_restore_session_services_internal(GnostrMainWindow *self)
{
  g_return_if_fail(GNOSTR_IS_MAIN_WINDOW(self));

  g_autofree char *npub = client_settings_get_current_npub_local();
  gboolean signed_in = (npub && *npub);

  if (signed_in) {
    GnostrSignerService *signer = gnostr_signer_service_get_default();
    if (gnostr_signer_service_restore_from_settings(signer)) {
      g_message("[MAIN] Restored NIP-46 session from saved credentials");

      g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(npub, NULL);
      if (n19) {
        const char *pubkey_hex = gnostr_nip19_get_pubkey(n19);
        if (pubkey_hex) {
          gnostr_signer_service_set_pubkey(signer, pubkey_hex);
          if (!self->user_pubkey_hex) {
            self->user_pubkey_hex = g_strdup(pubkey_hex);
            g_debug("[AUTH] Restored user_pubkey_hex from session restore: %.16s...", pubkey_hex);
          }
        }
      } else if (strlen(npub) == 64) {
        gnostr_signer_service_set_pubkey(signer, npub);
        if (!self->user_pubkey_hex) {
          self->user_pubkey_hex = g_strdup(npub);
          g_debug("[AUTH] Restored user_pubkey_hex from raw hex in settings: %.16s...", npub);
        }
      }
    } else {
      g_debug("[MAIN] No NIP-46 credentials to restore, checking NIP-55L fallback");
    }

    if (!gnostr_signer_service_is_available(signer)) {
      g_warning("[MAIN] Signer not available after restore - clearing signed-in state");
      signed_in = FALSE;
    }
  }

  if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view))
    gnostr_session_view_set_authenticated(self->session_view, signed_in);

  if (!(signed_in && self->user_pubkey_hex)) {
    gnostr_sync_bridge_set_user_pubkey(NULL);
    return;
  }

  if (self->dm_service) {
    gnostr_dm_service_set_user_pubkey(self->dm_service, self->user_pubkey_hex);
    gnostr_dm_service_start_with_dm_relays(self->dm_service);
    g_debug("[DM_SERVICE] Restored live DM subscriptions for user %.16s...", self->user_pubkey_hex);
  }

  if (self->profile_watch_id)
    gnostr_profile_provider_unwatch(self->profile_watch_id);
  self->profile_watch_id = gnostr_profile_provider_watch(self->user_pubkey_hex,
                                                         gnostr_main_window_on_user_profile_watch_internal,
                                                         self);

  GnostrProfileMeta *meta = gnostr_profile_provider_get(self->user_pubkey_hex);
  if (meta) {
    const char *final_name = (meta->display_name && *meta->display_name)
                             ? meta->display_name : meta->name;
    if (self->session_view && GNOSTR_IS_SESSION_VIEW(self->session_view)) {
      gnostr_session_view_set_user_profile(self->session_view,
                                           self->user_pubkey_hex,
                                           final_name,
                                           meta->picture);
    }
    gnostr_profile_meta_free(meta);
  }

  gnostr_nip65_load_on_login_async(self->user_pubkey_hex,
                                   gnostr_main_window_on_nip65_loaded_for_profile_internal,
                                   g_object_ref(self));

  gnostr_profile_provider_prewarm_async(self->user_pubkey_hex);

  {
    GnostrBadgeManager *badge_mgr = gnostr_badge_manager_get_default();
    GnostrDesktopNotify *desktop = gnostr_desktop_notify_get_default();
    GtkApplication *app = GTK_APPLICATION(gtk_window_get_application(GTK_WINDOW(self)));
    if (desktop && app)
      gnostr_desktop_notify_set_app(desktop, G_APPLICATION(app));
    if (desktop)
      gnostr_desktop_notify_request_permission(desktop);

    gnostr_badge_manager_set_user_pubkey(badge_mgr, self->user_pubkey_hex);
    gnostr_badge_manager_set_event_callback(badge_mgr,
                                            gnostr_main_window_on_notification_event_internal,
                                            self,
                                            NULL);
    gnostr_badge_manager_start_subscriptions(badge_mgr);

    GtkWidget *nw = gnostr_session_view_get_notifications_view(self->session_view);
    if (nw && GNOSTR_IS_NOTIFICATIONS_VIEW(nw)) {
      gnostr_notifications_view_set_loading(GNOSTR_NOTIFICATIONS_VIEW(nw), TRUE);
      gnostr_badge_manager_load_history(badge_mgr, GNOSTR_NOTIFICATIONS_VIEW(nw));
    }
  }

  gnostr_nip51_settings_auto_sync_on_login(self->user_pubkey_hex);
  gnostr_sync_bridge_set_user_pubkey(self->user_pubkey_hex);
  gnostr_blossom_settings_load_from_relays_async(self->user_pubkey_hex, NULL, NULL);

  GtkWidget *pp = self->session_view
    ? gnostr_session_view_get_profile_pane(self->session_view) : NULL;
  if (pp && NOSTR_GTK_IS_PROFILE_PANE(pp))
    nostr_gtk_profile_pane_set_own_pubkey(NOSTR_GTK_PROFILE_PANE(pp), self->user_pubkey_hex);

  gnostr_main_window_update_login_ui_state_internal(self);
  g_debug("[AUTH] Restored session services for user %.16s...", self->user_pubkey_hex);
}
