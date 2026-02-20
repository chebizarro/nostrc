/* SPDX-License-Identifier: GPL-3.0-or-later
 * mls-groups-plugin.c - MLS Group Messaging Plugin
 *
 * Implements the Marmot protocol (MIP-00 through MIP-04) for secure
 * group messaging over Nostr using MLS (RFC 9420).
 *
 * This plugin handles:
 * - Key package creation and publication (kind:443)
 * - Welcome message processing (kind:444 via NIP-59)
 * - Group message encryption/decryption (kind:445)
 * - Group lifecycle management (create, join, leave)
 * - Chat UI for group conversations
 *
 * Interoperable with Whitenoise and MDK-compatible clients.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "mls-groups-plugin.h"
#include "gn-marmot-service.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* Marmot event kinds */
#define MLS_KIND_KEY_PACKAGE        443
#define MLS_KIND_WELCOME            444
#define MLS_KIND_GROUP_MESSAGE      445
#define MLS_KIND_GIFT_WRAP          1059
#define MLS_KIND_KP_RELAY_LIST      10051

/* Supported event kinds for the event handler interface */
static const int SUPPORTED_KINDS[] = {
  MLS_KIND_KEY_PACKAGE,
  MLS_KIND_WELCOME,
  MLS_KIND_GROUP_MESSAGE,
  MLS_KIND_GIFT_WRAP,
  MLS_KIND_KP_RELAY_LIST,
};
static const gsize N_SUPPORTED_KINDS = G_N_ELEMENTS(SUPPORTED_KINDS);

/* Panel IDs */
#define PANEL_ID_GROUP_CHATS  "mls-group-chats"
#define PANEL_ID_INVITATIONS  "mls-invitations"

struct _MlsGroupsPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean             active;

  /* Event subscriptions */
  guint64 gift_wrap_subscription;    /* kind:1059 addressed to us */
  guint64 group_msg_subscription;    /* kind:445 for our groups */
  guint64 key_package_subscription;  /* kind:443 */
};

/* ══════════════════════════════════════════════════════════════════════════
 * Interface implementations
 * ══════════════════════════════════════════════════════════════════════════ */

static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MlsGroupsPlugin, mls_groups_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN,
                                              gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER,
                                              gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION,
                                              gnostr_ui_extension_iface_init))

/* ══════════════════════════════════════════════════════════════════════════
 * GObject lifecycle
 * ══════════════════════════════════════════════════════════════════════════ */

static void
mls_groups_plugin_dispose(GObject *object)
{
  MlsGroupsPlugin *self = MLS_GROUPS_PLUGIN(object);

  if (self->context)
    {
      if (self->gift_wrap_subscription > 0)
        {
          gnostr_plugin_context_unsubscribe_events(self->context,
                                                   self->gift_wrap_subscription);
          self->gift_wrap_subscription = 0;
        }
      if (self->group_msg_subscription > 0)
        {
          gnostr_plugin_context_unsubscribe_events(self->context,
                                                   self->group_msg_subscription);
          self->group_msg_subscription = 0;
        }
      if (self->key_package_subscription > 0)
        {
          gnostr_plugin_context_unsubscribe_events(self->context,
                                                   self->key_package_subscription);
          self->key_package_subscription = 0;
        }
    }

  G_OBJECT_CLASS(mls_groups_plugin_parent_class)->dispose(object);
}

static void
mls_groups_plugin_class_init(MlsGroupsPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = mls_groups_plugin_dispose;
}

