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
#include <nostr/nip46/nip46_client.h>
#include <nostr/nip46/nip46_uri.h>

/* NIP-46 Event Kind */
#define NIP46_KIND_NOSTR_CONNECT 24133

struct _Nip46ConnectPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* NIP-46 client session */
  NostrNip46Session *session;

  /* Connection state */
  gchar *bunker_uri;
  gchar *remote_pubkey;
  gboolean connected;
  gboolean auto_connect;

  /* Pending sign requests (hash table: request_id -> GTask*) */
  GHashTable *pending_requests;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrEventHandler interface */
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip46ConnectPlugin, nip46_connect_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

static void
nip46_connect_plugin_dispose(GObject *object)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(object);

  /* Free NIP-46 session */
  if (self->session)
    {
      nostr_nip46_session_free(self->session);
      self->session = NULL;
    }

  g_clear_pointer(&self->bunker_uri, g_free);
  g_clear_pointer(&self->remote_pubkey, g_free);
  g_clear_pointer(&self->pending_requests, g_hash_table_unref);

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
  self->session = NULL;
  self->connected = FALSE;
  self->auto_connect = FALSE;
  self->pending_requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
}

/* ============================================================================
 * Connection management
 * ============================================================================ */

static gboolean
nip46_connect_plugin_do_connect(Nip46ConnectPlugin *self, GError **error)
{
  if (!self->bunker_uri)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_INVALID_DATA,
                          "No bunker URI configured");
      return FALSE;
    }

  /* Create new session if needed */
  if (!self->session)
    self->session = nostr_nip46_client_new();

  /* Connect to the bunker */
  int ret = nostr_nip46_client_connect(self->session, self->bunker_uri, NULL);
  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_NETWORK,
                  "Failed to connect to bunker (error %d)", ret);
      return FALSE;
    }

  /* Get the remote signer's pubkey */
  char *remote_pubkey = NULL;
  ret = nostr_nip46_session_get_remote_pubkey(self->session, &remote_pubkey);
  if (ret == 0 && remote_pubkey)
    {
      g_free(self->remote_pubkey);
      self->remote_pubkey = remote_pubkey;
    }

  self->connected = TRUE;
  g_debug("[NIP-46] Connected to bunker, remote pubkey: %s",
          self->remote_pubkey ? self->remote_pubkey : "unknown");

  return TRUE;
}

static void
nip46_connect_plugin_disconnect(Nip46ConnectPlugin *self)
{
  if (!self->connected)
    return;

  /* Free session */
  if (self->session)
    {
      nostr_nip46_session_free(self->session);
      self->session = NULL;
    }

  self->connected = FALSE;
  g_debug("[NIP-46] Disconnected from bunker");
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip46_connect_plugin_activate(GnostrPlugin        *plugin,
                              GnostrPluginContext *context)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(plugin);

  g_debug("[NIP-46] Activating Nostr Connect plugin");

  self->context = context;
  self->active = TRUE;

  /* Note: Auto-connect and persistence would use plugin context storage
   * APIs when those are implemented. For now, connection is manual only. */
}

static void
nip46_connect_plugin_deactivate(GnostrPlugin        *plugin,
                                GnostrPluginContext *context)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-46] Deactivating Nostr Connect plugin");

  nip46_connect_plugin_disconnect(self);

  self->active = FALSE;
  self->context = NULL;
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
 * GnostrEventHandler interface implementation
 * ============================================================================ */

static gboolean
nip46_connect_plugin_handle_event(GnostrEventHandler  *handler,
                                  GnostrPluginContext *context,
                                  GnostrPluginEvent   *event)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(handler);
  (void)context;
  (void)event;

  /* NIP-46 event handling requires the plugin context event APIs
   * which are not yet implemented. For now, return FALSE to indicate
   * the event was not handled. The NIP-46 library handles communication
   * directly via relay connections. */

  if (!self->connected || !self->session)
    {
      g_debug("[NIP-46] Received event but not connected, ignoring");
      return FALSE;
    }

  return FALSE;
}

static gboolean
nip46_connect_plugin_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  (void)handler;
  return kind == NIP46_KIND_NOSTR_CONNECT;
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->handle_event = nip46_connect_plugin_handle_event;
  iface->can_handle_kind = nip46_connect_plugin_can_handle_kind;
}

/* ============================================================================
 * Public API for NIP-46 operations
 * ============================================================================ */

/**
 * nip46_connect_plugin_connect:
 * @self: The plugin instance
 * @bunker_uri: The bunker:// or nostrconnect:// URI
 * @error: Return location for error
 *
 * Connect to a NIP-46 bunker.
 *
 * Returns: %TRUE on success
 */
