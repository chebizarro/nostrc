/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip47-nwc-plugin.c - NIP-47 Nostr Wallet Connect Plugin
 *
 * Implements NIP-47 (Nostr Wallet Connect) for Lightning wallet integration.
 * Handles event kinds 13194 (info), 23194 (request), 23195 (response).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip47-nwc-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* NIP-47 Event Kinds */
#define NWC_KIND_INFO     13194
#define NWC_KIND_REQUEST  23194
#define NWC_KIND_RESPONSE 23195

struct _Nip47NwcPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Connection state */
  gchar *wallet_pubkey;
  gchar *connection_secret;
  gchar **relay_urls;

  /* Pending requests */
  GHashTable *pending_requests;  /* request_id -> callback data */
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip47NwcPlugin, nip47_nwc_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
nip47_nwc_plugin_dispose(GObject *object)
{
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(object);

  g_clear_pointer(&self->wallet_pubkey, g_free);
  g_clear_pointer(&self->connection_secret, g_free);
  g_clear_pointer(&self->relay_urls, g_strfreev);
  g_clear_pointer(&self->pending_requests, g_hash_table_unref);

  G_OBJECT_CLASS(nip47_nwc_plugin_parent_class)->dispose(object);
}

static void
nip47_nwc_plugin_class_init(Nip47NwcPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip47_nwc_plugin_dispose;
}

static void
nip47_nwc_plugin_init(Nip47NwcPlugin *self)
{
  self->pending_requests = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, NULL);
}

/* GnostrPlugin interface implementation */

static void
nip47_nwc_plugin_activate(GnostrPlugin        *plugin,
                          GnostrPluginContext *context)
{
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(plugin);

  g_debug("[NIP-47] Activating Nostr Wallet Connect plugin");

  self->context = context;
  self->active = TRUE;

  /* TODO: Load saved connection from settings */
  /* TODO: Subscribe to NWC response events */
}

static void
nip47_nwc_plugin_deactivate(GnostrPlugin        *plugin,
                            GnostrPluginContext *context)
{
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-47] Deactivating Nostr Wallet Connect plugin");

  self->active = FALSE;
  self->context = NULL;

  /* Cancel pending requests */
  g_hash_table_remove_all(self->pending_requests);
}

static const char *
nip47_nwc_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-47 Nostr Wallet Connect";
}

static const char *
nip47_nwc_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Lightning wallet integration via Nostr Wallet Connect protocol";
}

static const char *const *
nip47_nwc_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip47_nwc_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip47_nwc_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  static const int kinds[] = { NWC_KIND_INFO, NWC_KIND_REQUEST, NWC_KIND_RESPONSE };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip47_nwc_plugin_activate;
  iface->deactivate = nip47_nwc_plugin_deactivate;
  iface->get_name = nip47_nwc_plugin_get_name;
  iface->get_description = nip47_nwc_plugin_get_description;
  iface->get_authors = nip47_nwc_plugin_get_authors;
  iface->get_version = nip47_nwc_plugin_get_version;
  iface->get_supported_kinds = nip47_nwc_plugin_get_supported_kinds;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP47_TYPE_NWC_PLUGIN);
}
