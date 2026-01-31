/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip98-httpauth-plugin.c - NIP-98 HTTP Auth Plugin
 *
 * Implements NIP-98 (HTTP Auth) for signing HTTP requests with Nostr events.
 * Handles event kind 27235 for HTTP authentication.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip98-httpauth-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* NIP-98 HTTP Auth Event Kind */
#define NIP98_KIND_HTTP_AUTH 27235

struct _Nip98HttpAuthPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip98HttpAuthPlugin, nip98_httpauth_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
nip98_httpauth_plugin_dispose(GObject *object)
{
  Nip98HttpAuthPlugin *self = NIP98_HTTPAUTH_PLUGIN(object);
  (void)self;

  G_OBJECT_CLASS(nip98_httpauth_plugin_parent_class)->dispose(object);
}

static void
nip98_httpauth_plugin_class_init(Nip98HttpAuthPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip98_httpauth_plugin_dispose;
}

static void
nip98_httpauth_plugin_init(Nip98HttpAuthPlugin *self)
{
  self->active = FALSE;
  self->context = NULL;
}

/* GnostrPlugin interface implementation */

static void
nip98_httpauth_plugin_activate(GnostrPlugin        *plugin,
                               GnostrPluginContext *context)
{
  Nip98HttpAuthPlugin *self = NIP98_HTTPAUTH_PLUGIN(plugin);

  g_debug("[NIP-98] Activating HTTP Auth plugin");

  self->context = context;
  self->active = TRUE;

  /* TODO: Register as HTTP auth provider for Blossom uploads */
}

static void
nip98_httpauth_plugin_deactivate(GnostrPlugin        *plugin,
                                 GnostrPluginContext *context)
{
  Nip98HttpAuthPlugin *self = NIP98_HTTPAUTH_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-98] Deactivating HTTP Auth plugin");

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip98_httpauth_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-98 HTTP Auth";
}

static const char *
nip98_httpauth_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "HTTP authentication using Nostr events (kind 27235)";
}

static const char *const *
nip98_httpauth_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip98_httpauth_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip98_httpauth_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  static const int kinds[] = { NIP98_KIND_HTTP_AUTH };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip98_httpauth_plugin_activate;
  iface->deactivate = nip98_httpauth_plugin_deactivate;
  iface->get_name = nip98_httpauth_plugin_get_name;
  iface->get_description = nip98_httpauth_plugin_get_description;
  iface->get_authors = nip98_httpauth_plugin_get_authors;
  iface->get_version = nip98_httpauth_plugin_get_version;
  iface->get_supported_kinds = nip98_httpauth_plugin_get_supported_kinds;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP98_TYPE_HTTPAUTH_PLUGIN);
}
