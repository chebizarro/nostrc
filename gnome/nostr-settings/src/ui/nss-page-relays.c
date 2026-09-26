/* nss-page-relays.c — session relay, storage limits, NIP-65 relay list.
 * SPDX-License-Identifier: MIT
 *
 * Semantics (reviewed; see nss-systemd.h): the switch is the socket unit's
 * enabled state; the status row is runtime truth; stats come from
 * org.nostr.SessionRelay1 only while its name is owned (never
 * auto-started). Storage limits are written to session-relay.conf only
 * when the running relay reports RetentionSupported.
 */
#include "nss-ui.h"
#include "nss-config.h"
#include "nss-lists.h"
#include "nss-net.h"
#include "nss-nip11.h"
#include "nss-relay-stats.h"
#include "nss-systemd.h"

#define STATS_POLL_S 2
#define UNITS_POLL_S 5

typedef struct {
  NssContext *ctx;
  GtkWidget  *page;

  /* session relay */
  AdwSwitchRow *enable;
  GtkWidget    *status;
  GtkWidget    *restart;
  GtkWidget    *start_now;
  GtkWidget    *notify_note;
  GtkWidget    *storage;
  GtkWidget    *clients;
  GtkWidget    *uptime;
  GtkWidget    *nips;
  gboolean      syncing;       /* programmatic switch update */
  gboolean      busy;
  gboolean      socket_enabled;
  NssServiceStatus status_now;
  guint         watch_id;
  guint         stats_timer;
  guint         units_timer;
  gboolean      owned;
  NssRelayStats stats;
  gboolean      have_stats;

  /* retention */
  AdwPreferencesGroup *ret_group;
  GtkWidget    *ret_note;      /* first row: why the limits are read-only */
  GPtrArray    *ret_rows;      /* widgets toggled together */
  AdwSwitchRow *ret_enabled;
  AdwSpinRow   *ret_cache, *ret_high, *ret_low, *ret_minage, *ret_notes, *ret_reactions,
               *ret_interval;
  guint         ret_save_id;
  gboolean      ret_loading;

  /* NIP-65 */
  AdwPreferencesGroup *list_group;
  AdwEntryRow  *add_row;
  GtkWidget    *publish_row;
  GtkWidget    *publish_btn;
  GtkWidget    *list_status;
  GPtrArray    *entries;       /* NssRelayEntry* (edited) */
  GPtrArray    *old_entries;   /* as published */
  gchar       **signer_relays;
  gchar        *pubkey_hex;
  gboolean      dirty;
  gboolean      signer_failed;
  GHashTable   *nip11_cache;   /* url → NssNip11Info* (or NULL sentinel) */
  GCancellable *cancel;
} Page;

static void page_free(gpointer p);

/* ══ Session relay ═══════════════════════════════════════════════════════ */

typedef struct {
  NssUnitState socket, service, notify;
  gboolean     ok;
  gchar       *error;
} Units;

static void
units_free(gpointer p)
{
  Units *u = p;
  nss_unit_state_clear(&u->socket);
  nss_unit_state_clear(&u->service);
  nss_unit_state_clear(&u->notify);
  g_free(u->error);
  g_free(u);
}

static gpointer
units_load(gpointer data, GError **error)
{
  (void)error;
  GDBusConnection *bus = data;
  Units *u = g_new0(Units, 1);
  GError *e = NULL;
  u->ok = nss_systemd_get_state(bus, NSS_RELAY_SOCKET, &u->socket, &e) &&
          nss_systemd_get_state(bus, NSS_RELAY_SERVICE, &u->service, &e);
  if (!u->ok)
    u->error = g_strdup(e ? e->message : "systemd --user is not reachable");
  g_clear_error(&e);
  (void)nss_systemd_get_state(bus, NSS_NOTIFY_SERVICE, &u->notify, NULL);
  return u;
}