gboolean
nip46_connect_plugin_connect(Nip46ConnectPlugin *self,
                             const char         *bunker_uri,
                             GError            **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), FALSE);
  g_return_val_if_fail(bunker_uri != NULL, FALSE);

  /* Disconnect first if already connected */
  if (self->connected)
    nip46_connect_plugin_disconnect(self);

  /* Save the new URI */
  g_free(self->bunker_uri);
  self->bunker_uri = g_strdup(bunker_uri);

  return nip46_connect_plugin_do_connect(self, error);
}

/**
 * nip46_connect_plugin_do_disconnect:
 * @self: The plugin instance
 *
 * Disconnect from the current bunker.
 */
void
nip46_connect_plugin_do_disconnect(Nip46ConnectPlugin *self)
{
  g_return_if_fail(NIP46_IS_CONNECT_PLUGIN(self));
  nip46_connect_plugin_disconnect(self);
}

/**
 * nip46_connect_plugin_is_connected:
 * @self: The plugin instance
 *
 * Check if connected to a bunker.
 *
 * Returns: %TRUE if connected
 */
gboolean
nip46_connect_plugin_is_connected(Nip46ConnectPlugin *self)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), FALSE);
  return self->connected;
}

/**
 * nip46_connect_plugin_get_remote_pubkey:
 * @self: The plugin instance
 *
 * Get the remote signer's public key.
 *
 * Returns: (transfer none) (nullable): The pubkey hex string, or %NULL
 */
const char *
nip46_connect_plugin_get_remote_pubkey(Nip46ConnectPlugin *self)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);
  return self->remote_pubkey;
}

/**
 * nip46_connect_plugin_sign_event_async:
 * @self: The plugin instance
 * @unsigned_event_json: The unsigned event JSON
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Request the bunker to sign an event asynchronously.
 */
void
nip46_connect_plugin_sign_event_async(Nip46ConnectPlugin *self,
                                      const char         *unsigned_event_json,
                                      GCancellable       *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer            user_data)
{
  g_return_if_fail(NIP46_IS_CONNECT_PLUGIN(self));
  g_return_if_fail(unsigned_event_json != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);

  if (!self->connected || !self->session)
    {
      g_task_return_new_error(task, GNOSTR_PLUGIN_ERROR,
                              GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                              "Not connected to bunker");
      g_object_unref(task);
      return;
    }

  /* Use the NIP-46 client library to sign */
  char *signed_json = NULL;
  int ret = nostr_nip46_client_sign_event(self->session, unsigned_event_json, &signed_json);

  if (ret != 0)
    {
      g_task_return_new_error(task, GNOSTR_PLUGIN_ERROR,
                              GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                              "Bunker refused to sign (error %d)", ret);
      g_object_unref(task);
      return;
    }

  g_task_return_pointer(task, signed_json, free);
  g_object_unref(task);
}

/**
 * nip46_connect_plugin_sign_event_finish:
 * @self: The plugin instance
 * @result: The #GAsyncResult
 * @error: Return location for error
 *
 * Finish an async sign operation.
 *
 * Returns: (transfer full): The signed event JSON, free with g_free()
 */
char *
nip46_connect_plugin_sign_event_finish(Nip46ConnectPlugin *self,
                                       GAsyncResult       *result,
                                       GError            **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);

  return g_task_propagate_pointer(G_TASK(result), error);
}

/**
 * nip46_connect_plugin_get_public_key:
 * @self: The plugin instance
 * @error: Return location for error
 *
 * Get the user's public key from the remote signer.
 *
 * Returns: (transfer full) (nullable): The pubkey hex string, free with g_free()
 */
char *
nip46_connect_plugin_get_public_key(Nip46ConnectPlugin *self, GError **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);

  if (!self->connected || !self->session)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                          "Not connected to bunker");
      return NULL;
    }

  char *pubkey = NULL;
  int ret = nostr_nip46_client_get_public_key(self->session, &pubkey);

  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                  "Failed to get public key (error %d)", ret);
      return NULL;
    }

  return pubkey;
}

/**
 * nip46_connect_plugin_nip04_encrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @plaintext: The plaintext to encrypt
 * @error: Return location for error
 *
 * Encrypt a message using NIP-04 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The ciphertext, free with g_free()
 */
char *
nip46_connect_plugin_nip04_encrypt(Nip46ConnectPlugin *self,
                                   const char         *peer_pubkey_hex,
                                   const char         *plaintext,
                                   GError            **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);
  g_return_val_if_fail(peer_pubkey_hex != NULL, NULL);
  g_return_val_if_fail(plaintext != NULL, NULL);

  if (!self->connected || !self->session)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                          "Not connected to bunker");
      return NULL;
    }

  char *ciphertext = NULL;
  int ret = nostr_nip46_client_nip04_encrypt(self->session, peer_pubkey_hex, plaintext, &ciphertext);

  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                  "NIP-04 encryption failed (error %d)", ret);
      return NULL;
    }

  return ciphertext;
}

