/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip47-nwc-plugin.h - NIP-47 Nostr Wallet Connect Plugin
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef NIP47_NWC_PLUGIN_H
#define NIP47_NWC_PLUGIN_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define NIP47_TYPE_NWC_PLUGIN (nip47_nwc_plugin_get_type())
G_DECLARE_FINAL_TYPE(Nip47NwcPlugin, nip47_nwc_plugin, NIP47, NWC_PLUGIN, GObject)

/* NIP-47 Event Kinds */
#define NWC_KIND_INFO     13194
#define NWC_KIND_REQUEST  23194
#define NWC_KIND_RESPONSE 23195

/**
 * Nip47NwcState:
 * @NIP47_NWC_STATE_DISCONNECTED: No wallet connection configured
 * @NIP47_NWC_STATE_CONNECTING: Connection in progress
 * @NIP47_NWC_STATE_CONNECTED: Wallet connected and ready
 * @NIP47_NWC_STATE_ERROR: Connection error occurred
 *
 * Connection state for the NWC wallet.
 */
typedef enum {
  NIP47_NWC_STATE_DISCONNECTED,
  NIP47_NWC_STATE_CONNECTING,
  NIP47_NWC_STATE_CONNECTED,
  NIP47_NWC_STATE_ERROR
} Nip47NwcState;

/**
 * Nip47NwcError:
 * @NIP47_NWC_ERROR_INVALID_URI: Invalid nostr+walletconnect:// URI
 * @NIP47_NWC_ERROR_CONNECTION_FAILED: Failed to connect to wallet
 * @NIP47_NWC_ERROR_REQUEST_FAILED: NWC request failed
 * @NIP47_NWC_ERROR_TIMEOUT: Request timed out
 * @NIP47_NWC_ERROR_WALLET_ERROR: Wallet returned an error
 *
 * Error codes for NWC operations.
 */
typedef enum {
  NIP47_NWC_ERROR_INVALID_URI,
  NIP47_NWC_ERROR_CONNECTION_FAILED,
  NIP47_NWC_ERROR_REQUEST_FAILED,
  NIP47_NWC_ERROR_TIMEOUT,
  NIP47_NWC_ERROR_WALLET_ERROR
} Nip47NwcError;

#define NIP47_NWC_ERROR (nip47_nwc_error_quark())
GQuark nip47_nwc_error_quark(void);

/**
 * nip47_nwc_plugin_get_default:
 *
 * Get the active NWC plugin instance (if loaded).
 *
 * Returns: (transfer none) (nullable): The plugin instance or %NULL
 */
Nip47NwcPlugin *nip47_nwc_plugin_get_default(void);

/**
 * nip47_nwc_plugin_connect:
 * @self: The NWC plugin
 * @connection_uri: nostr+walletconnect:// URI string
 * @error: (out) (optional): Return location for error
 *
 * Parse and store a NWC connection URI.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nip47_nwc_plugin_connect(Nip47NwcPlugin *self,
                                  const gchar *connection_uri,
                                  GError **error);

/**
 * nip47_nwc_plugin_disconnect:
 * @self: The NWC plugin
 *
 * Disconnect from the wallet and clear stored connection.
 */
void nip47_nwc_plugin_disconnect(Nip47NwcPlugin *self);

/**
 * nip47_nwc_plugin_get_state:
 * @self: The NWC plugin
 *
 * Get the current connection state.
 *
 * Returns: The connection state
 */
Nip47NwcState nip47_nwc_plugin_get_state(Nip47NwcPlugin *self);

/**
 * nip47_nwc_plugin_is_connected:
 * @self: The NWC plugin
 *
 * Check if a wallet connection is configured.
 *
 * Returns: %TRUE if connected
 */
gboolean nip47_nwc_plugin_is_connected(Nip47NwcPlugin *self);

/**
 * nip47_nwc_plugin_get_wallet_pubkey:
 * @self: The NWC plugin
 *
 * Get the connected wallet's public key.
 *
 * Returns: (nullable) (transfer none): Wallet pubkey hex or %NULL
 */
const gchar *nip47_nwc_plugin_get_wallet_pubkey(Nip47NwcPlugin *self);