static void
units_loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)error; (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  Units *u = result;
  if (!u->ok) {
    gtk_widget_set_sensitive(GTK_WIDGET(p->enable), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status), "Unknown");
    nss_row_set_subtitle_plain(p->status, u->error);
    return;
  }
  p->status_now = nss_service_status(&u->socket, &u->service);
  p->socket_enabled = nss_unit_file_enabled(&u->socket);
  gboolean installed = p->status_now != NSS_SVC_NOT_INSTALLED;
  gboolean masked = p->status_now == NSS_SVC_MASKED;
  p->syncing = TRUE;
  adw_switch_row_set_active(p->enable, p->socket_enabled);
  p->syncing = FALSE;
  gtk_widget_set_sensitive(GTK_WIDGET(p->enable), installed && !masked && !p->busy);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->status),
                                nss_service_status_label(p->status_now, TRUE));
  const gchar *sub = NULL;
  switch (p->status_now) {
  case NSS_SVC_NOT_INSTALLED:
    sub = "Install the nostrc-session-relay package";
    break;
  case NSS_SVC_LISTENING:
    sub = p->socket_enabled ? "Apps reach it at $XDG_RUNTIME_DIR/nostr/relay.sock"
                            : "Started by hand — won't start at your next login";
    break;
  case NSS_SVC_RUNNING:
    sub = p->socket_enabled ? "Serving apps on this computer"
                            : "Started by hand — won't start at your next login";
    break;
  case NSS_SVC_FAILED:
    sub = "journalctl --user -u nostr-session-relay.service";
    break;
  case NSS_SVC_STOPPED:
    sub = "Starts at your next login";
    break;
  default:
    sub = NULL;
  }
  nss_row_set_subtitle_plain(p->status, sub);
  gtk_widget_set_visible(p->restart, nss_service_can_restart(p->status_now));
  gtk_widget_set_visible(p->start_now, p->status_now == NSS_SVC_STOPPED);
  gboolean notify_pulls = nss_unit_file_enabled(&u->notify) &&
                          p->status_now != NSS_SVC_NOT_INSTALLED && !p->socket_enabled;
  gtk_widget_set_visible(p->notify_note, notify_pulls);
}

static void
refresh_units(Page *p)
{
  if (p->ctx->bus)
    nss_run(p->page, units_load, units_loaded, g_object_ref(p->ctx->bus), g_object_unref,
            units_free);
}

static gboolean
units_tick(gpointer data)
{
  refresh_units(data);
  return G_SOURCE_CONTINUE;
}

static void
ops_done(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  GtkWidget *page = data; /* strong ref taken at call */
  Page *p = g_object_get_data(G_OBJECT(page), "nss-page");
  g_autoptr(GError) err = NULL;
  if (!nss_systemd_run_finish(res, &err) && p)
    nss_toast(p->ctx, "%s", err->message);
  if (p) {
    p->busy = FALSE;
    refresh_units(p);
  }
  g_object_unref(page);
}

static void
run_ops(Page *p, const NssUnitOp *ops, guint n)
{
  p->busy = TRUE;
  gtk_widget_set_sensitive(GTK_WIDGET(p->enable), FALSE);
  nss_systemd_run_async(p->ctx->bus, ops, n, NULL, ops_done, g_object_ref(p->page));
}

static void
on_enable(AdwSwitchRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  Page *p = data;
  if (p->syncing || p->ctx->bus == NULL)
    return;
  guint n = 0;
  const NssUnitOp *ops = nss_relay_plan(adw_switch_row_get_active(row), &n);
  run_ops(p, ops, n);
}

static void
on_restart(GtkButton *b, gpointer data)
{
  (void)b;
  static const NssUnitOp op[] = { { NSS_OP_RESTART, NSS_RELAY_SERVICE } };
  run_ops(data, op, 1);
}

static void
on_start_now(GtkButton *b, gpointer data)
{
  (void)b;
  static const NssUnitOp op[] = { { NSS_OP_START, NSS_RELAY_SOCKET } };
  run_ops(data, op, 1);
}

/* ── stats ── */

static void retention_update_sensitivity(Page *p);

static void
show_idle_stats(Page *p)
{
  g_autofree gchar *dir = nss_session_relay_storage_dir();
  g_autofree gchar *size = g_format_size(nss_disk_usage(dir));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->storage), "Disk used");
  g_autofree gchar *sub = g_strdup_printf("%s in %s", size, dir);
  nss_row_set_subtitle_plain(p->storage, sub);
  nss_row_set_subtitle_plain(p->clients, "Relay idle — statistics appear when an app connects");
  nss_row_set_subtitle_plain(p->uptime, "—");
  nss_row_set_subtitle_plain(p->nips, "—");
}

