/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip46-connect-plugin.h - NIP-46 Nostr Connect Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP46_CONNECT_PLUGIN_H
#define NIP46_CONNECT_PLUGIN_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define NIP46_TYPE_CONNECT_PLUGIN (nip46_connect_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip46ConnectPlugin, nip46_connect_plugin, NIP46, CONNECT_PLUGIN, GObject)

/**
 * nip46_connect_plugin_connect:
 * @self: The plugin instance
 * @bunker_uri: The bunker:// or nostrconnect:// URI
 * @error: Return location for error
 *
 * Connect to a NIP-46 bunker for remote signing.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nip46_connect_plugin_connect(Nip46ConnectPlugin *self,
                                      const char         *bunker_uri,
                                      GError            **error);

/**
 * nip46_connect_plugin_do_disconnect:
 * @self: The plugin instance
 *
 * Disconnect from the current bunker.
 */
void nip46_connect_plugin_do_disconnect(Nip46ConnectPlugin *self);

/**
 * nip46_connect_plugin_is_connected:
 * @self: The plugin instance
 *
 * Check if connected to a bunker.
 *
 * Returns: %TRUE if connected
 */
gboolean nip46_connect_plugin_is_connected(Nip46ConnectPlugin *self);

/**
 * nip46_connect_plugin_get_remote_pubkey:
 * @self: The plugin instance
 *
 * Get the remote signer's public key (the bunker's user pubkey).
 *
 * Returns: (transfer none) (nullable): The pubkey hex string, or %NULL if not connected
 */
const char *nip46_connect_plugin_get_remote_pubkey(Nip46ConnectPlugin *self);

/**
 * nip46_connect_plugin_get_public_key:
 * @self: The plugin instance
 * @error: Return location for error
 *
 * Get the user's public key from the remote signer.
 * This makes a request to the bunker.
 *
 * Returns: (transfer full) (nullable): The pubkey hex string, or %NULL on error.
 *          Free with g_free().
 */
char *nip46_connect_plugin_get_public_key(Nip46ConnectPlugin *self,
                                          GError            **error);

/**
 * nip46_connect_plugin_sign_event_async:
 * @self: The plugin instance
 * @unsigned_event_json: The unsigned event as JSON
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Request the bunker to sign an event asynchronously.
 * The callback receives the signed event JSON or an error.
 */
void nip46_connect_plugin_sign_event_async(Nip46ConnectPlugin *self,
                                           const char         *unsigned_event_json,
                                           GCancellable       *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer            user_data);

/**
 * nip46_connect_plugin_sign_event_finish:
 * @self: The plugin instance
 * @result: The #GAsyncResult
 * @error: Return location for error
 *
 * Finish an async sign operation.
 *
 * Returns: (transfer full): The signed event JSON, or %NULL on error.
 *          Free with g_free().
 */
char *nip46_connect_plugin_sign_event_finish(Nip46ConnectPlugin *self,
                                             GAsyncResult       *result,
                                             GError            **error);

/**
 * nip46_connect_plugin_nip04_encrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @plaintext: The plaintext to encrypt
 * @error: Return location for error
 *
 * Encrypt a message using NIP-04 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The ciphertext, or %NULL on error.
 *          Free with g_free().
 */
char *nip46_connect_plugin_nip04_encrypt(Nip46ConnectPlugin *self,
                                         const char         *peer_pubkey_hex,
                                         const char         *plaintext,
                                         GError            **error);

/**
 * nip46_connect_plugin_nip04_decrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @ciphertext: The ciphertext to decrypt
 * @error: Return location for error
 *
 * Decrypt a message using NIP-04 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The plaintext, or %NULL on error.
 *          Free with g_free().
 */
char *nip46_connect_plugin_nip04_decrypt(Nip46ConnectPlugin *self,
                                         const char         *peer_pubkey_hex,
                                         const char         *ciphertext,
                                         GError            **error);

/**
 * nip46_connect_plugin_nip44_encrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @plaintext: The plaintext to encrypt
 * @error: Return location for error
 *
 * Encrypt a message using NIP-44 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The ciphertext, or %NULL on error.
 *          Free with g_free().
 */
char *nip46_connect_plugin_nip44_encrypt(Nip46ConnectPlugin *self,
                                         const char         *peer_pubkey_hex,
                                         const char         *plaintext,
                                         GError            **error);

/**
 * nip46_connect_plugin_nip44_decrypt:
 * @self: The plugin instance
 * @peer_pubkey_hex: The peer's public key (hex)
 * @ciphertext: The ciphertext to decrypt
 * @error: Return location for error
 *
 * Decrypt a message using NIP-44 via the remote signer.
 *
 * Returns: (transfer full) (nullable): The plaintext, or %NULL on error.
 *          Free with g_free().
 */
char *nip46_connect_plugin_nip44_decrypt(Nip46ConnectPlugin *self,
                                         const char         *peer_pubkey_hex,
                                         const char         *ciphertext,
                                         GError            **error);

/**
 * nip46_connect_plugin_ping:
 * @self: The plugin instance
 * @error: Return location for error
 *
 * Ping the bunker to check connection status.
 *
 * Returns: %TRUE if ping succeeded, %FALSE on error
 */
gboolean nip46_connect_plugin_ping(Nip46ConnectPlugin *self,
                                   GError            **error);

G_END_DECLS

#endif /* NIP46_CONNECT_PLUGIN_H */
