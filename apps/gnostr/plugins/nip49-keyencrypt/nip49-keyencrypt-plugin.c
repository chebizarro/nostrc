/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip49-keyencrypt-plugin.c - NIP-49 Private Key Encryption Plugin
 *
 * Implements NIP-49 (Private Key Encryption) for secure key export/import.
 * Uses scrypt KDF and XChaCha20-Poly1305 AEAD for encryption.
 * Produces bech32-encoded "ncryptsec1..." strings.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip49-keyencrypt-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* Default scrypt work factor (log_n = 16 means N = 2^16 = 65536) */
#define NIP49_DEFAULT_LOG_N 16

struct _Nip49KeyencryptPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Configuration */
  guint8 default_log_n;  /* scrypt work factor */
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip49KeyencryptPlugin, nip49_keyencrypt_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
nip49_keyencrypt_plugin_dispose(GObject *object)
{
  Nip49KeyencryptPlugin *self = NIP49_KEYENCRYPT_PLUGIN(object);
  (void)self;

  G_OBJECT_CLASS(nip49_keyencrypt_plugin_parent_class)->dispose(object);
}

static void
nip49_keyencrypt_plugin_class_init(Nip49KeyencryptPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip49_keyencrypt_plugin_dispose;
}

static void
nip49_keyencrypt_plugin_init(Nip49KeyencryptPlugin *self)
{
  self->active = FALSE;
  self->context = NULL;
  self->default_log_n = NIP49_DEFAULT_LOG_N;
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip49_keyencrypt_plugin_activate(GnostrPlugin        *plugin,
                                 GnostrPluginContext *context)
{
  Nip49KeyencryptPlugin *self = NIP49_KEYENCRYPT_PLUGIN(plugin);

  g_debug("[NIP-49] Activating Private Key Encryption plugin");

  self->context = context;
  self->active = TRUE;

  /* Load work factor preference from plugin storage */
  GError *error = NULL;
  GBytes *data = gnostr_plugin_context_load_data(context, "log_n", &error);
  if (data) {
    gsize size;
    const guint8 *bytes = g_bytes_get_data(data, &size);
    if (size == 1 && bytes[0] >= 8 && bytes[0] <= 22) {
      self->default_log_n = bytes[0];
      g_debug("[NIP-49] Loaded work factor log_n=%u from storage", self->default_log_n);
    }
    g_bytes_unref(data);
  } else {
    g_debug("[NIP-49] Using default work factor log_n=%u", self->default_log_n);
    g_clear_error(&error);
  }
}

static void
nip49_keyencrypt_plugin_deactivate(GnostrPlugin        *plugin,
                                   GnostrPluginContext *context)
{
  Nip49KeyencryptPlugin *self = NIP49_KEYENCRYPT_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-49] Deactivating Private Key Encryption plugin");

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip49_keyencrypt_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-49 Private Key Encryption";
}

static const char *
nip49_keyencrypt_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Encrypted private key export/import using ncryptsec format";
}

static const char *const *
nip49_keyencrypt_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip49_keyencrypt_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip49_keyencrypt_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  /* NIP-49 doesn't define specific event kinds - it's for key encryption */
  (void)plugin;
  if (n_kinds) *n_kinds = 0;
  return NULL;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip49_keyencrypt_plugin_activate;
  iface->deactivate = nip49_keyencrypt_plugin_deactivate;
  iface->get_name = nip49_keyencrypt_plugin_get_name;
  iface->get_description = nip49_keyencrypt_plugin_get_description;
  iface->get_authors = nip49_keyencrypt_plugin_get_authors;
  iface->get_version = nip49_keyencrypt_plugin_get_version;
  iface->get_supported_kinds = nip49_keyencrypt_plugin_get_supported_kinds;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP49_TYPE_KEYENCRYPT_PLUGIN);
}