static void
show_stats(Page *p)
{
  g_autofree gchar *title = nss_format_storage_title(&p->stats);
  g_autofree gchar *detail = nss_format_storage_detail(&p->stats);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->storage), title);
  nss_row_set_subtitle_plain(p->storage, detail);
  g_autofree gchar *c = g_strdup_printf("%" G_GUINT32_FORMAT " connected now · %"
                                        G_GUINT64_FORMAT " since start",
                                        p->stats.connected_clients,
                                        p->stats.connections_total);
  nss_row_set_subtitle_plain(p->clients, c);
  g_autofree gchar *up = nss_format_uptime(p->stats.uptime);
  nss_row_set_subtitle_plain(p->uptime, up);
  g_autofree gchar *n = nss_format_nips(p->stats.supported_nips);
  nss_row_set_subtitle_plain(p->nips, n);
}

static void
stats_done(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  GtkWidget *page = data;
  Page *p = g_object_get_data(G_OBJECT(page), "nss-page");
  NssRelayStats s = { 0 };
  g_autoptr(GError) err = NULL;
  gboolean ok = nss_relay_stats_fetch_finish(res, &s, &err);
  if (p && ok) {
    nss_relay_stats_clear(&p->stats);
    p->stats = s;
    p->have_stats = TRUE;
    show_stats(p);
    retention_update_sensitivity(p);
  } else {
    nss_relay_stats_clear(&s);
  }
  g_object_unref(page);
}

static gboolean
stats_tick(gpointer data)
{
  Page *p = data;
  nss_relay_stats_fetch_async(p->ctx->bus, NULL, stats_done, g_object_ref(p->page));
  return G_SOURCE_CONTINUE;
}

static void
stop_stats(Page *p)
{
  if (p->stats_timer) {
    g_source_remove(p->stats_timer);
    p->stats_timer = 0;
  }
}

static void
start_stats(Page *p)
{
  if (p->stats_timer || !p->owned || !gtk_widget_get_mapped(p->page))
    return;
  stats_tick(p);
  p->stats_timer = g_timeout_add_seconds(STATS_POLL_S, stats_tick, p);
}

static void
on_name_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer data)
{
  (void)c; (void)name; (void)owner;
  Page *p = data;
  p->owned = TRUE;
  start_stats(p);
}

static void
on_name_vanished(GDBusConnection *c, const gchar *name, gpointer data)
{
  (void)c; (void)name;
  Page *p = data;
  p->owned = FALSE;
  stop_stats(p);
  p->have_stats = FALSE;
  show_idle_stats(p);
  retention_update_sensitivity(p);
}

static void
on_map(GtkWidget *w, gpointer data)
{
  (void)w;
  Page *p = data;
  refresh_units(p);
  if (!p->units_timer)
    p->units_timer = g_timeout_add_seconds(UNITS_POLL_S, units_tick, p);
  start_stats(p);
}

static void
on_unmap(GtkWidget *w, gpointer data)
{
  (void)w;
  Page *p = data;
  stop_stats(p);
  if (p->units_timer) {
    g_source_remove(p->units_timer);
    p->units_timer = 0;
  }
}

/* ══ Retention ═══════════════════════════════════════════════════════════ */

static void
retention_update_sensitivity(Page *p)
{
  gboolean supported = p->have_stats && p->stats.retention_supported &&
                       nss_relay_stats_has_storage(&p->stats);
  for (guint i = 0; i < p->ret_rows->len; i++)
    gtk_widget_set_sensitive(g_ptr_array_index(p->ret_rows, i), supported);
  const gchar *title = NULL, *msg = NULL;
  if (!p->have_stats) {
    title = "Waiting for the session relay";
    msg = "Limits can be changed once the relay is running and reports that it "
          "enforces them.";
  } else if (!nss_relay_stats_has_storage(&p->stats)) {
    title = "Nothing to limit";
    msg = "This relay has no storage backend and keeps nothing (nostrc-prqu.5).";
  } else if (!p->stats.retention_supported) {
    title = "Not enforced yet";
    msg = "The session relay doesn't apply storage limits yet (nostrc-prqu.17); "
          "they become editable here once it does.";
  }
  if (title) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->ret_note), title);
    nss_row_set_subtitle_plain(p->ret_note, msg);
  }
  gtk_widget_set_visible(p->ret_note, title != NULL);
}

