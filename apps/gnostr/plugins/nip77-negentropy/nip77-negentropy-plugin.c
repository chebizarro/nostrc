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

/* NIP-77 Message Types (relay protocol) */
#define NIP77_MSG_NEG_OPEN  "NEG-OPEN"
#define NIP77_MSG_NEG_MSG   "NEG-MSG"
#define NIP77_MSG_NEG_CLOSE "NEG-CLOSE"
#define NIP77_MSG_NEG_ERR   "NEG-ERR"

struct _Nip77NegentropyPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Sync state */
  GHashTable *active_syncs;  /* subscription_id -> sync session */
  gboolean auto_sync_enabled;
  guint auto_sync_interval_sec;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip77NegentropyPlugin, nip77_negentropy_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
nip77_negentropy_plugin_dispose(GObject *object)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(object);

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
  self->active_syncs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->auto_sync_enabled = FALSE;
  self->auto_sync_interval_sec = 300;  /* 5 minutes default */
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

  /* TODO: Register message handlers for NEG-OPEN, NEG-MSG, NEG-CLOSE, NEG-ERR */
  /* TODO: Load auto-sync settings from GSettings */
  /* TODO: Start auto-sync timer if enabled */
}

static void
nip77_negentropy_plugin_deactivate(GnostrPlugin        *plugin,
                                   GnostrPluginContext *context)
{
  Nip77NegentropyPlugin *self = NIP77_NEGENTROPY_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-77] Deactivating Negentropy Sync plugin");

  /* TODO: Stop auto-sync timer */
  /* TODO: Cancel active sync sessions */

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
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP77_TYPE_NEGENTROPY_PLUGIN);
}
