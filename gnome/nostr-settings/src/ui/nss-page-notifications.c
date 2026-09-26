/* nss-page-notifications.c — nostr-notify: unit on/off + its config file
 * (~/.config/nostr-notify/nostr-notify.conf).
 * SPDX-License-Identifier: MIT
 */
#include "nss-ui.h"
#include "nss-config.h"
#include "nss-lists.h"
#include "nss-systemd.h"

typedef struct {
  NssContext   *ctx;
  GtkWidget    *page;
  gchar        *path;
  NssNotifyConf conf;
  AdwSwitchRow *enable;
  GtkWidget    *status;
  GtkWidget    *restart;
  gboolean      syncing;
  gboolean      running;
  AdwComboRow  *mode;
  AdwPreferencesGroup *relays;
  AdwSwitchRow *groups, *dms, *preview, *sound;
} Page;

static void
page_free(gpointer data)
{
  Page *p = data;
  nss_notify_conf_clear(&p->conf);
  g_free(p->path);
  nss_context_unref(p->ctx);
  g_free(p);
}

/* ── unit ── */

static gpointer
unit_load(gpointer data, GError **error)
{
  NssUnitState *s = g_new0(NssUnitState, 1);
  if (!nss_systemd_get_state(data, NSS_NOTIFY_SERVICE, s, error)) {
    g_free(s);
    return NULL;
  }
  return s;
}

static void
unit_state_free(gpointer p)
{
  nss_unit_state_clear(p);
  g_free(p);
}

static void
unit_loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  NssUnitState *s = result;
  if (s == NULL) {
    gtk_widget_set_sensitive(GTK_WIDGET(p->enable), FALSE);
    nss_row_set_subtitle_plain(p->status, error ? error->message : "systemd unavailable");
    return;
  }
  NssServiceStatus st = nss_service_status(NULL, s);
  p->running = st == NSS_SVC_RUNNING;
  p->syncing = TRUE;
  adw_switch_row_set_active(p->enable, nss_unit_file_enabled(s));
  p->syncing = FALSE;
  gtk_widget_set_sensitive(GTK_WIDGET(p->enable),
                           st != NSS_SVC_NOT_INSTALLED && st != NSS_SVC_MASKED);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status),
                                nss_service_status_label(st, FALSE));
  nss_row_set_subtitle_plain(p->status, st == NSS_SVC_NOT_INSTALLED
                               ? "Install the nostr-notify package" : NULL);
  gtk_widget_set_visible(p->restart, nss_service_can_restart(st));
}

static void
refresh_unit(Page *p)
{
  if (p->ctx->bus)
    nss_run(p->page, unit_load, unit_loaded, g_object_ref(p->ctx->bus), g_object_unref,
            unit_state_free);
}

static void
ops_done(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  GtkWidget *page = data;
  Page *p = g_object_get_data(G_OBJECT(page), "nss-page");
  g_autoptr(GError) err = NULL;
  if (!nss_systemd_run_finish(res, &err) && p)
    nss_toast(p->ctx, "%s", err->message);
  if (p)
    refresh_unit(p);
  g_object_unref(page);
}

static void
on_enable(AdwSwitchRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  if (p->syncing)
    return;
  guint n = 0;
  const NssUnitOp *ops = nss_notify_plan(adw_switch_row_get_active(row), &n);
  nss_systemd_run_async(p->ctx->bus, ops, n, NULL, ops_done, g_object_ref(p->page));
}

static void
on_restart(GtkButton *b, gpointer data)
{
  (void)b;
  Page *p = data;
  static const NssUnitOp op[] = { { NSS_OP_RESTART, NSS_NOTIFY_SERVICE } };
  nss_systemd_run_async(p->ctx->bus, op, 1, NULL, ops_done, g_object_ref(p->page));
}

/* ── config ── */

static void render_relays(Page *p);

static void
save(Page *p)
{
  g_autoptr(GError) err = NULL;
  if (!nss_notify_conf_save(p->path, &p->conf, &err)) {
    nss_toast(p->ctx, "Not saved: %s", err->message);
    return;
  }
  if (p->running)
    nss_toast(p->ctx, "Saved. Restart notifications to apply.");
}

static void
on_mode(AdwComboRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  if (p->syncing)
    return;
  p->conf.upstream = adw_combo_row_get_selected(row) == 1 ? NSS_NOTIFY_UPSTREAM_SESSION_RELAY
                                                          : NSS_NOTIFY_UPSTREAM_DIRECT;
  save(p);
}

static void
on_remove_relay(GtkButton *b, gpointer data)
{
  Page *p = data;
  const gchar *url = g_object_get_data(G_OBJECT(b), "nss-url");
  g_autoptr(GStrvBuilder) sb = g_strv_builder_new();
  for (guint i = 0; p->conf.home_relays[i]; i++)
    if (!g_str_equal(p->conf.home_relays[i], url))
      g_strv_builder_add(sb, p->conf.home_relays[i]);
  g_strfreev(p->conf.home_relays);
  p->conf.home_relays = g_strv_builder_end(sb);
  save(p);
  render_relays(p);
}

static void
on_add_relay(AdwEntryRow *row, gpointer data)
{
  Page *p = data;
  g_autoptr(GError) err = NULL;
  g_autofree gchar *url = nss_relay_url_normalize(gtk_editable_get_text(GTK_EDITABLE(row)), &err);
  if (url == NULL) {
    nss_toast(p->ctx, "%s", err->message);
    return;
  }
  if (g_strv_contains((const gchar *const *)p->conf.home_relays, url))
    return;
  const gchar *one[] = { url, NULL };
  gchar **merged = nss_strv_union((const gchar *const *)p->conf.home_relays, one, NULL);
  g_strfreev(p->conf.home_relays);
  p->conf.home_relays = merged;
  gtk_editable_set_text(GTK_EDITABLE(row), "");
  save(p);
  render_relays(p);
}