static void
retention_read(Page *p, NssRetention *r)
{
  r->enabled = adw_switch_row_get_active(p->ret_enabled);
  r->cache_max_mb = (gint)adw_spin_row_get_value(p->ret_cache);
  r->high_watermark_pct = (gint)adw_spin_row_get_value(p->ret_high);
  r->low_watermark_pct = (gint)adw_spin_row_get_value(p->ret_low);
  r->min_age_days = (gint)adw_spin_row_get_value(p->ret_minage);
  r->note_ttl_days = (gint)adw_spin_row_get_value(p->ret_notes);
  r->reaction_ttl_days = (gint)adw_spin_row_get_value(p->ret_reactions);
  r->interval_mins = (gint)adw_spin_row_get_value(p->ret_interval);
}

static void
retention_save_now(Page *p, gboolean report)
{
  NssRetention r;
  retention_read(p, &r);
  g_autofree gchar *path = nss_relay_conf_path();
  g_autoptr(GError) err = NULL;
  if (!nss_relay_conf_save(path, &r, &err)) {
    if (report)
      nss_toast(p->ctx, "Storage limits not saved: %s", err->message);
    else
      g_warning("storage limits not saved: %s", err->message);
  }
}

static gboolean
retention_save(gpointer data)
{
  Page *p = data;
  p->ret_save_id = 0;
  retention_save_now(p, TRUE);
  return G_SOURCE_REMOVE;
}

static void
on_retention_changed(GObject *o, GParamSpec *ps, gpointer data)
{
  (void)o; (void)ps;
  Page *p = data;
  if (p->ret_loading || !gtk_widget_get_sensitive(GTK_WIDGET(p->ret_enabled)))
    return;
  if (p->ret_save_id)
    g_source_remove(p->ret_save_id);
  p->ret_save_id = g_timeout_add(600, retention_save, p);
}

static AdwSpinRow *
spin(Page *p, const gchar *title, const gchar *subtitle, double lo, double hi, double step)
{
  AdwSpinRow *r = ADW_SPIN_ROW(adw_spin_row_new_with_range(lo, hi, step));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(r), subtitle);
  g_signal_connect(r, "notify::value", G_CALLBACK(on_retention_changed), p);
  adw_preferences_group_add(p->ret_group, GTK_WIDGET(r));
  g_ptr_array_add(p->ret_rows, r);
  return r;
}

static void
retention_build(Page *p)
{
  p->ret_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->ret_group, "Storage limits");
  adw_preferences_group_set_description(p->ret_group,
    "How much the session relay may keep before evicting old events. Your own "
    "notes, your follows and bookmarks are never evicted.");
  /* A row, not an AdwBanner: non-row children of a preferences group are
   * placed after its rows, i.e. below the controls they explain. */
  p->ret_note = nss_status_row("", NULL, "dialog-information-symbolic");
  adw_preferences_group_add(p->ret_group, p->ret_note);
  p->ret_rows = g_ptr_array_new();

  p->ret_enabled = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->ret_enabled), "Limit storage");
  g_signal_connect(p->ret_enabled, "notify::active", G_CALLBACK(on_retention_changed), p);
  adw_preferences_group_add(p->ret_group, GTK_WIDGET(p->ret_enabled));
  g_ptr_array_add(p->ret_rows, p->ret_enabled);

  p->ret_cache = spin(p, "Maximum size (MB)", "0 means no limit", 0, 1048576, 64);
  p->ret_high = spin(p, "Start evicting at (%)", "Of the maximum size", 2, 99, 1);
  p->ret_low = spin(p, "Stop evicting at (%)", "Must be below the start threshold", 1, 98, 1);
  p->ret_minage = spin(p, "Keep everything newer than (days)", "Nothing younger is evicted",
                       0, 3650, 1);
  p->ret_notes = spin(p, "Keep notes for (days)", "0 keeps notes until space is needed",
                      0, 3650, 1);
  p->ret_reactions = spin(p, "Keep reactions and zaps for (days)", "0 keeps them", 0, 3650, 1);
  p->ret_interval = spin(p, "Check every (minutes)", NULL, 1, 10080, 5);

  NssRetention r;
  g_autofree gchar *path = nss_relay_conf_path();
  g_autoptr(GError) err = NULL;
  if (!nss_relay_conf_load(path, &r, &err)) {
    g_warning("%s", err->message);
    nss_retention_defaults(&r);
  }
  p->ret_loading = TRUE;
  adw_switch_row_set_active(p->ret_enabled, r.enabled);
  adw_spin_row_set_value(p->ret_cache, r.cache_max_mb);
  adw_spin_row_set_value(p->ret_high, r.high_watermark_pct);
  adw_spin_row_set_value(p->ret_low, r.low_watermark_pct);
  adw_spin_row_set_value(p->ret_minage, r.min_age_days);
  adw_spin_row_set_value(p->ret_notes, r.note_ttl_days);
  adw_spin_row_set_value(p->ret_reactions, r.reaction_ttl_days);
  adw_spin_row_set_value(p->ret_interval, r.interval_mins);
  p->ret_loading = FALSE;
  retention_update_sensitivity(p);
}

