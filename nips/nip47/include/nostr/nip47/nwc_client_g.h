#ifndef NIPS_NIP47_NOSTR_NIP47_NWC_CLIENT_G_H
#define NIPS_NIP47_NOSTR_NIP47_NWC_CLIENT_G_H

#include <glib.h>
#include "nwc_client.h"

G_BEGIN_DECLS

/*
 * NIP-47 (Nostr Wallet Connect) GLib Client API
 *
 * This API provides GLib-friendly wrappers for NIP-47 client operations.
 * All functions use GLib types and error handling conventions.
 */

/* Session Management */

/**
 * nostr_nwc_client_session_init_g:
 * @wallet_pub_hex: The wallet's public key in hex format (64 chars)
 * @client_supported: Array of encryption methods supported by the client
 * @client_n: Number of elements in @client_supported
 * @wallet_supported: Array of encryption methods supported by the wallet
 * @wallet_n: Number of elements in @wallet_supported
 * @error: Return location for error or %NULL
 *
 * Creates a new NWC client session with encryption negotiation.
 *
 * Returns: (transfer full): A new session handle, or %NULL on error
 */
gpointer nostr_nwc_client_session_init_g(const gchar *wallet_pub_hex,
                                         const gchar **client_supported, gsize client_n,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         GError **error);

/**
 * nostr_nwc_client_session_free_g:
 * @session: The session to free
 *
 * Frees all resources associated with the session.
 */
void nostr_nwc_client_session_free_g(gpointer session);

/* Request Building */

/**
 * nostr_nwc_client_build_request_g:
 * @session: The client session
 * @method: The NWC method name (e.g., "pay_invoice", "get_balance")
 * @params_json: JSON string of parameters, or %NULL for empty params
 * @out_event_json: (out): Location to store the resulting event JSON
 * @error: Return location for error or %NULL
 *
 * Builds a NWC request event JSON for the specified method.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_client_build_request_g(gpointer session,
                                          const gchar *method,
                                          const gchar *params_json,
                                          gchar **out_event_json,
                                          GError **error);

/* Encryption/Decryption */

/**
 * nostr_nwc_client_encrypt_g:
 * @session: The client session (determines encryption scheme)
 * @client_sk_hex: Client's secret key in hex (64 chars)
 * @wallet_pub_hex: Wallet's public key in hex (64 chars)
 * @plaintext: The text to encrypt
 * @out_ciphertext: (out): Location to store the encrypted result
 * @error: Return location for error or %NULL
 *
 * Encrypts plaintext using the session's negotiated encryption scheme.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_client_encrypt_g(gpointer session,
                                    const gchar *client_sk_hex,
                                    const gchar *wallet_pub_hex,
                                    const gchar *plaintext,
                                    gchar **out_ciphertext,
                                    GError **error);

/**
 * nostr_nwc_client_decrypt_g:
 * @session: The client session (determines encryption scheme)
 * @client_sk_hex: Client's secret key in hex (64 chars)
 * @wallet_pub_hex: Wallet's public key in hex (64 chars)
 * @ciphertext: The text to decrypt
 * @out_plaintext: (out): Location to store the decrypted result
 * @error: Return location for error or %NULL
 *
 * Decrypts ciphertext using the session's negotiated encryption scheme.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_client_decrypt_g(gpointer session,
                                    const gchar *client_sk_hex,
                                    const gchar *wallet_pub_hex,
                                    const gchar *ciphertext,
                                    gchar **out_plaintext,
                                    GError **error);

/**
 * nostr_nwc_client_get_encryption_g:
 * @session: The client session
 *
 * Gets the negotiated encryption scheme as a string.
 *
 * Returns: "nip44-v2" or "nip04", or %NULL if session is invalid
 */
const gchar *nostr_nwc_client_get_encryption_g(gpointer session);

/* URI Handling */

/**
 * nostr_nwc_uri_parse_g:
 * @uri: The nostr+walletconnect:// URI to parse
 * @out_wallet_pubkey_hex: (out) (optional): Location for wallet pubkey
 * @out_relays: (out) (optional) (array zero-terminated=1): Location for relay list
 * @out_secret_hex: (out) (optional): Location for client secret
 * @out_lud16: (out) (optional): Location for LUD-16 address
 * @error: Return location for error or %NULL
 *
 * Parses a NWC connection URI into its components.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_uri_parse_g(const gchar *uri,
                               gchar **out_wallet_pubkey_hex,
                               gchar ***out_relays,
                               gchar **out_secret_hex,
                               gchar **out_lud16,
                               GError **error);

/**
 * nostr_nwc_uri_build_g:
 * @wallet_pubkey_hex: Wallet public key in hex (64 chars)
 * @relays: (array zero-terminated=1) (nullable): List of relay URLs
 * @secret_hex: Client secret in hex (64 chars)
 * @lud16: (nullable): Optional LUD-16 address
 * @out_uri: (out): Location to store the resulting URI
 * @error: Return location for error or %NULL
 *
 * Builds a NWC connection URI from components.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_uri_build_g(const gchar *wallet_pubkey_hex,
                               const gchar *const *relays,
                               const gchar *secret_hex,
                               const gchar *lud16,
                               gchar **out_uri,
                               GError **error);

/* Info Event Handling */

