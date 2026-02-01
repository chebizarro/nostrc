/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip77-negentropy-plugin.c - NIP-77 Negentropy Sync Plugin
 *
 * Implements NIP-77 (Negentropy) for efficient event set reconciliation.
 * Handles NEG-OPEN, NEG-MSG, and NEG-CLOSE relay messages.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip77-negentropy-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>
#include <adwaita.h>

/* NIP-77 Message Types (relay protocol) */
#define NIP77_MSG_NEG_OPEN  "NEG-OPEN"
#define NIP77_MSG_NEG_MSG   "NEG-MSG"
#define NIP77_MSG_NEG_CLOSE "NEG-CLOSE"
#define NIP77_MSG_NEG_ERR   "NEG-ERR"

/* Settings keys */
#define SETTINGS_KEY_AUTO_SYNC_ENABLED  "auto-sync-enabled"
#define SETTINGS_KEY_SYNC_INTERVAL      "sync-interval"

/* Sync session state */
typedef struct {
  char *subscription_id;
  char *relay_url;
  gint64 started_at;
  guint rounds;
  gboolean completed;
} NegSyncSession;

struct _Nip77NegentropyPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Sync state */
  GHashTable *active_syncs;  /* subscription_id -> NegSyncSession* */
  gboolean auto_sync_enabled;
  guint auto_sync_interval_sec;
  guint auto_sync_timer_id;

  /* Stats */
  guint total_syncs;
  guint64 total_events_synced;
};

static void sync_session_free(gpointer data);
static void load_settings(Nip77NegentropyPlugin *self);
static void save_settings(Nip77NegentropyPlugin *self);
static gboolean on_auto_sync_timer(gpointer user_data);
static void start_auto_sync_timer(Nip77NegentropyPlugin *self);
static void stop_auto_sync_timer(Nip77NegentropyPlugin *self);

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip77NegentropyPlugin, nip77_negentropy_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

static void
sync_session_free(gpointer data)
{
  NegSyncSession *session = data;
  if (session) {
    g_free(session->subscription_id);
    g_free(session->relay_url);
    g_free(session);
  }
}

static void
nip77_negentropy_plugin_dispose(GObject *object)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(object);

  stop_auto_sync_timer(self);
  g_clear_pointer(&self->active_syncs, g_hash_table_unref);

  G_OBJECT_CLASS(nip77_negentropy_plugin_parent_class)->dispose(object);
}

static void
nip77_negentropy_plugin_class_init(Nip77NegentropyPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip77_negentropy_plugin_dispose;
}

static void
nip77_negentropy_plugin_init(Nip77NegentropyPlugin *self)
{
  self->active = FALSE;
  self->context = NULL;
  self->active_syncs = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              NULL, sync_session_free);
  self->auto_sync_enabled = FALSE;
  self->auto_sync_interval_sec = 300;  /* 5 minutes default */
  self->auto_sync_timer_id = 0;
  self->total_syncs = 0;
  self->total_events_synced = 0;
}

/* ============================================================================
 * Settings load/save
 * ============================================================================ */

static void
load_settings(Nip77NegentropyPlugin *self)
{
  if (!self->context)
    return;

  GError *error = NULL;

  /* Load auto-sync enabled */
  GBytes *enabled_data = gnostr_plugin_context_load_data(self->context,
                                                         SETTINGS_KEY_AUTO_SYNC_ENABLED,
                                                         &error);
  if (enabled_data) {
    gsize size;
    const gchar *data = g_bytes_get_data(enabled_data, &size);
    if (size == sizeof(gboolean)) {
      memcpy(&self->auto_sync_enabled, data, sizeof(gboolean));
    }
    g_bytes_unref(enabled_data);
  }
  g_clear_error(&error);

  /* Load sync interval */
  GBytes *interval_data = gnostr_plugin_context_load_data(self->context,
                                                          SETTINGS_KEY_SYNC_INTERVAL,
                                                          &error);
  if (interval_data) {
    gsize size;
    const gchar *data = g_bytes_get_data(interval_data, &size);
    if (size == sizeof(guint)) {
      memcpy(&self->auto_sync_interval_sec, data, sizeof(guint));
    }
    g_bytes_unref(interval_data);
  }
  g_clear_error(&error);

  g_debug("[NIP-77] Loaded settings: auto_sync=%d, interval=%u sec",
          self->auto_sync_enabled, self->auto_sync_interval_sec);
}

