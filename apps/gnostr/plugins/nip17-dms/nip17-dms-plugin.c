/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip17-dms-plugin.c - NIP-17 Private Direct Messages Plugin
 *
 * Implements NIP-17 (Private Direct Messages) using gift-wrapped encryption:
 * - Kind 14 (chat message/rumor, unsigned)
 * - Kind 13 (seal, signed wrapper)
 * - Kind 1059 (gift wrap, final container with ephemeral sender)
 * - Kind 10050 (DM relay list preferences)
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip17-dms-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* NIP-17 Event Kinds */
#define NIP17_KIND_DIRECT_MESSAGE 14
#define NIP17_KIND_SEAL           13
#define NIP17_KIND_GIFT_WRAP      1059
#define NIP17_KIND_DM_RELAY_LIST  10050

struct _Nip17DmsPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Subscription for incoming gift-wrapped DMs */
  guint64 gift_wrap_subscription;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrEventHandler interface */
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip17DmsPlugin, nip17_dms_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

static void
nip17_dms_plugin_dispose(GObject *object)
{
  Nip17DmsPlugin *self = NIP17_DMS_PLUGIN(object);

  if (self->gift_wrap_subscription > 0 && self->context)
    {
      gnostr_plugin_context_unsubscribe_events(self->context, self->gift_wrap_subscription);
      self->gift_wrap_subscription = 0;
    }

  G_OBJECT_CLASS(nip17_dms_plugin_parent_class)->dispose(object);
}

static void
nip17_dms_plugin_class_init(Nip17DmsPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip17_dms_plugin_dispose;
}

static void
nip17_dms_plugin_init(Nip17DmsPlugin *self)
{
  self->active = FALSE;
  self->context = NULL;
  self->gift_wrap_subscription = 0;
}

/* ============================================================================
 * Event subscription callback
 * ============================================================================ */

static void
on_gift_wrap_received(GnostrPluginContext *context G_GNUC_UNUSED,
                      const char          *event_json,
                      gpointer             user_data)
{
  Nip17DmsPlugin *self = NIP17_DMS_PLUGIN(user_data);
  (void)self;

  g_debug("[NIP-17] Received gift-wrapped DM: %.64s...", event_json);

  /* TODO: Decrypt and process the gift-wrapped DM
   * 1. Parse event JSON
   * 2. Verify it's kind 1059
   * 3. Check p-tag matches our pubkey
   * 4. Decrypt gift wrap to get seal (kind 13)
   * 5. Verify seal signature
   * 6. Decrypt seal to get rumor (kind 14)
   * 7. Verify seal pubkey matches rumor pubkey
   * 8. Extract message content and sender
   * 9. Store in DM conversation
   * 10. Emit notification
   */
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip17_dms_plugin_activate(GnostrPlugin        *plugin,
                          GnostrPluginContext *context)
{
  Nip17DmsPlugin *self = NIP17_DMS_PLUGIN(plugin);

  g_debug("[NIP-17] Activating Private DMs plugin");

  self->context = context;
  self->active = TRUE;

  /* Subscribe to incoming gift-wrapped events for the current user */
  const char *user_pubkey = gnostr_plugin_context_get_user_pubkey(context);
  if (user_pubkey)
    {
      /* Subscribe to gift wraps addressed to us (p-tag matches our pubkey) */
      g_autofree char *filter = g_strdup_printf(
        "{\"kinds\":[%d],\"#p\":[\"%s\"]}",
        NIP17_KIND_GIFT_WRAP,
        user_pubkey);

      self->gift_wrap_subscription = gnostr_plugin_context_subscribe_events(
        context,
        filter,
        G_CALLBACK(on_gift_wrap_received),
        self,
        NULL);

      g_debug("[NIP-17] Subscribed to gift wraps for pubkey: %.16s...", user_pubkey);
    }
  else
    {
      g_debug("[NIP-17] No user logged in, deferring subscription");
    }

  /* TODO: Register UI hooks for:
   * - DM inbox view
   * - Conversation thread view
   * - Compose DM dialog
   * - Unread indicators in sidebar
   */
}

static void
nip17_dms_plugin_deactivate(GnostrPlugin        *plugin,
                            GnostrPluginContext *context)
{
  Nip17DmsPlugin *self = NIP17_DMS_PLUGIN(plugin);

  g_debug("[NIP-17] Deactivating Private DMs plugin");

  /* Unsubscribe from event notifications */
  if (self->gift_wrap_subscription > 0)
    {
      gnostr_plugin_context_unsubscribe_events(context, self->gift_wrap_subscription);
      self->gift_wrap_subscription = 0;
    }

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip17_dms_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-17 Private DMs";
}

static const char *
nip17_dms_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Private direct messages using gift-wrapped encryption (NIP-17)";
}

static const char *const *
nip17_dms_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip17_dms_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip17_dms_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  static const int kinds[] = {
    NIP17_KIND_DIRECT_MESSAGE,  /* Kind 14: Chat message (rumor) */
    NIP17_KIND_SEAL,            /* Kind 13: Seal */
    NIP17_KIND_GIFT_WRAP,       /* Kind 1059: Gift wrap */
    NIP17_KIND_DM_RELAY_LIST,   /* Kind 10050: DM relay preferences */
  };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip17_dms_plugin_activate;
  iface->deactivate = nip17_dms_plugin_deactivate;
  iface->get_name = nip17_dms_plugin_get_name;
  iface->get_description = nip17_dms_plugin_get_description;
  iface->get_authors = nip17_dms_plugin_get_authors;
  iface->get_version = nip17_dms_plugin_get_version;
  iface->get_supported_kinds = nip17_dms_plugin_get_supported_kinds;
}

