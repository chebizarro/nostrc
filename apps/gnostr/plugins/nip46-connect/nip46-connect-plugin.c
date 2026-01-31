/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip46-connect-plugin.c - NIP-46 Nostr Connect Plugin
 *
 * Implements NIP-46 (Nostr Connect) for remote signing via bunker protocol.
 * Handles event kind 24133 for request/response messages.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip46-connect-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* NIP-46 Event Kind */
#define NIP46_KIND_NOSTR_CONNECT 24133

struct _Nip46ConnectPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Connection state */
  gchar *bunker_pubkey;
  gchar *client_secret;
  gchar **relay_urls;
  gboolean connected;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip46ConnectPlugin, nip46_connect_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
nip46_connect_plugin_dispose(GObject *object)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(object);

  g_clear_pointer(&self->bunker_pubkey, g_free);
  g_clear_pointer(&self->client_secret, g_free);
  g_clear_pointer(&self->relay_urls, g_strfreev);

  G_OBJECT_CLASS(nip46_connect_plugin_parent_class)->dispose(object);
}

static void
nip46_connect_plugin_class_init(Nip46ConnectPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip46_connect_plugin_dispose;
}

static void
nip46_connect_plugin_init(Nip46ConnectPlugin *self)
{
  self->active = FALSE;
  self->context = NULL;
  self->connected = FALSE;
}

/* GnostrPlugin interface implementation */

static void
nip46_connect_plugin_activate(GnostrPlugin        *plugin,
                              GnostrPluginContext *context)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(plugin);

  g_debug("[NIP-46] Activating Nostr Connect plugin");

  self->context = context;
  self->active = TRUE;

  /* TODO: Load saved bunker connection from settings */
  /* TODO: Auto-reconnect if configured */
}

static void
nip46_connect_plugin_deactivate(GnostrPlugin        *plugin,
                                GnostrPluginContext *context)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-46] Deactivating Nostr Connect plugin");

  self->active = FALSE;
  self->context = NULL;
  self->connected = FALSE;

  /* TODO: Disconnect from bunker gracefully */
}

static const char *
nip46_connect_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-46 Nostr Connect";
}

static const char *
nip46_connect_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Remote signing via Nostr Connect bunker protocol";
}

static const char *const *
nip46_connect_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip46_connect_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip46_connect_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  static const int kinds[] = { NIP46_KIND_NOSTR_CONNECT };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip46_connect_plugin_activate;
  iface->deactivate = nip46_connect_plugin_deactivate;
  iface->get_name = nip46_connect_plugin_get_name;
  iface->get_description = nip46_connect_plugin_get_description;
  iface->get_authors = nip46_connect_plugin_get_authors;
  iface->get_version = nip46_connect_plugin_get_version;
  iface->get_supported_kinds = nip46_connect_plugin_get_supported_kinds;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP46_TYPE_CONNECT_PLUGIN);
}