/* ══ NIP-65 relay list ═══════════════════════════════════════════════════ */

static void list_render(Page *p);

static void
set_dirty(Page *p, gboolean dirty)
{
  p->dirty = dirty;
  gtk_widget_set_sensitive(p->publish_btn, dirty && p->pubkey_hex != NULL &&
                                           p->entries->len > 0);
}

static const gchar *
usage_label(const NssRelayEntry *e)
{
  return e->read && e->write ? "Read and write" : e->read ? "Read only"
       : e->write ? "Write only" : "Unused — turn on read or write";
}

typedef struct {
  Page          *p;
  NssRelayEntry *e;
  GtkWidget     *expander;
  GtkWidget     *info;
} RowCtx;

static void
on_rw(AdwSwitchRow *sw, GParamSpec *ps, gpointer data)
{
  (void)ps;
  RowCtx *rc = data;
  gboolean v = adw_switch_row_get_active(sw);
  if (g_object_get_data(G_OBJECT(sw), "nss-write"))
    rc->e->write = v;
  else
    rc->e->read = v;
  adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(rc->expander), usage_label(rc->e));
  set_dirty(rc->p, TRUE);
}

static void
on_remove(GtkButton *b, gpointer data)
{
  (void)b;
  RowCtx *rc = data;
  Page *p = rc->p;
  g_ptr_array_remove(p->entries, rc->e); /* frees e */
  set_dirty(p, TRUE);
  list_render(p); /* destroys rc */
}

static void
show_nip11(RowCtx *rc, NssNip11Info *info, const gchar *error)
{
  if (info == NULL) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rc->info), "Relay information unavailable");
    nss_row_set_subtitle_plain(rc->info, error);
    return;
  }
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rc->info),
                                info->name ? info->name : "Unnamed relay");
  g_autofree gchar *nips = nss_nip11_format_nips(info);
  g_autofree gchar *sub = g_strdup_printf("%s%s%s\nNIPs: %s",
                                          info->software ? info->software : "unknown software",
                                          info->version ? " " : "",
                                          info->version ? info->version : "",
                                          *nips ? nips : "none listed");
  nss_row_set_subtitle_plain(rc->info, sub);
}

typedef struct {
  GWeakRef  row;
  Page     *p;
  gchar    *url;
} Nip11Req;

static void
nip11_done(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)src;
  Nip11Req *q = data;
  g_autoptr(GError) err = NULL;
  NssNip11Info *info = nss_nip11_fetch_finish(res, &err);
  g_autoptr(GtkWidget) info_row = g_weak_ref_get(&q->row);
  if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_hash_table_replace(q->p->nip11_cache, g_strdup(q->url), info);
    info = NULL;
  }
  if (info_row) {
    RowCtx *rc = g_object_get_data(G_OBJECT(info_row), "nss-rowctx");
    if (rc)
      show_nip11(rc, g_hash_table_lookup(q->p->nip11_cache, q->url),
                 err ? err->message : NULL);
  }
  nss_nip11_info_free(info);
  g_weak_ref_clear(&q->row);
  g_free(q->url);
  g_free(q);
}

static void
on_expanded(AdwExpanderRow *row, GParamSpec *ps, gpointer data)
{
  (void)ps;
  RowCtx *rc = data;
  if (!adw_expander_row_get_expanded(row))
    return;
  if (g_hash_table_contains(rc->p->nip11_cache, rc->e->url)) {
    show_nip11(rc, g_hash_table_lookup(rc->p->nip11_cache, rc->e->url),
               "the relay did not provide a NIP-11 document");
    return;
  }
  Nip11Req *q = g_new0(Nip11Req, 1);
  g_weak_ref_init(&q->row, rc->info);
  q->p = rc->p;
  q->url = g_strdup(rc->e->url);
  nss_nip11_fetch_async(rc->p->ctx->soup, rc->e->url, 5, rc->p->cancel, nip11_done, q);
}