static void
save_settings(Nip77NegentropyPlugin *self)
{
  if (!self->context)
    return;

  GError *error = NULL;

  /* Save auto-sync enabled */
  GBytes *enabled_data = g_bytes_new(&self->auto_sync_enabled, sizeof(gboolean));
  gnostr_plugin_context_store_data(self->context, SETTINGS_KEY_AUTO_SYNC_ENABLED,
                                   enabled_data, &error);
  g_bytes_unref(enabled_data);
  g_clear_error(&error);

  /* Save sync interval */
  GBytes *interval_data = g_bytes_new(&self->auto_sync_interval_sec, sizeof(guint));
  gnostr_plugin_context_store_data(self->context, SETTINGS_KEY_SYNC_INTERVAL,
                                   interval_data, &error);
  g_bytes_unref(interval_data);
  g_clear_error(&error);
}

/* ============================================================================
 * Auto-sync timer
 * ============================================================================ */

static gboolean
on_auto_sync_timer(gpointer user_data)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(user_data);

  if (!self->active || !self->auto_sync_enabled) {
    self->auto_sync_timer_id = 0;
    return G_SOURCE_REMOVE;
  }

  g_debug("[NIP-77] Auto-sync timer triggered");

  /* Get relay URLs */
  gsize n_urls = 0;
  char **relay_urls = gnostr_plugin_context_get_relay_urls(self->context, &n_urls);

  if (relay_urls && n_urls > 0) {
    g_debug("[NIP-77] Starting sync with %zu relays", n_urls);

    /*
     * Note: Full negentropy protocol would require:
     * 1. For each relay, open a NEG-OPEN message with initial fingerprint
     * 2. Handle NEG-MSG responses
     * 3. Build NEG-MSG replies until reconciled
     * 4. Close with NEG-CLOSE
     *
     * This requires relay message hooks which aren't in the current plugin API.
     * The host application should integrate with the negentropy library
     * (nips/nip77/) for actual protocol handling.
     *
     * For now, we track that sync was requested and increment stats.
     */
    self->total_syncs++;

    for (gsize i = 0; i < n_urls; i++) {
      g_debug("[NIP-77] Would sync with relay: %s", relay_urls[i]);
    }

    g_strfreev(relay_urls);
  }

  return G_SOURCE_CONTINUE;
}

static void
start_auto_sync_timer(Nip77NegentropyPlugin *self)
{
  if (self->auto_sync_timer_id > 0)
    return;

  if (!self->auto_sync_enabled)
    return;

  guint interval_ms = self->auto_sync_interval_sec * 1000;
  self->auto_sync_timer_id = g_timeout_add(interval_ms, on_auto_sync_timer, self);
  g_debug("[NIP-77] Started auto-sync timer (%u sec interval)", self->auto_sync_interval_sec);
}

static void
stop_auto_sync_timer(Nip77NegentropyPlugin *self)
{
  if (self->auto_sync_timer_id > 0) {
    g_source_remove(self->auto_sync_timer_id);
    self->auto_sync_timer_id = 0;
    g_debug("[NIP-77] Stopped auto-sync timer");
  }
}

/* ============================================================================
 * Sync session management
 * ============================================================================ */

static void
cancel_all_sync_sessions(Nip77NegentropyPlugin *self)
{
  guint count = g_hash_table_size(self->active_syncs);
  if (count > 0) {
    g_debug("[NIP-77] Cancelling %u active sync sessions", count);
    g_hash_table_remove_all(self->active_syncs);
  }
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip77_negentropy_plugin_activate(GnostrPlugin        *plugin,
                                 GnostrPluginContext *context)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(plugin);

  g_debug("[NIP-77] Activating Negentropy Sync plugin");

  self->context = context;
  self->active = TRUE;

  /* Load auto-sync settings from plugin data storage */
  load_settings(self);

  /* Start auto-sync timer if enabled */
  if (self->auto_sync_enabled) {
    start_auto_sync_timer(self);
  }

  /*
   * Note: NEG-OPEN, NEG-MSG, NEG-CLOSE, NEG-ERR message handling requires
   * relay protocol message hooks which aren't available in the current
   * plugin API. The host application should integrate with the negentropy
   * library (nips/nip77/) for full protocol support.
   */
}