/**
 * nip46_connect_plugin_nip04_decrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @ciphertext: The ciphertext to decrypt
 * @error: Return location for error
 *
 * Decrypt a message using NIP-04 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The plaintext, free with g_free()
 */
char *
nip46_connect_plugin_nip04_decrypt(Nip46ConnectPlugin *self,
                                   const char         *peer_pubkey_hex,
                                   const char         *ciphertext,
                                   GError            **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);
  g_return_val_if_fail(peer_pubkey_hex != NULL, NULL);
  g_return_val_if_fail(ciphertext != NULL, NULL);

  if (!self->connected || !self->session)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                          "Not connected to bunker");
      return NULL;
    }

  char *plaintext = NULL;
  int ret = nostr_nip46_client_nip04_decrypt(self->session, peer_pubkey_hex, ciphertext, &plaintext);

  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                  "NIP-04 decryption failed (error %d)", ret);
      return NULL;
    }

  return plaintext;
}

/**
 * nip46_connect_plugin_nip44_encrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @plaintext: The plaintext to encrypt
 * @error: Return location for error
 *
 * Encrypt a message using NIP-44 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The ciphertext, free with g_free()
 */
char *
nip46_connect_plugin_nip44_encrypt(Nip46ConnectPlugin *self,
                                   const char         *peer_pubkey_hex,
                                   const char         *plaintext,
                                   GError            **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);
  g_return_val_if_fail(peer_pubkey_hex != NULL, NULL);
  g_return_val_if_fail(plaintext != NULL, NULL);

  if (!self->connected || !self->session)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                          "Not connected to bunker");
      return NULL;
    }

  char *ciphertext = NULL;
  int ret = nostr_nip46_client_nip44_encrypt(self->session, peer_pubkey_hex, plaintext, &ciphertext);

  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                  "NIP-44 encryption failed (error %d)", ret);
      return NULL;
    }

  return ciphertext;
}

/**
 * nip46_connect_plugin_nip44_decrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @ciphertext: The ciphertext to decrypt
 * @error: Return location for error
 *
 * Decrypt a message using NIP-44 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The plaintext, free with g_free()
 */
char *
nip46_connect_plugin_nip44_decrypt(Nip46ConnectPlugin *self,
                                   const char         *peer_pubkey_hex,
                                   const char         *ciphertext,
                                   GError            **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), NULL);
  g_return_val_if_fail(peer_pubkey_hex != NULL, NULL);
  g_return_val_if_fail(ciphertext != NULL, NULL);

  if (!self->connected || !self->session)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                          "Not connected to bunker");
      return NULL;
    }

  char *plaintext = NULL;
  int ret = nostr_nip46_client_nip44_decrypt(self->session, peer_pubkey_hex, ciphertext, &plaintext);

  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
                  "NIP-44 decryption failed (error %d)", ret);
      return NULL;
    }

  return plaintext;
}

/**
 * nip46_connect_plugin_ping:
 * @self: The plugin instance
 * @error: Return location for error
 *
 * Ping the bunker to check connection status.
 *
 * Returns: %TRUE if ping succeeded
 */
gboolean
nip46_connect_plugin_ping(Nip46ConnectPlugin *self, GError **error)
{
  g_return_val_if_fail(NIP46_IS_CONNECT_PLUGIN(self), FALSE);

  if (!self->connected || !self->session)
    {
      g_set_error_literal(error, GNOSTR_PLUGIN_ERROR,
                          GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
                          "Not connected to bunker");
      return FALSE;
    }

  int ret = nostr_nip46_client_ping(self->session);

  if (ret != 0)
    {
      g_set_error(error, GNOSTR_PLUGIN_ERROR,
                  GNOSTR_PLUGIN_ERROR_NETWORK,
                  "Ping failed (error %d)", ret);
      return FALSE;
    }

  return TRUE;
}

/* ============================================================================
 * GnostrUIExtension interface implementation - Settings page
 * ============================================================================ */

typedef struct {
  Nip46ConnectPlugin *plugin;
  GtkEntry *uri_entry;
  GtkLabel *status_label;
  GtkButton *connect_button;
} SettingsPageData;

static void
settings_page_data_free(SettingsPageData *data)
{
  g_slice_free(SettingsPageData, data);
}

static void
update_settings_ui(SettingsPageData *data)
{
  if (data->plugin->connected)
    {
      gtk_label_set_text(data->status_label, "Connected");
      gtk_button_set_label(data->connect_button, "Disconnect");
      gtk_widget_add_css_class(GTK_WIDGET(data->status_label), "success");
      gtk_widget_remove_css_class(GTK_WIDGET(data->status_label), "error");
    }
  else
    {
      gtk_label_set_text(data->status_label, "Not connected");
      gtk_button_set_label(data->connect_button, "Connect");
      gtk_widget_remove_css_class(GTK_WIDGET(data->status_label), "success");
    }
}

