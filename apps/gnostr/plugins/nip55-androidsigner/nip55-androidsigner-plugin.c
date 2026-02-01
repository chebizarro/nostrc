/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip55-androidsigner-plugin.c - NIP-55 Android Signer Plugin
 *
 * Implements NIP-55 (Android Signer Application) for external key management.
 * On Android: uses intents to communicate with signer apps.
 * On Linux: wraps DBus signer interface (nip55l).
 * On other platforms: graceful no-op.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip55-androidsigner-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

#ifdef __ANDROID__
#include <android/log.h>
#define NIP55_PLATFORM_ANDROID 1
#elif defined(__linux__)
#define NIP55_PLATFORM_LINUX 1
#endif

struct _Nip55AndroidsignerPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Signer state */
  gchar *signer_package;      /* Android: package name of selected signer */
  gchar *signer_npub;         /* Cached public key from signer */
  gboolean signer_available;  /* Whether a signer was detected */
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip55AndroidsignerPlugin, nip55_androidsigner_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
nip55_androidsigner_plugin_dispose(GObject *object)
{
  Nip55AndroidsignerPlugin *self = NIP55_ANDROIDSIGNER_PLUGIN(object);

  g_clear_pointer(&self->signer_package, g_free);
  g_clear_pointer(&self->signer_npub, g_free);

  G_OBJECT_CLASS(nip55_androidsigner_plugin_parent_class)->dispose(object);
}

static void
nip55_androidsigner_plugin_class_init(Nip55AndroidsignerPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip55_androidsigner_plugin_dispose;
}

static void
nip55_androidsigner_plugin_init(Nip55AndroidsignerPlugin *self)
{
  self->active = FALSE;
  self->context = NULL;
  self->signer_package = NULL;
  self->signer_npub = NULL;
  self->signer_available = FALSE;
}

/* ============================================================================
 * Platform-specific signer detection
 * ============================================================================ */

#ifdef NIP55_PLATFORM_ANDROID

static gboolean
nip55_detect_android_signers(Nip55AndroidsignerPlugin *self)
{
  /*
   * Android signer detection requires JNI to query PackageManager for apps handling:
   *   - android.intent.action.VIEW
   *   - android.intent.category.DEFAULT
   *   - scheme: nostrsigner
   *
   * Known signer packages:
   *   - com.greenart7c3.nostrsigner (Amber)
   *   - com.example.nostrsigner (example)
   *
   * This code path is only compiled for NIP55_PLATFORM_ANDROID.
   * JNI implementation would call:
   *   PackageManager.queryIntentActivities(intent, 0)
   * and iterate through ResolveInfo results.
   */
  g_debug("[NIP-55] Android signer detection not implemented (requires JNI)");
  (void)self;

  /* Return FALSE - no signers detected without JNI implementation */
  return FALSE;
}

#elif defined(NIP55_PLATFORM_LINUX)

static gboolean
nip55_detect_dbus_signer(Nip55AndroidsignerPlugin *self)
{
  /* Check if org.nostr.Signer is available on the session bus */
  GError *error = NULL;
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

  if (conn == NULL) {
    g_debug("[NIP-55] No DBus session bus: %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  /* Check if the service is registered or can be activated */
  GVariant *result = g_dbus_connection_call_sync(
      conn,
      "org.freedesktop.DBus",
      "/org/freedesktop/DBus",
      "org.freedesktop.DBus",
      "NameHasOwner",
      g_variant_new("(s)", "org.nostr.Signer"),
      G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NONE,
      1000,
      NULL,
      &error);

  g_object_unref(conn);

  if (result == NULL) {
    g_debug("[NIP-55] DBus check failed: %s", error->message);
    g_error_free(error);
    return FALSE;
  }

  gboolean has_owner = FALSE;
  g_variant_get(result, "(b)", &has_owner);
  g_variant_unref(result);

  g_debug("[NIP-55] DBus signer available: %s", has_owner ? "yes" : "no");
  return has_owner;
}

#endif

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip55_androidsigner_plugin_activate(GnostrPlugin        *plugin,
                                    GnostrPluginContext *context)
{
  Nip55AndroidsignerPlugin *self = NIP55_ANDROIDSIGNER_PLUGIN(plugin);

  g_debug("[NIP-55] Activating Android Signer plugin");

  self->context = context;
  self->active = TRUE;

#ifdef NIP55_PLATFORM_ANDROID
  self->signer_available = nip55_detect_android_signers(self);
  if (self->signer_available) {
    g_debug("[NIP-55] Android signer detected");
  }
#elif defined(NIP55_PLATFORM_LINUX)
  self->signer_available = nip55_detect_dbus_signer(self);
  if (self->signer_available) {
    g_debug("[NIP-55] Linux DBus signer detected");
  }
#else
  g_debug("[NIP-55] No signer support on this platform");
  self->signer_available = FALSE;
#endif
}

static void
nip55_androidsigner_plugin_deactivate(GnostrPlugin        *plugin,
                                      GnostrPluginContext *context)
{
  Nip55AndroidsignerPlugin *self = NIP55_ANDROIDSIGNER_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-55] Deactivating Android Signer plugin");

  self->active = FALSE;
  self->context = NULL;
  self->signer_available = FALSE;
}

static const char *
nip55_androidsigner_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-55 Android Signer";
}

static const char *
nip55_androidsigner_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "External key management via Android Signer Application";
}

static const char *const *
nip55_androidsigner_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip55_androidsigner_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip55_androidsigner_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  /* NIP-55 doesn't define specific event kinds - it's a signing interface */
  (void)plugin;
  if (n_kinds) *n_kinds = 0;
  return NULL;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip55_androidsigner_plugin_activate;
  iface->deactivate = nip55_androidsigner_plugin_deactivate;
  iface->get_name = nip55_androidsigner_plugin_get_name;
  iface->get_description = nip55_androidsigner_plugin_get_description;
  iface->get_authors = nip55_androidsigner_plugin_get_authors;
  iface->get_version = nip55_androidsigner_plugin_get_version;
  iface->get_supported_kinds = nip55_androidsigner_plugin_get_supported_kinds;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP55_TYPE_ANDROIDSIGNER_PLUGIN);
}