static void
mls_groups_plugin_init(MlsGroupsPlugin *self)
{
  self->active                  = FALSE;
  self->context                 = NULL;
  self->gift_wrap_subscription  = 0;
  self->group_msg_subscription  = 0;
  self->key_package_subscription = 0;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Event subscription callbacks
 * ══════════════════════════════════════════════════════════════════════════ */

static void
on_gift_wrap_received(const gchar *event_json,
                      gpointer     user_data)
{
  MlsGroupsPlugin *self = MLS_GROUPS_PLUGIN(user_data);
  if (!self->active || !self->context)
    return;

  g_debug("MLS: Received gift wrap event — checking for welcome/group message");

  /*
   * TODO Phase 3: Unwrap NIP-59 gift wrap and route:
   *
   * 1. gnostr_nip59_unwrap_async(gift_wrap, user_pubkey, ...)
   * 2. Check inner event kind:
   *    - 444 (welcome): marmot_gobject_client_process_welcome_async()
   *    - 445 (group msg): marmot_gobject_client_process_message_async()
   * 3. Emit appropriate signal on GnMarmotService
   */
}

static void
on_group_message_received(const gchar *event_json,
                          gpointer     user_data)
{
  MlsGroupsPlugin *self = MLS_GROUPS_PLUGIN(user_data);
  if (!self->active || !self->context)
    return;

  g_debug("MLS: Received kind:445 group message event");

  /*
   * TODO Phase 3: Process group message:
   *
   * 1. Extract h tag → nostr_group_id
   * 2. NIP-44 decrypt content to get MLS ciphertext
   * 3. marmot_gobject_client_process_message_async()
   * 4. Handle result:
   *    - APPLICATION_MESSAGE: emit message-received with decrypted content
   *    - COMMIT: emit group-updated
   */
}

/* ══════════════════════════════════════════════════════════════════════════
 * GnostrPlugin interface
 * ══════════════════════════════════════════════════════════════════════════ */

static void
mls_groups_plugin_activate(GnostrPlugin       *plugin,
                           GnostrPluginContext *context)
{
  MlsGroupsPlugin *self = MLS_GROUPS_PLUGIN(plugin);

  g_info("MLS Groups plugin: activating");

  self->context = context;
  self->active  = TRUE;

  /* Initialize Marmot service */
  g_autofree gchar *data_dir = g_build_filename(
    g_get_user_data_dir(), "gnostr", NULL);

  g_autoptr(GError) error = NULL;
  GnMarmotService *service = gn_marmot_service_initialize(data_dir, &error);
  if (service == NULL)
    {
      g_warning("MLS Groups plugin: failed to initialize marmot service: %s",
                error->message);
      self->active = FALSE;
      return;
    }

  /* Set user identity if logged in */
  const gchar *user_pubkey = gnostr_plugin_context_get_user_pubkey(context);
  if (user_pubkey != NULL)
    {
      gn_marmot_service_set_user_identity(service, user_pubkey, NULL);
    }

  /* Subscribe to gift-wrapped events addressed to us (kind:1059) */
  if (user_pubkey != NULL)
    {
      g_autofree gchar *gift_wrap_filter = g_strdup_printf(
        "{\"kinds\":[%d],\"#p\":[\"%s\"],\"limit\":100}",
        MLS_KIND_GIFT_WRAP, user_pubkey);

      self->gift_wrap_subscription =
        gnostr_plugin_context_subscribe_events(context,
                                               gift_wrap_filter,
                                               G_CALLBACK(on_gift_wrap_received),
                                               self,
                                               NULL);
    }

  /* Subscribe to kind:445 group messages */
  {
    g_autofree gchar *group_msg_filter = g_strdup_printf(
      "{\"kinds\":[%d],\"limit\":500}",
      MLS_KIND_GROUP_MESSAGE);

    self->group_msg_subscription =
      gnostr_plugin_context_subscribe_events(context,
                                             group_msg_filter,
                                             G_CALLBACK(on_group_message_received),
                                             self,
                                             NULL);
  }

  /* Request recent MLS events from relays */
  {
    static const int mls_kinds[] = {
      MLS_KIND_KEY_PACKAGE,
      MLS_KIND_GIFT_WRAP,
      MLS_KIND_GROUP_MESSAGE,
      MLS_KIND_KP_RELAY_LIST,
    };

    gnostr_plugin_context_request_relay_events_async(
      context,
      mls_kinds,
      G_N_ELEMENTS(mls_kinds),
      200,   /* limit */
      NULL,  /* cancellable */
      NULL,  /* callback — fire and forget initial sync */
      NULL);
  }

  g_info("MLS Groups plugin: activated successfully");
}

static void
mls_groups_plugin_deactivate(GnostrPlugin       *plugin,
                             GnostrPluginContext *context)
{
  MlsGroupsPlugin *self = MLS_GROUPS_PLUGIN(plugin);

  g_info("MLS Groups plugin: deactivating");

  self->active = FALSE;

  /* Unsubscribe event listeners */
  if (self->gift_wrap_subscription > 0)
    {
      gnostr_plugin_context_unsubscribe_events(context, self->gift_wrap_subscription);
      self->gift_wrap_subscription = 0;
    }
  if (self->group_msg_subscription > 0)
    {
      gnostr_plugin_context_unsubscribe_events(context, self->group_msg_subscription);
      self->group_msg_subscription = 0;
    }
  if (self->key_package_subscription > 0)
    {
      gnostr_plugin_context_unsubscribe_events(context, self->key_package_subscription);
      self->key_package_subscription = 0;
    }

  /* Shut down Marmot service */
  gn_marmot_service_shutdown();

  self->context = NULL;

  g_info("MLS Groups plugin: deactivated");
}

static const char *
mls_groups_plugin_get_name(GnostrPlugin *plugin)
{
  return "MLS Group Messaging";
}

static const char *
mls_groups_plugin_get_description(GnostrPlugin *plugin)
{
  return "Secure group messaging using the Marmot protocol (MLS over Nostr). "
         "Interoperable with Whitenoise and MDK-compatible clients.";
}

static const char *const *
mls_groups_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  return authors;
}

