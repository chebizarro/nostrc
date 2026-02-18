/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip57-zaps-plugin.c - NIP-57 Lightning Zaps Plugin
 *
 * Implements NIP-57 (Lightning Zaps) for sending and receiving zaps.
 * Handles event kinds 9734 (zap request) and 9735 (zap receipt).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip57-zaps-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>
#include <json-glib/json-glib.h>
#include <adwaita.h>

/* NIP-57 Event Kinds */
#define ZAP_KIND_REQUEST  9734
#define ZAP_KIND_RECEIPT  9735

/* Settings keys */
#define SETTINGS_KEY_DEFAULT_AMOUNT "default-zap-amount"
#define SETTINGS_KEY_SHOW_BUTTON    "show-zap-button"
#define SETTINGS_KEY_PRESETS        "amount-presets"

struct _Nip57ZapsPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Zap settings */
  gint64 default_zap_amount;      /* Default zap amount in msats */
  gint64 *amount_presets;         /* Array of preset amounts */
  gsize n_presets;
  gboolean show_zap_button;

  /* Subscriptions */
  guint64 receipt_subscription;   /* Subscription for zap receipts */

  /* Cached zap stats per event */
  GHashTable *zap_stats;          /* event_id -> ZapStats */
};

typedef struct {
  gint64 total_msats;
  guint zap_count;
} ZapStats;

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrEventHandler interface */
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip57ZapsPlugin, nip57_zaps_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

static void
zap_stats_free(gpointer data)
{
  g_free(data);
}

/* ============================================================================
 * Settings load/save helpers
 * ============================================================================ */

static void
load_settings(Nip57ZapsPlugin *self)
{
  if (!self->context)
    return;

  GError *error = NULL;

  /* Load default amount */
  GBytes *amount_data = gnostr_plugin_context_load_data(self->context,
                                                        SETTINGS_KEY_DEFAULT_AMOUNT,
                                                        &error);
  if (amount_data) {
    gsize size;
    const gchar *data = g_bytes_get_data(amount_data, &size);
    if (size == sizeof(gint64)) {
      memcpy(&self->default_zap_amount, data, sizeof(gint64));
    }
    g_bytes_unref(amount_data);
  }
  g_clear_error(&error);

  /* Load show button setting */
  GBytes *button_data = gnostr_plugin_context_load_data(self->context,
                                                        SETTINGS_KEY_SHOW_BUTTON,
                                                        &error);
  if (button_data) {
    gsize size;
    const gchar *data = g_bytes_get_data(button_data, &size);
    if (size == sizeof(gboolean)) {
      memcpy(&self->show_zap_button, data, sizeof(gboolean));
    }
    g_bytes_unref(button_data);
  }
  g_clear_error(&error);

  g_debug("[NIP-57] Loaded settings: default_amount=%" G_GINT64_FORMAT " msats, show_button=%d",
          self->default_zap_amount, self->show_zap_button);
}

static void
save_settings(Nip57ZapsPlugin *self)
{
  if (!self->context)
    return;

  GError *error = NULL;

  /* Save default amount */
  GBytes *amount_data = g_bytes_new(&self->default_zap_amount, sizeof(gint64));
  gnostr_plugin_context_store_data(self->context, SETTINGS_KEY_DEFAULT_AMOUNT,
                                   amount_data, &error);
  g_bytes_unref(amount_data);
  g_clear_error(&error);

  /* Save show button setting */
  GBytes *button_data = g_bytes_new(&self->show_zap_button, sizeof(gboolean));
  gnostr_plugin_context_store_data(self->context, SETTINGS_KEY_SHOW_BUTTON,
                                   button_data, &error);
  g_bytes_unref(button_data);
  g_clear_error(&error);
}

/* ============================================================================
 * Zap receipt callback
 * ============================================================================ */

static void
on_zap_receipt_received(GnostrPluginContext *context G_GNUC_UNUSED,
                        const char          *event_json,
                        gpointer             user_data)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(user_data);

  g_debug("[NIP-57] Received zap receipt: %.64s...", event_json);

  /* Parse the receipt to extract zap amount and target event */
  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("[NIP-57] Failed to parse zap receipt: %s", error->message);
    g_error_free(error);
    return;
  }

  JsonNode *root = json_parser_get_root(parser);
  JsonObject *event = json_node_get_object(root);
  JsonArray *tags = json_object_get_array_member(event, "tags");

  const char *target_event_id = NULL;
  gint64 amount_msats = 0;

  /* Find the 'e' tag (target event) and 'bolt11' or 'amount' */
  guint n_tags = json_array_get_length(tags);
  for (guint i = 0; i < n_tags; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (json_array_get_length(tag) < 2)
      continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    if (g_strcmp0(tag_name, "e") == 0) {
      target_event_id = json_array_get_string_element(tag, 1);
    } else if (g_strcmp0(tag_name, "amount") == 0) {
      const char *amount_str = json_array_get_string_element(tag, 1);
      amount_msats = g_ascii_strtoll(amount_str, NULL, 10);
    }
  }

  /* Update cached stats for the target event */
  if (target_event_id && amount_msats > 0) {
    ZapStats *stats = g_hash_table_lookup(self->zap_stats, target_event_id);
    if (!stats) {
      stats = g_new0(ZapStats, 1);
      g_hash_table_insert(self->zap_stats, g_strdup(target_event_id), stats);
    }
    stats->total_msats += amount_msats;
    stats->zap_count++;

    g_debug("[NIP-57] Updated stats for %s: %" G_GINT64_FORMAT " msats, %u zaps",
            target_event_id, stats->total_msats, stats->zap_count);
  }

}

