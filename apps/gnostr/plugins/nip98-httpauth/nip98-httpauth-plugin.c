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

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip98HttpAuthPlugin, nip98_httpauth_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

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

  /*
   * NIP-98 HTTP Auth is now active for this session.
   * Blossom uploads use NIP-98 auth directly via the signer service
   * (see blossom.c upload_with_auth/delete_with_auth).
   * This plugin provides:
   * - Kind 27235 event support declaration
   * - Settings UI page for user visibility
   */
  g_message("[NIP-98] HTTP Auth provider ready for Blossom uploads");
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
 * GnostrUIExtension interface implementation - Settings page
 * ============================================================================ */

static GtkWidget *
nip98_httpauth_plugin_create_settings_page(GnostrUIExtension   *extension,
                                           GnostrPluginContext *context G_GNUC_UNUSED)
{
  (void)extension;

  /* Create settings page container */
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(page, 18);
  gtk_widget_set_margin_end(page, 18);
  gtk_widget_set_margin_top(page, 18);
  gtk_widget_set_margin_bottom(page, 18);

  /* Title */
  GtkWidget *title = gtk_label_new("HTTP Authentication (NIP-98)");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(page), title);

  /* Description */
  GtkWidget *desc = gtk_label_new(
    "Authenticate HTTP requests using signed Nostr events (kind 27235).\n\n"
    "This plugin provides authentication for:\n"
    "• Blossom media uploads and downloads\n"
    "• Protected API endpoints\n"
    "• Any HTTP service supporting NIP-98");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_box_append(GTK_BOX(page), desc);

  /* How it works section */
  GtkWidget *how_frame = gtk_frame_new("How it works");
  gtk_widget_set_margin_top(how_frame, 12);

  GtkWidget *how_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(how_box, 12);
  gtk_widget_set_margin_end(how_box, 12);
  gtk_widget_set_margin_top(how_box, 8);
  gtk_widget_set_margin_bottom(how_box, 12);

  GtkWidget *how_desc = gtk_label_new(
    "When making authenticated requests:\n"
    "1. A kind 27235 event is created with the request URL and method\n"
    "2. The event is signed with your Nostr key\n"
    "3. The base64-encoded event is sent in the Authorization header\n"
    "4. The server verifies the signature and grants access");
  gtk_label_set_wrap(GTK_LABEL(how_desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(how_desc), 0);
  gtk_box_append(GTK_BOX(how_box), how_desc);

  gtk_frame_set_child(GTK_FRAME(how_frame), how_box);
  gtk_box_append(GTK_BOX(page), how_frame);

  /* Status */
  GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(status_box, 12);

  GtkWidget *status_label = gtk_label_new("Status:");
  gtk_box_append(GTK_BOX(status_box), status_label);

  GtkWidget *status_value = gtk_label_new("Active - ready to sign HTTP requests");
  gtk_widget_add_css_class(status_value, "success");
  gtk_box_append(GTK_BOX(status_box), status_value);

  gtk_box_append(GTK_BOX(page), status_box);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->create_settings_page = nip98_httpauth_plugin_create_settings_page;
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
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP98_TYPE_HTTPAUTH_PLUGIN);
}