static void
list_render(Page *p)
{
  nss_group_clear_dynamic(p->list_group);
  for (guint i = 0; i < p->entries->len; i++) {
    NssRelayEntry *e = g_ptr_array_index(p->entries, i);
    GtkWidget *x = adw_expander_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(x), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(x), e->url);
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(x), usage_label(e));
    RowCtx *rc = g_new0(RowCtx, 1);
    rc->p = p;
    rc->e = e;
    rc->expander = x;
    g_object_set_data_full(G_OBJECT(x), "nss-rowctx-owner", rc, g_free);

    GtkWidget *rm = nss_suffix_button("user-trash-symbolic", "Remove relay");
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove), rc);
    adw_expander_row_add_suffix(ADW_EXPANDER_ROW(x), rm);

    AdwSwitchRow *rd = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rd), "Read");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(rd), "Others send mentions and replies here");
    adw_switch_row_set_active(rd, e->read);
    g_signal_connect(rd, "notify::active", G_CALLBACK(on_rw), rc);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(x), GTK_WIDGET(rd));
    AdwSwitchRow *wr = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(wr), "Write");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(wr), "Your notes are published here");
    adw_switch_row_set_active(wr, e->write);
    g_object_set_data(G_OBJECT(wr), "nss-write", GINT_TO_POINTER(1));
    g_signal_connect(wr, "notify::active", G_CALLBACK(on_rw), rc);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(x), GTK_WIDGET(wr));

    rc->info = nss_info_row("Relay information", "Loading…");
    g_object_set_data(G_OBJECT(rc->info), "nss-rowctx", rc);
    adw_expander_row_add_row(ADW_EXPANDER_ROW(x), rc->info);
    g_signal_connect(x, "notify::expanded", G_CALLBACK(on_expanded), rc);
    nss_group_add_dynamic(p->list_group, x);
  }
  if (p->entries->len == 0)
    nss_group_add_dynamic(p->list_group,
      nss_info_row("No relays", p->pubkey_hex ? "Add the relays you publish to and read from"
                              : p->signer_failed ? "A signer is needed to load and publish your list"
                              : "Waiting for the signer…"));
}

static void
on_add(AdwEntryRow *row, gpointer data)
{
  Page *p = data;
  g_autoptr(GError) err = NULL;
  g_autofree gchar *url = nss_relay_url_normalize(gtk_editable_get_text(GTK_EDITABLE(row)), &err);
  if (url == NULL) {
    nss_toast(p->ctx, "%s", err->message);
    return;
  }
  for (guint i = 0; i < p->entries->len; i++)
    if (g_str_equal(((NssRelayEntry *)g_ptr_array_index(p->entries, i))->url, url)) {
      nss_toast(p->ctx, "%s is already in the list", url);
      return;
    }
  g_ptr_array_add(p->entries, nss_relay_entry_new(url, TRUE, TRUE));
  gtk_editable_set_text(GTK_EDITABLE(row), "");
  set_dirty(p, TRUE);
  list_render(p);
}

static GPtrArray *
copy_entries(GPtrArray *src)
{
  GPtrArray *a = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  for (guint i = 0; src && i < src->len; i++) {
    NssRelayEntry *e = g_ptr_array_index(src, i);
    g_ptr_array_add(a, nss_relay_entry_new(e->url, e->read, e->write));
  }
  return a;
}

static void
lists_loaded(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  NssUserLists *l = result;
  if (l == NULL) {
    p->signer_failed = TRUE;
    nss_row_set_subtitle_plain(p->list_status, error ? error->message : "Signer unavailable");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->list_status), "Cannot load your relay list");
    list_render(p);
    return;
  }
  g_free(p->pubkey_hex);
  p->pubkey_hex = g_strdup(l->pubkey_hex);
  g_strfreev(p->signer_relays);
  p->signer_relays = g_strdupv(l->signer_relays);
  g_ptr_array_unref(p->entries);
  g_ptr_array_unref(p->old_entries);
  p->old_entries = copy_entries(l->relay_list);
  if (l->relay_list) {
    p->entries = copy_entries(l->relay_list);
  } else {
    /* Nothing published yet: start from the signer's relays. */
    p->entries = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
    for (guint i = 0; l->signer_relays[i]; i++)
      g_ptr_array_add(p->entries, nss_relay_entry_new(l->signer_relays[i], TRUE, TRUE));
  }
  if (l->relay_list) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->list_status), "Published relay list");
    g_autofree gchar *s = g_strdup_printf("Loaded from %s", l->relay_list_source);
    nss_row_set_subtitle_plain(p->list_status, s);
  } else {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->list_status), "No relay list published yet");
    nss_row_set_subtitle_plain(p->list_status,
      l->signer_relays[0] ? "Starting from the signer's relays — review and publish"
                          : "Add relays and publish");
  }
  set_dirty(p, l->relay_list == NULL && p->entries->len > 0);
  list_render(p);
}