static void
on_connect_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  SettingsPageData *data = user_data;
  Nip46ConnectPlugin *self = data->plugin;

  if (self->connected)
    {
      /* Disconnect */
      nip46_connect_plugin_disconnect(self);
      update_settings_ui(data);
    }
  else
    {
      /* Connect */
      const char *uri = gtk_editable_get_text(GTK_EDITABLE(data->uri_entry));
      if (!uri || *uri == '\0')
        {
          gtk_label_set_text(data->status_label, "Error: Enter a bunker URI");
          gtk_widget_add_css_class(GTK_WIDGET(data->status_label), "error");
          return;
        }

      GError *error = NULL;
      if (nip46_connect_plugin_connect(self, uri, &error))
        {
          update_settings_ui(data);
        }
      else
        {
          g_autofree char *msg = g_strdup_printf("Error: %s", error->message);
          gtk_label_set_text(data->status_label, msg);
          gtk_widget_add_css_class(GTK_WIDGET(data->status_label), "error");
          g_error_free(error);
        }
    }
}

static GtkWidget *
nip46_connect_plugin_create_settings_page(GnostrUIExtension   *extension,
                                          GnostrPluginContext *context G_GNUC_UNUSED)
{
  Nip46ConnectPlugin *self = NIP46_CONNECT_PLUGIN(extension);

  /* Create settings page container */
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(page, 18);
  gtk_widget_set_margin_end(page, 18);
  gtk_widget_set_margin_top(page, 18);
  gtk_widget_set_margin_bottom(page, 18);

  /* Title */
  GtkWidget *title = gtk_label_new("Nostr Connect (NIP-46)");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(page), title);

  /* Description */
  GtkWidget *desc = gtk_label_new(
    "Connect to a remote signer (bunker) to sign events without exposing your private key.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_box_append(GTK_BOX(page), desc);

  /* Bunker URI input */
  GtkWidget *uri_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(uri_box, 12);

  GtkWidget *uri_label = gtk_label_new("Bunker URI:");
  gtk_box_append(GTK_BOX(uri_box), uri_label);

  GtkWidget *uri_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(uri_entry), "bunker://... or nostrconnect://...");
  gtk_widget_set_hexpand(uri_entry, TRUE);
  if (self->bunker_uri)
    gtk_editable_set_text(GTK_EDITABLE(uri_entry), self->bunker_uri);
  gtk_box_append(GTK_BOX(uri_box), uri_entry);

  gtk_box_append(GTK_BOX(page), uri_box);

  /* Status and connect button */
  GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(status_box, 8);

  GtkWidget *status_label = gtk_label_new("Not connected");
  gtk_widget_set_hexpand(status_label, TRUE);
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(status_box), status_label);

  GtkWidget *connect_button = gtk_button_new_with_label("Connect");
  gtk_box_append(GTK_BOX(status_box), connect_button);

  gtk_box_append(GTK_BOX(page), status_box);

  /* Remote pubkey display (when connected) */
  GtkWidget *pubkey_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(pubkey_box, 8);

  GtkWidget *pubkey_label = gtk_label_new("Remote Signer:");
  gtk_box_append(GTK_BOX(pubkey_box), pubkey_label);

  GtkWidget *pubkey_value = gtk_label_new(self->remote_pubkey ? self->remote_pubkey : "—");
  gtk_label_set_selectable(GTK_LABEL(pubkey_value), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(pubkey_value), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(pubkey_value, TRUE);
  gtk_widget_add_css_class(pubkey_value, "monospace");
  gtk_box_append(GTK_BOX(pubkey_box), pubkey_value);

  gtk_box_append(GTK_BOX(page), pubkey_box);

  /* Setup data and signals */
  SettingsPageData *data = g_slice_new0(SettingsPageData);
  data->plugin = self;
  data->uri_entry = GTK_ENTRY(uri_entry);
  data->status_label = GTK_LABEL(status_label);
  data->connect_button = GTK_BUTTON(connect_button);

  g_signal_connect_data(connect_button, "clicked",
                        G_CALLBACK(on_connect_button_clicked), data,
                        (GClosureNotify)settings_page_data_free, 0);

  /* Update UI to reflect current state */
  update_settings_ui(data);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->create_settings_page = nip46_connect_plugin_create_settings_page;
  /* menu_items and note_decoration not needed for this plugin */
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
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_EVENT_HANDLER,
                                              NIP46_TYPE_CONNECT_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP46_TYPE_CONNECT_PLUGIN);
}
