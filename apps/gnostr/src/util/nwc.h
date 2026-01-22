/**
 * gnostr NWC (Nostr Wallet Connect) Service
 *
 * NIP-47 implementation for the gnostr GTK app.
 * Provides wallet connection management, balance queries, and payment operations.
 */

#ifndef GNOSTR_NWC_H
#define GNOSTR_NWC_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_NWC_SERVICE (gnostr_nwc_service_get_type())
G_DECLARE_FINAL_TYPE(GnostrNwcService, gnostr_nwc_service, GNOSTR, NWC_SERVICE, GObject)

/**
 * NWC connection state
 */
typedef enum {
  GNOSTR_NWC_STATE_DISCONNECTED,
  GNOSTR_NWC_STATE_CONNECTING,
  GNOSTR_NWC_STATE_CONNECTED,
  GNOSTR_NWC_STATE_ERROR
} GnostrNwcState;

/**
 * NWC error codes
 */
typedef enum {
  GNOSTR_NWC_ERROR_INVALID_URI,
  GNOSTR_NWC_ERROR_CONNECTION_FAILED,
  GNOSTR_NWC_ERROR_REQUEST_FAILED,
  GNOSTR_NWC_ERROR_TIMEOUT,
  GNOSTR_NWC_ERROR_WALLET_ERROR
} GnostrNwcError;

#define GNOSTR_NWC_ERROR (gnostr_nwc_error_quark())
GQuark gnostr_nwc_error_quark(void);

/**
 * gnostr_nwc_service_get_default:
 *
 * Get the singleton NWC service instance.
 *
 * Returns: (transfer none): the shared NWC service
 */
GnostrNwcService *gnostr_nwc_service_get_default(void);

/**
 * gnostr_nwc_service_connect:
 * @self: the NWC service
 * @connection_uri: nostr+walletconnect:// URI string
 * @error: (out) (optional): return location for error
 *
 * Parse and store a NWC connection URI. Does not establish relay connection.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean gnostr_nwc_service_connect(GnostrNwcService *self,
                                    const gchar *connection_uri,
                                    GError **error);

/**
 * gnostr_nwc_service_disconnect:
 * @self: the NWC service
 *
 * Disconnect from the wallet and clear stored connection.
 */
void gnostr_nwc_service_disconnect(GnostrNwcService *self);

/**
 * gnostr_nwc_service_get_state:
 * @self: the NWC service
 *
 * Get the current connection state.
 *
 * Returns: the connection state
 */
GnostrNwcState gnostr_nwc_service_get_state(GnostrNwcService *self);

/**
 * gnostr_nwc_service_is_connected:
 * @self: the NWC service
 *
 * Check if a wallet connection is configured.
 *
 * Returns: %TRUE if connected
 */
gboolean gnostr_nwc_service_is_connected(GnostrNwcService *self);

/**
 * gnostr_nwc_service_get_wallet_pubkey:
 * @self: the NWC service
 *
 * Get the connected wallet's public key.
 *
 * Returns: (nullable) (transfer none): wallet pubkey hex or NULL if not connected
 */
const gchar *gnostr_nwc_service_get_wallet_pubkey(GnostrNwcService *self);

/**
 * gnostr_nwc_service_get_relay:
 * @self: the NWC service
 *
 * Get the first relay URL for the wallet connection.
 *
 * Returns: (nullable) (transfer none): relay URL or NULL if not connected
 */
const gchar *gnostr_nwc_service_get_relay(GnostrNwcService *self);

/**
 * gnostr_nwc_service_get_lud16:
 * @self: the NWC service
 *
 * Get the lightning address from the connection URI if present.
 *
 * Returns: (nullable) (transfer none): lud16 address or NULL
 */
const gchar *gnostr_nwc_service_get_lud16(GnostrNwcService *self);

/**
 * gnostr_nwc_service_get_balance_async:
 * @self: the NWC service
 * @cancellable: (nullable): optional cancellable
 * @callback: callback when balance is retrieved
 * @user_data: user data for callback
 *
 * Asynchronously get the wallet balance.
 */
void gnostr_nwc_service_get_balance_async(GnostrNwcService *self,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

/**
 * gnostr_nwc_service_get_balance_finish:
 * @self: the NWC service
 * @result: async result
 * @balance_msat: (out): balance in millisatoshis
 * @error: (out) (optional): return location for error
 *
 * Finish an async balance request.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nwc_service_get_balance_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gint64 *balance_msat,
                                               GError **error);

/**
 * gnostr_nwc_service_pay_invoice_async:
 * @self: the NWC service
 * @bolt11: BOLT-11 invoice string
 * @amount_msat: (optional): amount override in millisatoshis, or 0 to use invoice amount
 * @cancellable: (nullable): optional cancellable
 * @callback: callback when payment completes
 * @user_data: user data for callback
 *
 * Asynchronously pay a lightning invoice.
 */
void gnostr_nwc_service_pay_invoice_async(GnostrNwcService *self,
                                          const gchar *bolt11,
                                          gint64 amount_msat,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data);

/**
 * gnostr_nwc_service_pay_invoice_finish:
 * @self: the NWC service
 * @result: async result
 * @preimage: (out) (transfer full) (optional): payment preimage hex
 * @error: (out) (optional): return location for error
 *
 * Finish an async payment request.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nwc_service_pay_invoice_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gchar **preimage,
                                               GError **error);

/**
 * gnostr_nwc_service_make_invoice_async:
 * @self: the NWC service
 * @amount_msat: amount in millisatoshis
 * @description: (nullable): invoice description
 * @expiry_secs: expiry time in seconds, or 0 for default
 * @cancellable: (nullable): optional cancellable
 * @callback: callback when invoice is created
 * @user_data: user data for callback
 *
 * Asynchronously create a lightning invoice.
 */
void gnostr_nwc_service_make_invoice_async(GnostrNwcService *self,
                                           gint64 amount_msat,
                                           const gchar *description,
                                           gint64 expiry_secs,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data);

/**
 * gnostr_nwc_service_make_invoice_finish:
 * @self: the NWC service
 * @result: async result
 * @bolt11: (out) (transfer full): BOLT-11 invoice string
 * @payment_hash: (out) (transfer full) (optional): payment hash hex
 * @error: (out) (optional): return location for error
 *
 * Finish an async make_invoice request.
 *
 * Returns: %TRUE on success
 */
gboolean gnostr_nwc_service_make_invoice_finish(GnostrNwcService *self,
                                                GAsyncResult *result,
                                                gchar **bolt11,
                                                gchar **payment_hash,
                                                GError **error);

/**
 * gnostr_nwc_service_save_to_settings:
 * @self: the NWC service
 *
 * Save the current connection to GSettings for persistence.
 */
void gnostr_nwc_service_save_to_settings(GnostrNwcService *self);

/**
 * gnostr_nwc_service_load_from_settings:
 * @self: the NWC service
 *
 * Load a saved connection from GSettings.
 *
 * Returns: %TRUE if a connection was loaded
 */
gboolean gnostr_nwc_service_load_from_settings(GnostrNwcService *self);

/**
 * gnostr_nwc_format_balance:
 * @balance_msat: balance in millisatoshis
 *
 * Format a balance for display (e.g., "1,234 sats").
 *
 * Returns: (transfer full): formatted string
 */
gchar *gnostr_nwc_format_balance(gint64 balance_msat);

G_END_DECLS

#endif /* GNOSTR_NWC_H */