/**
 * nostr_nwc_info_build_g:
 * @pubkey: (nullable): Wallet public key in hex, or %NULL
 * @created_at: Timestamp, or 0 for current time
 * @methods: (array length=methods_count): Supported NWC methods
 * @methods_count: Number of methods
 * @encryptions: (array length=enc_count): Supported encryption schemes
 * @enc_count: Number of encryption schemes
 * @notifications: Whether notifications are supported
 * @out_event_json: (out): Location for the Info event JSON
 * @error: Return location for error or %NULL
 *
 * Builds a NIP-47 Info event (kind 13194).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_info_build_g(const gchar *pubkey,
                                gint64 created_at,
                                const gchar *const *methods,
                                gsize methods_count,
                                const gchar *const *encryptions,
                                gsize enc_count,
                                gboolean notifications,
                                gchar **out_event_json,
                                GError **error);

/**
 * nostr_nwc_info_parse_g:
 * @event_json: The Info event JSON to parse
 * @out_methods: (out) (optional) (array zero-terminated=1): Supported methods
 * @out_methods_count: (out) (optional): Number of methods
 * @out_encryptions: (out) (optional) (array zero-terminated=1): Encryptions
 * @out_enc_count: (out) (optional): Number of encryptions
 * @out_notifications: (out) (optional): Notifications support flag
 * @error: Return location for error or %NULL
 *
 * Parses a NIP-47 Info event (kind 13194).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_info_parse_g(const gchar *event_json,
                                gchar ***out_methods,
                                gsize *out_methods_count,
                                gchar ***out_encryptions,
                                gsize *out_enc_count,
                                gboolean *out_notifications,
                                GError **error);

/* Request/Response Parsing */

/**
 * nostr_nwc_request_parse_g:
 * @event_json: The request event JSON to parse
 * @out_wallet_pub_hex: (out) (optional): Target wallet pubkey
 * @out_encryption: (out) (optional): Encryption scheme used
 * @out_method: (out) (optional): Request method name
 * @out_params_json: (out) (optional): Parameters as JSON string
 * @error: Return location for error or %NULL
 *
 * Parses a NWC request event (kind 23194).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_request_parse_g(const gchar *event_json,
                                   gchar **out_wallet_pub_hex,
                                   gchar **out_encryption,
                                   gchar **out_method,
                                   gchar **out_params_json,
                                   GError **error);

/**
 * nostr_nwc_response_parse_g:
 * @event_json: The response event JSON to parse
 * @out_client_pub_hex: (out) (optional): Target client pubkey
 * @out_req_event_id: (out) (optional): Referenced request event ID
 * @out_encryption: (out) (optional): Encryption scheme used
 * @out_result_type: (out) (optional): Result type (method name)
 * @out_result_json: (out) (optional): Result as JSON string
 * @out_error_code: (out) (optional): Error code if error response
 * @out_error_message: (out) (optional): Error message if error response
 * @error: Return location for error or %NULL
 *
 * Parses a NWC response event (kind 23195).
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_response_parse_g(const gchar *event_json,
                                    gchar **out_client_pub_hex,
                                    gchar **out_req_event_id,
                                    gchar **out_encryption,
                                    gchar **out_result_type,
                                    gchar **out_result_json,
                                    gchar **out_error_code,
                                    gchar **out_error_message,
                                    GError **error);

/* Utilities */

/**
 * nostr_nwc_select_encryption_g:
 * @client_supported: (array length=client_n): Client's supported encryptions
 * @client_n: Number of client encryptions
 * @wallet_supported: (array length=wallet_n): Wallet's supported encryptions
 * @wallet_n: Number of wallet encryptions
 * @out_encryption: (out): Selected encryption scheme
 * @error: Return location for error or %NULL
 *
 * Selects the best common encryption scheme (prefers nip44-v2 over nip04).
 *
 * Returns: %TRUE on success, %FALSE if no common encryption found
 */
gboolean nostr_nwc_select_encryption_g(const gchar *const *client_supported, gsize client_n,
                                       const gchar *const *wallet_supported, gsize wallet_n,
                                       gchar **out_encryption,
                                       GError **error);

G_END_DECLS
#endif /* NIPS_NIP47_NOSTR_NIP47_NWC_CLIENT_G_H */
