#pragma once

#include <glib.h>
#include "nwc_wallet.h"

G_BEGIN_DECLS

/*
 * NIP-47 (Nostr Wallet Connect) GLib Wallet API
 *
 * This API provides GLib-friendly wrappers for NIP-47 wallet operations.
 * All functions use GLib types and error handling conventions.
 */

/* Session Management */

/**
 * nostr_nwc_wallet_session_init_g:
 * @client_pub_hex: The client's public key in hex format (64 chars)
 * @wallet_supported: Array of encryption methods supported by the wallet
 * @wallet_n: Number of elements in @wallet_supported
 * @client_supported: Array of encryption methods supported by the client
 * @client_n: Number of elements in @client_supported
 * @error: Return location for error or %NULL
 *
 * Creates a new NWC wallet session with encryption negotiation.
 *
 * Returns: (transfer full): A new session handle, or %NULL on error
 */
gpointer nostr_nwc_wallet_session_init_g(const gchar *client_pub_hex,
                                         const gchar **wallet_supported, gsize wallet_n,
                                         const gchar **client_supported, gsize client_n,
                                         GError **error);

/**
 * nostr_nwc_wallet_session_free_g:
 * @session: The session to free
 *
 * Frees all resources associated with the session.
 */
void nostr_nwc_wallet_session_free_g(gpointer session);

/* Response Building */

/**
 * nostr_nwc_wallet_build_response_g:
 * @session: The wallet session
 * @req_event_id: The ID of the request event being responded to
 * @result_type: The method name (e.g., "pay_invoice", "get_balance")
 * @result_json: JSON string of the result, or %NULL for empty result
 * @out_event_json: (out): Location to store the resulting event JSON
 * @error: Return location for error or %NULL
 *
 * Builds a successful NWC response event JSON.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_wallet_build_response_g(gpointer session,
                                           const gchar *req_event_id,
                                           const gchar *result_type,
                                           const gchar *result_json,
                                           gchar **out_event_json,
                                           GError **error);

/**
 * nostr_nwc_wallet_build_error_response_g:
 * @session: The wallet session
 * @req_event_id: The ID of the request event being responded to
 * @error_code: NWC error code (e.g., "RATE_LIMITED", "NOT_IMPLEMENTED")
 * @error_message: Human-readable error message
 * @out_event_json: (out): Location to store the resulting event JSON
 * @error: Return location for error or %NULL
 *
 * Builds an error NWC response event JSON.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_wallet_build_error_response_g(gpointer session,
                                                  const gchar *req_event_id,
                                                  const gchar *error_code,
                                                  const gchar *error_message,
                                                  gchar **out_event_json,
                                                  GError **error);

/* Encryption/Decryption */

/**
 * nostr_nwc_wallet_encrypt_g:
 * @session: The wallet session (determines encryption scheme)
 * @wallet_sk_hex: Wallet's secret key in hex (64 chars)
 * @client_pub_hex: Client's public key in hex (64 chars)
 * @plaintext: The text to encrypt
 * @out_ciphertext: (out): Location to store the encrypted result
 * @error: Return location for error or %NULL
 *
 * Encrypts plaintext using the session's negotiated encryption scheme.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_wallet_encrypt_g(gpointer session,
                                    const gchar *wallet_sk_hex,
                                    const gchar *client_pub_hex,
                                    const gchar *plaintext,
                                    gchar **out_ciphertext,
                                    GError **error);

/**
 * nostr_nwc_wallet_decrypt_g:
 * @session: The wallet session (determines encryption scheme)
 * @wallet_sk_hex: Wallet's secret key in hex (64 chars)
 * @client_pub_hex: Client's public key in hex (64 chars)
 * @ciphertext: The text to decrypt
 * @out_plaintext: (out): Location to store the decrypted result
 * @error: Return location for error or %NULL
 *
 * Decrypts ciphertext using the session's negotiated encryption scheme.
 *
 * Returns: %TRUE on success, %FALSE on error
 */
gboolean nostr_nwc_wallet_decrypt_g(gpointer session,
                                    const gchar *wallet_sk_hex,
                                    const gchar *client_pub_hex,
                                    const gchar *ciphertext,
                                    gchar **out_plaintext,
                                    GError **error);

/* Session Properties */

/**
 * nostr_nwc_wallet_get_encryption_g:
 * @session: The wallet session
 *
 * Gets the negotiated encryption scheme as a string.
 *
 * Returns: "nip44-v2" or "nip04", or %NULL if session is invalid
 */
const gchar *nostr_nwc_wallet_get_encryption_g(gpointer session);

/**
 * nostr_nwc_wallet_get_client_pub_g:
 * @session: The wallet session
 *
 * Gets the client's public key from the session.
 *
 * Returns: The client public key in hex, or %NULL if session is invalid
 */
const gchar *nostr_nwc_wallet_get_client_pub_g(gpointer session);

G_END_DECLS