static void
nip57_zaps_plugin_dispose(GObject *object)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(object);

  g_clear_pointer(&self->amount_presets, g_free);
  g_clear_pointer(&self->zap_stats, g_hash_table_unref);

  G_OBJECT_CLASS(nip57_zaps_plugin_parent_class)->dispose(object);
}

static void
nip57_zaps_plugin_class_init(Nip57ZapsPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip57_zaps_plugin_dispose;
}

static void
nip57_zaps_plugin_init(Nip57ZapsPlugin *self)
{
  /* Default settings */
  self->default_zap_amount = 21000;  /* 21 sats in msats */
  self->show_zap_button = TRUE;

  /* Default presets: 21, 100, 500, 1000, 5000 sats (in msats) */
  self->n_presets = 5;
  self->amount_presets = g_new(gint64, self->n_presets);
  self->amount_presets[0] = 21000;
  self->amount_presets[1] = 100000;
  self->amount_presets[2] = 500000;
  self->amount_presets[3] = 1000000;
  self->amount_presets[4] = 5000000;

  self->zap_stats = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free, zap_stats_free);
}

/* GnostrPlugin interface implementation */

static void
nip57_zaps_plugin_activate(GnostrPlugin        *plugin,
                           GnostrPluginContext *context)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(plugin);

  g_debug("[NIP-57] Activating Lightning Zaps plugin");

  self->context = context;
  self->active = TRUE;

  /* Load saved settings from plugin data storage */
  load_settings(self);

  /* Subscribe to zap receipt events for the current user */
  const char *user_pubkey = gnostr_plugin_context_get_user_pubkey(context);
  if (user_pubkey) {
    /* Create filter for zap receipts targeting the current user */
    g_autofree char *filter = g_strdup_printf(
      "{\"kinds\":[%d],\"#p\":[\"%s\"]}",
      ZAP_KIND_RECEIPT, user_pubkey);

    self->receipt_subscription = gnostr_plugin_context_subscribe_events(
      context,
      filter,
      G_CALLBACK(on_zap_receipt_received),
      self,
      NULL);

    if (self->receipt_subscription > 0) {
      g_debug("[NIP-57] Subscribed to zap receipts for %s", user_pubkey);
    } else {
      g_warning("[NIP-57] Failed to subscribe to zap receipts");
    }
  } else {
    g_debug("[NIP-57] No user logged in, skipping zap receipt subscription");
  }
}

static void
nip57_zaps_plugin_deactivate(GnostrPlugin        *plugin,
                             GnostrPluginContext *context)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(plugin);

  g_debug("[NIP-57] Deactivating Lightning Zaps plugin");

  /* Save settings before deactivating */
  save_settings(self);

  /* Cancel active subscriptions */
  if (self->receipt_subscription > 0 && self->context) {
    gnostr_plugin_context_unsubscribe_events(context, self->receipt_subscription);
    self->receipt_subscription = 0;
  }

  /* Clear cached stats */
  g_hash_table_remove_all(self->zap_stats);

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip57_zaps_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-57 Lightning Zaps";
}

static const char *
nip57_zaps_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Lightning Network zaps for sending and receiving payments on notes";
}

static const char *const *
nip57_zaps_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip57_zaps_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip57_zaps_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  static const int kinds[] = { ZAP_KIND_REQUEST, ZAP_KIND_RECEIPT };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip57_zaps_plugin_activate;
  iface->deactivate = nip57_zaps_plugin_deactivate;
  iface->get_name = nip57_zaps_plugin_get_name;
  iface->get_description = nip57_zaps_plugin_get_description;
  iface->get_authors = nip57_zaps_plugin_get_authors;
  iface->get_version = nip57_zaps_plugin_get_version;
  iface->get_supported_kinds = nip57_zaps_plugin_get_supported_kinds;
}

/* ============================================================================
 * GnostrEventHandler interface implementation
 * ============================================================================ */