/**
 * nip47_nwc_plugin_get_relay:
 * @self: The NWC plugin
 *
 * Get the primary relay URL for the wallet connection.
 *
 * Returns: (nullable) (transfer none): Relay URL or %NULL
 */
const gchar *nip47_nwc_plugin_get_relay(Nip47NwcPlugin *self);

/**
 * nip47_nwc_plugin_get_lud16:
 * @self: The NWC plugin
 *
 * Get the lightning address from the connection URI if present.
 *
 * Returns: (nullable) (transfer none): lud16 address or %NULL
 */
const gchar *nip47_nwc_plugin_get_lud16(Nip47NwcPlugin *self);

/**
 * nip47_nwc_plugin_get_balance_async:
 * @self: The NWC plugin
 * @cancellable: (nullable): Optional cancellable
 * @callback: Callback when balance is retrieved
 * @user_data: User data for callback
 *
 * Asynchronously get the wallet balance.
 */
void nip47_nwc_plugin_get_balance_async(Nip47NwcPlugin *self,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

/**
 * nip47_nwc_plugin_get_balance_finish:
 * @self: The NWC plugin
 * @result: Async result
 * @balance_msat: (out): Balance in millisatoshis
 * @error: (out) (optional): Return location for error
 *
 * Finish an async balance request.
 *
 * Returns: %TRUE on success
 */
gboolean nip47_nwc_plugin_get_balance_finish(Nip47NwcPlugin *self,
                                             GAsyncResult *result,
                                             gint64 *balance_msat,
                                             GError **error);

/**
 * nip47_nwc_plugin_pay_invoice_async:
 * @self: The NWC plugin
 * @bolt11: BOLT-11 invoice string
 * @amount_msat: Amount override in millisatoshis, or 0 to use invoice amount
 * @cancellable: (nullable): Optional cancellable
 * @callback: Callback when payment completes
 * @user_data: User data for callback
 *
 * Asynchronously pay a lightning invoice.
 */
void nip47_nwc_plugin_pay_invoice_async(Nip47NwcPlugin *self,
                                        const gchar *bolt11,
                                        gint64 amount_msat,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

/**
 * nip47_nwc_plugin_pay_invoice_finish:
 * @self: The NWC plugin
 * @result: Async result
 * @preimage: (out) (transfer full) (optional): Payment preimage hex
 * @error: (out) (optional): Return location for error
 *
 * Finish an async payment request.
 *
 * Returns: %TRUE on success
 */
gboolean nip47_nwc_plugin_pay_invoice_finish(Nip47NwcPlugin *self,
                                             GAsyncResult *result,
                                             gchar **preimage,
                                             GError **error);

/**
 * nip47_nwc_plugin_make_invoice_async:
 * @self: The NWC plugin
 * @amount_msat: Amount in millisatoshis
 * @description: (nullable): Invoice description
 * @expiry_secs: Expiry time in seconds, or 0 for default
 * @cancellable: (nullable): Optional cancellable
 * @callback: Callback when invoice is created
 * @user_data: User data for callback
 *
 * Asynchronously create a lightning invoice.
 */
void nip47_nwc_plugin_make_invoice_async(Nip47NwcPlugin *self,
                                         gint64 amount_msat,
                                         const gchar *description,
                                         gint64 expiry_secs,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data);

/**
 * nip47_nwc_plugin_make_invoice_finish:
 * @self: The NWC plugin
 * @result: Async result
 * @bolt11: (out) (transfer full): BOLT-11 invoice string
 * @payment_hash: (out) (transfer full) (optional): Payment hash hex
 * @error: (out) (optional): Return location for error
 *
 * Finish an async make_invoice request.
 *
 * Returns: %TRUE on success
 */
gboolean nip47_nwc_plugin_make_invoice_finish(Nip47NwcPlugin *self,
                                              GAsyncResult *result,
                                              gchar **bolt11,
                                              gchar **payment_hash,
                                              GError **error);

/**
 * nip47_nwc_format_balance:
 * @balance_msat: Balance in millisatoshis
 *
 * Format a balance for display (e.g., "1,234 sats").
 *
 * Returns: (transfer full): Formatted string
 */
gchar *nip47_nwc_format_balance(gint64 balance_msat);

G_END_DECLS

#endif /* NIP47_NWC_PLUGIN_H */
