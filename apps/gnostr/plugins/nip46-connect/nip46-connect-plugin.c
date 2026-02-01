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

G_DEFINE_TYPE_WITH_CODE(Nip46ConnectPlugin, nip46_connect_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init))

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
}
