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

/* NIP-57 Event Kinds */
#define ZAP_KIND_REQUEST  9734
#define ZAP_KIND_RECEIPT  9735

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

G_DEFINE_TYPE_WITH_CODE(Nip57ZapsPlugin, nip57_zaps_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
zap_stats_free(gpointer data)
{
  g_free(data);
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

  /* TODO: Load saved settings from GSettings */

  /* TODO: Subscribe to zap receipt events for the current user */
  /* This would allow real-time updates when zaps are received */

  /* TODO: Register UI hooks for zap button on notes */
}

static void
nip57_zaps_plugin_deactivate(GnostrPlugin        *plugin,
                             GnostrPluginContext *context)
{
  Nip57ZapsPlugin *self = NIP57_ZAPS_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-57] Deactivating Lightning Zaps plugin");

  /* TODO: Cancel any active subscriptions when API is implemented */
  /* if (self->receipt_subscription > 0 && self->context) {
   *   gnostr_plugin_context_unsubscribe_events(self->context, self->receipt_subscription);
   *   self->receipt_subscription = 0;
   * }
   */
  self->receipt_subscription = 0;

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
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP57_TYPE_ZAPS_PLUGIN);
}