static gpointer
lists_load(gpointer data, GError **error)
{
  return nss_user_lists_load(data, FALSE, error);
}

static void
published(GtkWidget *owner, gpointer result, const GError *error, gpointer data)
{
  (void)data;
  Page *p = g_object_get_data(G_OBJECT(owner), "nss-page");
  NssPublishReport *r = result;
  gtk_widget_set_sensitive(p->publish_btn, TRUE);
  if (r == NULL) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->list_status), "Not published");
    nss_row_set_subtitle_plain(p->list_status, error ? error->message : "failed");
    return;
  }
  g_autofree gchar *text = nss_publish_report_text(r);
  gboolean ok = r->n_required > 0 && r->n_required_ok == r->n_required;
  g_autofree gchar *title = ok
    ? g_strdup_printf("Published — all %u write relays confirmed", r->n_required)
    : g_strdup_printf("Partly published — %u of %u write relays confirmed",
                      r->n_required_ok, r->n_required);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->list_status), title);
  nss_row_set_subtitle_plain(p->list_status, text);
  /* Only a complete publish becomes the new baseline: after a partial one
   * the previous list's relays must stay in the next publish's targets so
   * the ones that missed the update still get it. */
  if (ok) {
    g_ptr_array_unref(p->old_entries);
    p->old_entries = copy_entries(p->entries);
  }
  set_dirty(p, !ok);
}

static void
on_publish(GtkButton *b, gpointer data)
{
  (void)b;
  Page *p = data;
  g_autoptr(GError) err = NULL;
  g_autofree gchar *json = nss_relay_list_build(p->entries, p->pubkey_hex,
                                                g_get_real_time() / G_USEC_PER_SEC, &err);
  if (json == NULL) {
    nss_toast(p->ctx, "%s", err->message);
    return;
  }
  NssPublishJob *j = g_new0(NssPublishJob, 1);
  j->bus = g_object_ref(p->ctx->bus);
  j->unsigned_json = g_steal_pointer(&json);
  j->targets = nss_relay_list_publish_targets(p->entries, p->old_entries,
                                              (const gchar *const *)p->signer_relays,
                                              &j->required);
  if (j->required == NULL || j->required[0] == NULL) {
    nss_publish_job_free(j);
    nss_toast(p->ctx, "Mark at least one relay for writing");
    return;
  }
  gtk_widget_set_sensitive(p->publish_btn, FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->list_status), "Publishing…");
  nss_row_set_subtitle_plain(p->list_status, "Approve the request in your signer");
  nss_run(p->page, nss_publish_job_run, published, j, nss_publish_job_free,
          nss_publish_report_free);
}

static void
list_build(Page *p)
{
  p->list_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(p->list_group, "Your relays");
  adw_preferences_group_set_description(p->list_group,
    "Your public relay list (NIP-65, kind 10002): where your notes are published "
    "and where others send you mentions. Publishing asks your signer to sign it and "
    "also updates relays you remove, so they stop serving the old list.");
  p->add_row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->add_row), "Add relay (wss://…)");
  adw_entry_row_set_show_apply_button(p->add_row, TRUE);
  gtk_editable_set_enable_undo(GTK_EDITABLE(p->add_row), TRUE);
  g_signal_connect(p->add_row, "apply", G_CALLBACK(on_add), p);
  g_signal_connect(p->add_row, "entry-activated", G_CALLBACK(on_add), p);
  adw_preferences_group_add(p->list_group, GTK_WIDGET(p->add_row));

  p->list_status = nss_info_row("Loading your relay list…", NULL);
  p->publish_btn = gtk_button_new_with_label("Publish");
  gtk_widget_add_css_class(p->publish_btn, "suggested-action");
  gtk_widget_set_valign(p->publish_btn, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(p->publish_btn, FALSE);
  g_signal_connect(p->publish_btn, "clicked", G_CALLBACK(on_publish), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->list_status), p->publish_btn);
  adw_preferences_group_add(p->list_group, p->list_status);

  p->entries = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  p->old_entries = g_ptr_array_new_with_free_func((GDestroyNotify)nss_relay_entry_free);
  list_render(p);
}