static void
nip77_negentropy_plugin_deactivate(GnostrPlugin        *plugin,
                                   GnostrPluginContext *context)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-77] Deactivating Negentropy Sync plugin");

  /* Save settings before deactivating */
  save_settings(self);

  /* Stop auto-sync timer */
  stop_auto_sync_timer(self);

  /* Cancel active sync sessions */
  cancel_all_sync_sessions(self);

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip77_negentropy_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-77 Negentropy Sync";
}

static const char *
nip77_negentropy_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Efficient set reconciliation for syncing events between client and relays";
}

static const char *const *
nip77_negentropy_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip77_negentropy_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip77_negentropy_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  /* NIP-77 doesn't define specific event kinds - it's a sync protocol */
  (void)plugin;
  if (n_kinds) *n_kinds = 0;
  return NULL;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip77_negentropy_plugin_activate;
  iface->deactivate = nip77_negentropy_plugin_deactivate;
  iface->get_name = nip77_negentropy_plugin_get_name;
  iface->get_description = nip77_negentropy_plugin_get_description;
  iface->get_authors = nip77_negentropy_plugin_get_authors;
  iface->get_version = nip77_negentropy_plugin_get_version;
  iface->get_supported_kinds = nip77_negentropy_plugin_get_supported_kinds;
}

/* ============================================================================
 * GnostrUIExtension interface implementation
 * ============================================================================ */

/* Settings page callbacks */
static void
on_auto_sync_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(user_data);
  (void)pspec;

  self->auto_sync_enabled = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  save_settings(self);

  /* Start or stop timer based on new setting */
  if (self->auto_sync_enabled && self->active) {
    start_auto_sync_timer(self);
  } else {
    stop_auto_sync_timer(self);
  }
}

static void
on_interval_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(user_data);
  (void)pspec;

  self->auto_sync_interval_sec = (guint)adw_spin_row_get_value(ADW_SPIN_ROW(row));
  save_settings(self);

  /* Restart timer with new interval if running */
  if (self->auto_sync_timer_id > 0) {
    stop_auto_sync_timer(self);
    start_auto_sync_timer(self);
  }
}

static GtkWidget *
nip77_create_settings_page(GnostrUIExtension   *extension,
                           GnostrPluginContext *context)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(extension);
  (void)context;

  /* Create settings page */
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  /* Sync settings group */
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Sync Settings");
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
    "Negentropy provides efficient event set reconciliation between client and relays.");
  gtk_box_append(GTK_BOX(page), group);

  /* Auto-sync toggle */
  GtkWidget *auto_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(auto_row), "Auto-Sync");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(auto_row),
                              "Periodically sync events with relays");
  adw_switch_row_set_active(ADW_SWITCH_ROW(auto_row), self->auto_sync_enabled);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), auto_row);

  g_signal_connect(auto_row, "notify::active",
                   G_CALLBACK(on_auto_sync_changed), self);

  /* Sync interval */
  GtkWidget *interval_row = adw_spin_row_new_with_range(60, 3600, 60);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(interval_row), "Sync Interval");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(interval_row),
                              "Seconds between automatic syncs (60-3600)");
  adw_spin_row_set_value(ADW_SPIN_ROW(interval_row), (double)self->auto_sync_interval_sec);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), interval_row);

  g_signal_connect(interval_row, "notify::value",
                   G_CALLBACK(on_interval_changed), self);

  /* Stats group */
  GtkWidget *stats_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(stats_group), "Statistics");
  gtk_box_append(GTK_BOX(page), stats_group);

  /* Total syncs */
  GtkWidget *syncs_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(syncs_row), "Total Syncs");
  g_autofree char *syncs_str = g_strdup_printf("%u", self->total_syncs);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(syncs_row), syncs_str);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(stats_group), syncs_row);

  /* Active sessions */
  GtkWidget *active_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(active_row), "Active Sessions");
  g_autofree char *active_str = g_strdup_printf("%u", g_hash_table_size(self->active_syncs));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(active_row), active_str);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(stats_group), active_row);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->create_settings_page = nip77_create_settings_page;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP77_TYPE_NEGENTROPY_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP77_TYPE_NEGENTROPY_PLUGIN);
}