static gboolean
nip57_handle_event(GnostrEventHandler  *handler,
                   GnostrPluginContext *context,
                   GnostrPluginEvent   *event)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(handler);
  (void)context;

  int kind = gnostr_plugin_event_get_kind(event);

  if (kind == ZAP_KIND_RECEIPT) {
    /* Process zap receipt */
    const char *event_id = gnostr_plugin_event_get_id(event);
    g_debug("[NIP-57] Processing zap receipt: %s", event_id);

    /* Extract target event from 'e' tag */
    const char *target_id = gnostr_plugin_event_get_tag_value(event, "e", 0);
    const char *amount_str = gnostr_plugin_event_get_tag_value(event, "amount", 0);

    if (target_id && amount_str) {
      gint64 amount_msats = g_ascii_strtoll(amount_str, NULL, 10);

      /* Update cached stats */
      ZapStats *stats = g_hash_table_lookup(self->zap_stats, target_id);
      if (!stats) {
        stats = g_new0(ZapStats, 1);
        g_hash_table_insert(self->zap_stats, g_strdup(target_id), stats);
      }
      stats->total_msats += amount_msats;
      stats->zap_count++;
    }

    return TRUE;  /* Event was handled */
  }

  return FALSE;  /* Pass to other handlers */
}

static gboolean
nip57_can_handle_kind(GnostrEventHandler *handler,
                      int                 kind)
{
  (void)handler;
  return (kind == ZAP_KIND_REQUEST || kind == ZAP_KIND_RECEIPT);
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->handle_event = nip57_handle_event;
  iface->can_handle_kind = nip57_can_handle_kind;
}

/* ============================================================================
 * GnostrUIExtension interface implementation
 * ============================================================================ */

static GtkWidget *
nip57_create_note_decoration(GnostrUIExtension   *extension,
                             GnostrPluginContext *context,
                             GnostrPluginEvent   *event)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(extension);
  (void)context;

  if (!self->show_zap_button)
    return NULL;

  /* Only add zap decoration to notes (kind 1) */
  int kind = gnostr_plugin_event_get_kind(event);
  if (kind != 1)
    return NULL;

  const char *event_id = gnostr_plugin_event_get_id(event);
  if (!event_id)
    return NULL;

  /* Get cached zap stats for this event */
  ZapStats *stats = g_hash_table_lookup(self->zap_stats, event_id);

  /* Create zap info box */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class(box, "zap-decoration");

  /* Zap icon */
  GtkWidget *icon = gtk_image_new_from_icon_name("flash-symbolic");
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_box_append(GTK_BOX(box), icon);

  /* Zap count/amount label */
  g_autofree char *label_text = NULL;
  if (stats && stats->zap_count > 0) {
    /* Format amount in sats */
    gint64 sats = stats->total_msats / 1000;
    if (sats >= 1000000) {
      label_text = g_strdup_printf("%.1fM sats (%u)", (double)sats / 1000000.0, stats->zap_count);
    } else if (sats >= 1000) {
      label_text = g_strdup_printf("%.1fk sats (%u)", (double)sats / 1000.0, stats->zap_count);
    } else {
      label_text = g_strdup_printf("%" G_GINT64_FORMAT " sats (%u)", sats, stats->zap_count);
    }
  } else {
    label_text = g_strdup("0 sats");
  }

  GtkWidget *label = gtk_label_new(label_text);
  gtk_widget_add_css_class(label, "caption");
  gtk_widget_add_css_class(label, "dim-label");
  gtk_box_append(GTK_BOX(box), label);

  return box;
}

/* Settings page callbacks */
static void
on_show_button_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(user_data);
  (void)pspec;

  self->show_zap_button = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  save_settings(self);
}

static void
on_default_amount_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(user_data);
  (void)pspec;

  self->default_zap_amount = (gint64)adw_spin_row_get_value(ADW_SPIN_ROW(row));
  save_settings(self);
}

static GtkWidget *
nip57_create_settings_page(GnostrUIExtension   *extension,
                           GnostrPluginContext *context)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(extension);
  (void)context;

  /* Create settings page using Adwaita widgets */
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

  /* Preferences group for zap settings */
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Zap Settings");
  gtk_box_append(GTK_BOX(page), group);

  /* Show zap button toggle */
  GtkWidget *show_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(show_row), "Show Zap Button");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(show_row),
                              "Display zap statistics on notes");
  adw_switch_row_set_active(ADW_SWITCH_ROW(show_row), self->show_zap_button);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), show_row);

  /* Bind switch to update setting */
  g_signal_connect(show_row, "notify::active",
                   G_CALLBACK(on_show_button_changed), self);

  /* Default amount setting */
  GtkWidget *amount_row = adw_spin_row_new_with_range(1000, 10000000, 1000);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(amount_row), "Default Zap Amount");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(amount_row),
                              "Amount in millisatoshis (21000 = 21 sats)");
  adw_spin_row_set_value(ADW_SPIN_ROW(amount_row), (double)self->default_zap_amount);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), amount_row);

  /* Bind spinner to update setting */
  g_signal_connect(amount_row, "notify::value",
                   G_CALLBACK(on_default_amount_changed), self);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->create_note_decoration = nip57_create_note_decoration;
  iface->create_settings_page = nip57_create_settings_page;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP57_TYPE_ZAPS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_EVENT_HANDLER,
                                              NIP57_TYPE_ZAPS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP57_TYPE_ZAPS_PLUGIN);
}