static void
render_relays(Page *p)
{
  nss_group_clear_dynamic(p->relays);
  for (guint i = 0; p->conf.home_relays[i]; i++) {
    GtkWidget *row = nss_info_row(p->conf.home_relays[i], NULL);
    GtkWidget *rm = nss_suffix_button("user-trash-symbolic", "Remove relay");
    g_object_set_data_full(G_OBJECT(rm), "nss-url", g_strdup(p->conf.home_relays[i]), g_free);
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove_relay), p);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), rm);
    nss_group_add_dynamic(p->relays, row);
  }
}

static AdwSwitchRow *
reserved(AdwPreferencesGroup *g, const gchar *title, const gchar *subtitle, gboolean v)
{
  AdwSwitchRow *r = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
  adw_switch_row_set_active(r, v);
  gtk_widget_set_sensitive(GTK_WIDGET(r), nss_notify_presentation_supported());
  adw_preferences_group_add(g, GTK_WIDGET(r));
  return r;
}

AdwPreferencesPage *
nss_page_notifications_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Notifications");
  adw_preferences_page_set_icon_name(page, "preferences-system-notifications-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  p->page = GTK_WIDGET(page);
  p->path = nss_notify_conf_path();
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);
  g_autoptr(GError) err = NULL;
  if (!nss_notify_conf_load(p->path, &p->conf, &err)) {
    g_warning("%s", err->message);
    nss_notify_conf_clear(&p->conf);
    nss_notify_conf_init(&p->conf);
  }

  AdwPreferencesGroup *svc = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(svc, "Nostr notifications");
  adw_preferences_group_set_description(svc,
    "Desktop notifications for group messages (NIP-29) and direct messages "
    "(NIP-17) addressed to your active identity.");
  p->enable = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->enable), "Show Nostr notifications");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(p->enable), "Starts with your session");
  g_signal_connect(p->enable, "notify::active", G_CALLBACK(on_enable), p);
  adw_preferences_group_add(svc, GTK_WIDGET(p->enable));
  p->status = nss_info_row("Checking…", NULL);
  p->restart = gtk_button_new_with_label("Restart");
  gtk_widget_set_valign(p->restart, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(p->restart, FALSE);
  g_signal_connect(p->restart, "clicked", G_CALLBACK(on_restart), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->status), p->restart);
  adw_preferences_group_add(svc, p->status);
  adw_preferences_page_add(page, svc);

  AdwPreferencesGroup *src = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(src, "Where to listen");
  g_autofree gchar *desc = g_strdup_printf(
    "Saved in %s. The service reads it when it starts.", p->path);
  adw_preferences_group_set_description(src, desc);
  p->mode = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->mode), "Connect");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(p->mode),
    "Through the session relay currently falls back to your relays directly");
  const gchar *modes[] = { "To your relays directly", "Through the session relay", NULL };
  g_autoptr(GtkStringList) model = gtk_string_list_new(modes);
  adw_combo_row_set_model(p->mode, G_LIST_MODEL(model));
  p->syncing = TRUE;
  adw_combo_row_set_selected(p->mode, p->conf.upstream == NSS_NOTIFY_UPSTREAM_SESSION_RELAY);
  p->syncing = FALSE;
  g_signal_connect(p->mode, "notify::selected", G_CALLBACK(on_mode), p);
  adw_preferences_group_add(src, GTK_WIDGET(p->mode));
  adw_preferences_page_add(page, src);

  p->relays = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->relays, "Fallback relays");
  adw_preferences_group_set_description(p->relays,
    "Used only when the signer has no relays configured (the signer's list wins).");
  AdwEntryRow *add = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add), "Add relay (wss://…)");
  adw_entry_row_set_show_apply_button(add, TRUE);
  g_signal_connect(add, "apply", G_CALLBACK(on_add_relay), p);
  g_signal_connect(add, "entry-activated", G_CALLBACK(on_add_relay), p);
  adw_preferences_group_add(p->relays, GTK_WIDGET(add));
  render_relays(p);
  adw_preferences_page_add(page, p->relays);

  AdwPreferencesGroup *show = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(show, "What to show");
  if (!nss_notify_presentation_supported()) {
    AdwBanner *b = ADW_BANNER(adw_banner_new(
      "nostr-notify doesn't read these options yet (nostrc-prqu.16); they show its "
      "current fixed behaviour."));
    adw_banner_set_revealed(b, TRUE);
    adw_preferences_group_add(show, GTK_WIDGET(b));
  }
  p->groups = reserved(show, "Group messages", "NIP-29 groups you are in", p->conf.notify_groups);
  p->dms = reserved(show, "Direct messages", "Encrypted NIP-17 messages to you", p->conf.notify_dms);
  p->preview = reserved(show, "Preview group messages", "Show the first line of the message",
                        p->conf.group_preview);
  p->sound = reserved(show, "Play a sound", NULL, p->conf.sound);
  adw_preferences_group_add(show, nss_status_row("Direct message contents are never shown",
    "The notification only says a message arrived; open it in your client",
    "channel-secure-symbolic"));
  adw_preferences_page_add(page, show);

  if (ctx->bus)
    refresh_unit(p);
  return page;
}