/* ============================================================================
 * GnostrEventHandler interface implementation
 * ============================================================================ */

static gboolean
nip17_dms_plugin_handle_event(GnostrEventHandler  *handler,
                              GnostrPluginContext *context,
                              GnostrPluginEvent   *event)
{
  Nip17DmsPlugin *self = NIP17_DMS_PLUGIN(handler);
  (void)self;

  int kind = gnostr_plugin_event_get_kind(event);

  switch (kind)
    {
    case NIP17_KIND_GIFT_WRAP:
      {
        /* Handle incoming gift-wrapped DM */
        const char *event_id = gnostr_plugin_event_get_id(event);
        const char *sender = gnostr_plugin_event_get_pubkey(event);
        g_debug("[NIP-17] Handling gift wrap %s from %s", event_id, sender);

        /* Verify recipient tag matches current user */
        const char *user_pubkey = gnostr_plugin_context_get_user_pubkey(context);
        const char *p_tag = gnostr_plugin_event_get_tag_value(event, "p", 0);

        if (!user_pubkey || !p_tag || g_strcmp0(user_pubkey, p_tag) != 0)
          {
            g_debug("[NIP-17] Gift wrap not addressed to us, skipping");
            return FALSE;
          }

        /* TODO: Full decryption pipeline:
         * 1. Get user's secret key (via signer)
         * 2. Decrypt gift wrap content (NIP-44) using sender's ephemeral pubkey
         * 3. Parse seal event from decrypted content
         * 4. Verify seal signature
         * 5. Decrypt seal content (NIP-44) using seal pubkey
         * 6. Parse rumor event from decrypted content
         * 7. Verify seal.pubkey == rumor.pubkey
         * 8. Extract message and conversation metadata
         * 9. Store in local DM database
         * 10. Notify UI of new message
         */

        return TRUE;  /* Event handled */
      }

    case NIP17_KIND_DM_RELAY_LIST:
      {
        /* Handle DM relay list update (kind 10050) */
        const char *pubkey = gnostr_plugin_event_get_pubkey(event);
        g_debug("[NIP-17] Received DM relay list update from %s", pubkey);

        /* TODO: Parse relay tags and cache for sending DMs to this user */
        return TRUE;
      }

    default:
      /* Kinds 13 and 14 are internal (inside encrypted containers) */
      return FALSE;
    }
}

static gboolean
nip17_dms_plugin_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  (void)handler;

  switch (kind)
    {
    case NIP17_KIND_GIFT_WRAP:
    case NIP17_KIND_DM_RELAY_LIST:
      return TRUE;
    default:
      return FALSE;
    }
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->handle_event = nip17_dms_plugin_handle_event;
  iface->can_handle_kind = nip17_dms_plugin_can_handle_kind;
}

/* ============================================================================
 * GnostrUIExtension interface implementation - Settings page
 * ============================================================================ */

static GtkWidget *
nip17_dms_plugin_create_settings_page(GnostrUIExtension   *extension,
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
  GtkWidget *title = gtk_label_new("Private Direct Messages (NIP-17)");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(page), title);

  /* Description */
  GtkWidget *desc = gtk_label_new(
    "End-to-end encrypted direct messages using gift-wrapped encryption.\n\n"
    "Messages are wrapped in multiple layers of encryption:\n"
    "• Kind 14 (rumor) - The actual message content\n"
    "• Kind 13 (seal) - Signed wrapper hiding the sender\n"
    "• Kind 1059 (gift wrap) - Final encrypted container");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_box_append(GTK_BOX(page), desc);

  /* DM Relay Preferences section */
  GtkWidget *relay_frame = gtk_frame_new("DM Relay Preferences (Kind 10050)");
  gtk_widget_set_margin_top(relay_frame, 12);

  GtkWidget *relay_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(relay_box, 12);
  gtk_widget_set_margin_end(relay_box, 12);
  gtk_widget_set_margin_top(relay_box, 8);
  gtk_widget_set_margin_bottom(relay_box, 12);

  GtkWidget *relay_desc = gtk_label_new(
    "Specify preferred relays for receiving DMs. Other users will check "
    "your kind 10050 event to know where to send encrypted messages.");
  gtk_label_set_wrap(GTK_LABEL(relay_desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(relay_desc), 0);
  gtk_widget_add_css_class(relay_desc, "dim-label");
  gtk_box_append(GTK_BOX(relay_box), relay_desc);

  /* Placeholder for relay list editor */
  GtkWidget *relay_placeholder = gtk_label_new("(Relay list editor coming soon)");
  gtk_widget_add_css_class(relay_placeholder, "dim-label");
  gtk_widget_set_margin_top(relay_placeholder, 8);
  gtk_box_append(GTK_BOX(relay_box), relay_placeholder);

  gtk_frame_set_child(GTK_FRAME(relay_frame), relay_box);
  gtk_box_append(GTK_BOX(page), relay_frame);

  /* Status section */
  GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(status_box, 12);

  GtkWidget *status_label = gtk_label_new("Status:");
  gtk_box_append(GTK_BOX(status_box), status_label);

  GtkWidget *status_value = gtk_label_new("Plugin loaded (subscription pending login)");
  gtk_widget_add_css_class(status_value, "dim-label");
  gtk_box_append(GTK_BOX(status_box), status_value);

  gtk_box_append(GTK_BOX(page), status_box);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->create_settings_page = nip17_dms_plugin_create_settings_page;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP17_TYPE_DMS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_EVENT_HANDLER,
                                              NIP17_TYPE_DMS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP17_TYPE_DMS_PLUGIN);
}