static const char *
mls_groups_plugin_get_version(GnostrPlugin *plugin)
{
  return "1.0";
}

static const int *
mls_groups_plugin_get_supported_kinds(GnostrPlugin *plugin,
                                      gsize        *n_kinds)
{
  if (n_kinds)
    *n_kinds = N_SUPPORTED_KINDS;
  return SUPPORTED_KINDS;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate            = mls_groups_plugin_activate;
  iface->deactivate          = mls_groups_plugin_deactivate;
  iface->get_name            = mls_groups_plugin_get_name;
  iface->get_description     = mls_groups_plugin_get_description;
  iface->get_authors         = mls_groups_plugin_get_authors;
  iface->get_version         = mls_groups_plugin_get_version;
  iface->get_supported_kinds = mls_groups_plugin_get_supported_kinds;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GnostrEventHandler interface
 * ══════════════════════════════════════════════════════════════════════════ */

static gboolean
mls_groups_can_handle_kind(GnostrEventHandler *handler,
                           int                 kind)
{
  switch (kind)
    {
    case MLS_KIND_KEY_PACKAGE:
    case MLS_KIND_WELCOME:
    case MLS_KIND_GROUP_MESSAGE:
    case MLS_KIND_GIFT_WRAP:
    case MLS_KIND_KP_RELAY_LIST:
      return TRUE;
    default:
      return FALSE;
    }
}

static gboolean
mls_groups_handle_event(GnostrEventHandler  *handler,
                        GnostrPluginContext *context,
                        GnostrPluginEvent   *event)
{
  int kind = gnostr_plugin_event_get_kind(event);

  switch (kind)
    {
    case MLS_KIND_KEY_PACKAGE:
      g_debug("MLS: Received key package event (kind:443)");
      /* Key packages are consumed when creating groups / adding members.
       * They're fetched on-demand from relays, not processed inline. */
      return TRUE;

    case MLS_KIND_GIFT_WRAP:
      /* Gift wraps are handled by the subscription callback which has
       * access to the full event JSON for NIP-59 unwrapping. */
      return TRUE;

    case MLS_KIND_GROUP_MESSAGE:
      /* Group messages are handled by the subscription callback. */
      return TRUE;

    case MLS_KIND_KP_RELAY_LIST:
      g_debug("MLS: Received key package relay list (kind:10051)");
      /* TODO Phase 2: Cache relay lists for key package discovery */
      return TRUE;

    default:
      return FALSE;
    }
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->can_handle_kind = mls_groups_can_handle_kind;
  iface->handle_event    = mls_groups_handle_event;
}

/* ══════════════════════════════════════════════════════════════════════════
 * GnostrUIExtension interface
 * ══════════════════════════════════════════════════════════════════════════ */

static GList *
mls_groups_get_sidebar_items(GnostrUIExtension  *extension,
                             GnostrPluginContext *context)
{
  GList *items = NULL;

  /* Group Chats sidebar item */
  GnostrSidebarItem *chat_item = gnostr_sidebar_item_new(
    PANEL_ID_GROUP_CHATS,
    "Group Chats",
    "chat-bubble-text-symbolic");
  gnostr_sidebar_item_set_requires_auth(chat_item, TRUE);
  gnostr_sidebar_item_set_position(chat_item, 25); /* After DMs, before repos */
  items = g_list_append(items, chat_item);

  /* Invitations sidebar item */
  GnostrSidebarItem *invite_item = gnostr_sidebar_item_new(
    PANEL_ID_INVITATIONS,
    "Invitations",
    "mail-unread-symbolic");
  gnostr_sidebar_item_set_requires_auth(invite_item, TRUE);
  gnostr_sidebar_item_set_position(invite_item, 26);
  items = g_list_append(items, invite_item);

  return items;
}

static GtkWidget *
mls_groups_create_panel_widget(GnostrUIExtension  *extension,
                               GnostrPluginContext *context,
                               const char          *panel_id)
{
  if (g_strcmp0(panel_id, PANEL_ID_GROUP_CHATS) == 0)
    {
      /* TODO Phase 4: Return GnGroupListView */
      GtkWidget *placeholder = gtk_label_new("Group Chats — Coming Soon");
      gtk_widget_add_css_class(placeholder, "dim-label");
      gtk_widget_set_vexpand(placeholder, TRUE);
      gtk_widget_set_hexpand(placeholder, TRUE);
      return placeholder;
    }

  if (g_strcmp0(panel_id, PANEL_ID_INVITATIONS) == 0)
    {
      /* TODO Phase 3: Return GnWelcomeListView */
      GtkWidget *placeholder = gtk_label_new("Group Invitations — Coming Soon");
      gtk_widget_add_css_class(placeholder, "dim-label");
      gtk_widget_set_vexpand(placeholder, TRUE);
      gtk_widget_set_hexpand(placeholder, TRUE);
      return placeholder;
    }

  return NULL;
}

static GtkWidget *
mls_groups_create_settings_page(GnostrUIExtension  *extension,
                                GnostrPluginContext *context)
{
  /* TODO Phase 2: Settings for key package management, relay preferences */
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(page, 24);
  gtk_widget_set_margin_end(page, 24);
  gtk_widget_set_margin_top(page, 24);
  gtk_widget_set_margin_bottom(page, 24);

  GtkWidget *title = gtk_label_new("MLS Group Messaging");
  gtk_widget_add_css_class(title, "title-2");
  gtk_box_append(GTK_BOX(page), title);

  GtkWidget *desc = gtk_label_new(
    "Secure group messaging using the Marmot protocol.\n"
    "Interoperable with Whitenoise and other MLS clients.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_box_append(GTK_BOX(page), desc);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->get_sidebar_items   = mls_groups_get_sidebar_items;
  iface->create_panel_widget = mls_groups_create_panel_widget;
  iface->create_settings_page = mls_groups_create_settings_page;
  /* create_menu_items and create_note_decoration left NULL — not needed */
}

/* ══════════════════════════════════════════════════════════════════════════
 * libpeas module registration
 * ══════════════════════════════════════════════════════════════════════════ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_PLUGIN,
                                             MLS_TYPE_GROUPS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_EVENT_HANDLER,
                                             MLS_TYPE_GROUPS_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_UI_EXTENSION,
                                             MLS_TYPE_GROUPS_PLUGIN);
}