/* ══ Page ════════════════════════════════════════════════════════════════ */

static void
page_free(gpointer data)
{
  Page *p = data;
  stop_stats(p);
  if (p->units_timer)
    g_source_remove(p->units_timer);
  if (p->watch_id)
    g_bus_unwatch_name(p->watch_id);
  if (p->ret_save_id) {
    g_source_remove(p->ret_save_id);
    retention_save_now(p, FALSE);
  }
  g_cancellable_cancel(p->cancel);
  g_clear_object(&p->cancel);
  nss_relay_stats_clear(&p->stats);
  g_clear_pointer(&p->ret_rows, g_ptr_array_unref);
  g_clear_pointer(&p->entries, g_ptr_array_unref);
  g_clear_pointer(&p->old_entries, g_ptr_array_unref);
  g_strfreev(p->signer_relays);
  g_free(p->pubkey_hex);
  g_clear_pointer(&p->nip11_cache, g_hash_table_unref);
  nss_context_unref(p->ctx);
  g_free(p);
}

static GtkWidget *
add_stat(AdwPreferencesGroup *g, const gchar *title)
{
  GtkWidget *r = nss_info_row(title, "—");
  adw_preferences_group_add(g, r);
  return r;
}

AdwPreferencesPage *
nss_page_relays_new(NssContext *ctx)
{
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_page_set_title(page, "Relays");
  adw_preferences_page_set_icon_name(page, "network-server-symbolic");
  Page *p = g_new0(Page, 1);
  p->ctx = nss_context_ref(ctx);
  p->page = GTK_WIDGET(page);
  p->cancel = g_cancellable_new();
  p->nip11_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         (GDestroyNotify)nss_nip11_info_free);
  g_object_set_data_full(G_OBJECT(page), "nss-page", p, page_free);

  AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(g, "Session relay");
  adw_preferences_group_set_description(g,
    "A private relay for Nostr apps on this computer. It shares what apps have "
    "already fetched so they start faster and work offline.");
  p->enable = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->enable), "Session relay");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(p->enable), "Starts with your session");
  g_signal_connect(p->enable, "notify::active", G_CALLBACK(on_enable), p);
  adw_preferences_group_add(g, GTK_WIDGET(p->enable));

  p->status = nss_info_row("Checking…", NULL);
  p->restart = gtk_button_new_with_label("Restart");
  gtk_widget_set_valign(p->restart, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(p->restart, FALSE);
  g_signal_connect(p->restart, "clicked", G_CALLBACK(on_restart), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->status), p->restart);
  p->start_now = gtk_button_new_with_label("Start now");
  gtk_widget_set_valign(p->start_now, GTK_ALIGN_CENTER);
  gtk_widget_set_visible(p->start_now, FALSE);
  g_signal_connect(p->start_now, "clicked", G_CALLBACK(on_start_now), p);
  adw_action_row_add_suffix(ADW_ACTION_ROW(p->status), p->start_now);
  adw_preferences_group_add(g, p->status);

  p->notify_note = nss_status_row("Notifications will start it again",
    "nostr-notify is enabled and asks for the session relay when it starts", "dialog-information-symbolic");
  gtk_widget_set_visible(p->notify_note, FALSE);
  adw_preferences_group_add(g, p->notify_note);

  p->storage = add_stat(g, "Disk used");
  p->clients = add_stat(g, "Connected apps");
  p->uptime = add_stat(g, "Relay uptime");
  p->nips = add_stat(g, "Supported NIPs");
  adw_preferences_page_add(page, g);

  retention_build(p);
  adw_preferences_page_add(page, p->ret_group);

  list_build(p);
  adw_preferences_page_add(page, p->list_group);

  show_idle_stats(p);
  if (ctx->bus) {
    p->watch_id = g_bus_watch_name_on_connection(ctx->bus, NSS_RELAY_BUS_NAME,
                                                 G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                 on_name_appeared, on_name_vanished, p, NULL);
    g_signal_connect(page, "map", G_CALLBACK(on_map), p);
    g_signal_connect(page, "unmap", G_CALLBACK(on_unmap), p);
    nss_run(GTK_WIDGET(page), lists_load, lists_loaded, g_object_ref(ctx->bus),
            g_object_unref, (GDestroyNotify)nss_user_lists_free);
  } else {
    gtk_widget_set_sensitive(GTK_WIDGET(p->enable), FALSE);
  }
  return page;
}
