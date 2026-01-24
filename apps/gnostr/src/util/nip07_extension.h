/**
 * NIP-07 Browser Extension Interface for Desktop Applications
 *
 * NIP-07 defines the window.nostr API that browser extensions expose for web apps.
 * Since gnostr is a GTK desktop app, this module provides:
 *   1. A D-Bus interface matching NIP-07 semantics
 *   2. Utility functions for the protocol
 *
 * The window.nostr API includes:
 *   - getPublicKey(): Promise<string>  // returns hex pubkey
 *   - signEvent(event): Promise<SignedEvent>  // sign an unsigned event
 *   - getRelays(): Promise<RelayMap>  // get user's relays
 *   - nip04.encrypt(pubkey, plaintext): Promise<string>  // NIP-04 encrypt
 *   - nip04.decrypt(pubkey, ciphertext): Promise<string>  // NIP-04 decrypt
 *   - nip44.encrypt(pubkey, plaintext): Promise<string>  // NIP-44 encrypt
 *   - nip44.decrypt(pubkey, ciphertext): Promise<string>  // NIP-44 decrypt
 *
 * D-Bus interface: org.nostr.Nip07
 * D-Bus path: /org/nostr/nip07
 *
 * This allows other desktop applications to request Nostr signing operations
 * through the standard D-Bus session bus, similar to how web apps use window.nostr.
 */

#ifndef GNOSTR_NIP07_EXTENSION_H
#define GNOSTR_NIP07_EXTENSION_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * D-Bus interface constants
 */
#define GNOSTR_NIP07_DBUS_NAME       "org.nostr.Nip07"
#define GNOSTR_NIP07_DBUS_PATH       "/org/nostr/nip07"
#define GNOSTR_NIP07_DBUS_INTERFACE  "org.nostr.Nip07"

/**
 * GnostrNip07Request:
 *
 * Types of requests that can be made via the NIP-07 interface.
 * These correspond to the window.nostr API methods.
 */
typedef enum {
  GNOSTR_NIP07_GET_PUBLIC_KEY = 0,  /* getPublicKey() */
  GNOSTR_NIP07_SIGN_EVENT,          /* signEvent(event) */
  GNOSTR_NIP07_GET_RELAYS,          /* getRelays() */
  GNOSTR_NIP07_NIP04_ENCRYPT,       /* nip04.encrypt(pubkey, plaintext) */
  GNOSTR_NIP07_NIP04_DECRYPT,       /* nip04.decrypt(pubkey, ciphertext) */
  GNOSTR_NIP07_NIP44_ENCRYPT,       /* nip44.encrypt(pubkey, plaintext) */
  GNOSTR_NIP07_NIP44_DECRYPT        /* nip44.decrypt(pubkey, ciphertext) */
} GnostrNip07Request;

/**
 * GnostrNip07Response:
 * @success: Whether the operation succeeded
 * @result_str: The result string (pubkey, signature, ciphertext, etc.) - owned
 * @error_msg: Error message if failed - owned
 *
 * Response structure for NIP-07 operations.
 */
typedef struct {
  gboolean success;
  char *result_str;     /* Result data (context-dependent) - owned */
  char *error_msg;      /* Error message if !success - owned */
} GnostrNip07Response;

/**
 * GnostrNip07Relay:
 * @url: The relay URL
 * @read: Whether the relay is used for reading
 * @write: Whether the relay is used for writing
 *
 * Relay information for getRelays() response.
 */
typedef struct {
  char *url;
  gboolean read;
  gboolean write;
} GnostrNip07Relay;

/**
 * gnostr_nip07_response_free:
 * @response: The response to free
 *
 * Free a NIP-07 response structure.
 */
void gnostr_nip07_response_free(GnostrNip07Response *response);

/**
 * gnostr_nip07_relay_free:
 * @relay: The relay to free
 *
 * Free a NIP-07 relay structure.
 */
void gnostr_nip07_relay_free(GnostrNip07Relay *relay);

/**
 * gnostr_nip07_request_to_string:
 * @request: The request type
 *
 * Get a string representation of a request type for debugging.
 *
 * Returns: A static string (do not free)
 */
const char *gnostr_nip07_request_to_string(GnostrNip07Request request);

/* ---- D-Bus Client Functions ---- */
/* These functions call a NIP-07 D-Bus service (like gnostr-signer or similar) */

/**
 * gnostr_nip07_get_public_key:
 * @error: (out) (optional): Return location for error
 *
 * Request the user's public key via D-Bus.
 * Equivalent to window.nostr.getPublicKey().
 *
 * Returns: (transfer full): A new response structure containing the hex pubkey,
 *          or NULL on D-Bus communication error. Caller frees with
 *          gnostr_nip07_response_free().
 */
GnostrNip07Response *gnostr_nip07_get_public_key(GError **error);

/**
 * gnostr_nip07_get_public_key_async:
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_nip07_get_public_key().
 */
void gnostr_nip07_get_public_key_async(GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data);

/**
 * gnostr_nip07_get_public_key_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async getPublicKey operation.
 *
 * Returns: (transfer full): The response, or NULL on error.
 */
GnostrNip07Response *gnostr_nip07_get_public_key_finish(GAsyncResult *result,
                                                         GError **error);

/**
 * gnostr_nip07_sign_event:
 * @unsigned_event_json: The unsigned event as JSON string
 * @error: (out) (optional): Return location for error
 *
 * Request signing of an event via D-Bus.
 * Equivalent to window.nostr.signEvent(event).
 *
 * The unsigned event JSON should contain:
 *   - kind: event kind (integer)
 *   - content: event content (string)
 *   - tags: event tags (array of arrays)
 *   - created_at: timestamp (integer, optional - will be set if missing)
 *
 * Returns: (transfer full): A new response structure containing the signed event JSON,
 *          or NULL on D-Bus communication error. Caller frees with
 *          gnostr_nip07_response_free().
 */
GnostrNip07Response *gnostr_nip07_sign_event(const char *unsigned_event_json,
                                              GError **error);

/**
 * gnostr_nip07_sign_event_async:
 * @unsigned_event_json: The unsigned event as JSON string
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_nip07_sign_event().
 */
void gnostr_nip07_sign_event_async(const char *unsigned_event_json,
                                    GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

/**
 * gnostr_nip07_sign_event_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async signEvent operation.
 *
 * Returns: (transfer full): The response, or NULL on error.
 */
GnostrNip07Response *gnostr_nip07_sign_event_finish(GAsyncResult *result,
                                                     GError **error);

/**
 * gnostr_nip07_get_relays:
 * @error: (out) (optional): Return location for error
 *
 * Request the user's relay list via D-Bus.
 * Equivalent to window.nostr.getRelays().
 *
 * Returns: (transfer full): A new response structure containing relay map JSON,
 *          or NULL on D-Bus communication error. The result_str contains JSON
 *          in the format: {"wss://relay.example": {"read": true, "write": true}, ...}
 *          Caller frees with gnostr_nip07_response_free().
 */
GnostrNip07Response *gnostr_nip07_get_relays(GError **error);

/**
 * gnostr_nip07_get_relays_async:
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_nip07_get_relays().
 */
void gnostr_nip07_get_relays_async(GCancellable *cancellable,
                                    GAsyncReadyCallback callback,
                                    gpointer user_data);

/**
 * gnostr_nip07_get_relays_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async getRelays operation.
 *
 * Returns: (transfer full): The response, or NULL on error.
 */
GnostrNip07Response *gnostr_nip07_get_relays_finish(GAsyncResult *result,
                                                     GError **error);

/**
 * gnostr_nip07_encrypt:
 * @recipient_pubkey: The recipient's public key in hex format
 * @plaintext: The plaintext to encrypt
 * @use_nip44: If TRUE, use NIP-44 encryption; if FALSE, use NIP-04
 * @error: (out) (optional): Return location for error
 *
 * Request encryption via D-Bus.
 * Equivalent to window.nostr.nip04.encrypt() or window.nostr.nip44.encrypt().
 *
 * Returns: (transfer full): A new response structure containing the ciphertext,
 *          or NULL on D-Bus communication error. Caller frees with
 *          gnostr_nip07_response_free().
 */
GnostrNip07Response *gnostr_nip07_encrypt(const char *recipient_pubkey,
                                           const char *plaintext,
                                           gboolean use_nip44,
                                           GError **error);

/**
 * gnostr_nip07_encrypt_async:
 * @recipient_pubkey: The recipient's public key in hex format
 * @plaintext: The plaintext to encrypt
 * @use_nip44: If TRUE, use NIP-44 encryption; if FALSE, use NIP-04
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_nip07_encrypt().
 */
void gnostr_nip07_encrypt_async(const char *recipient_pubkey,
                                 const char *plaintext,
                                 gboolean use_nip44,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

/**
 * gnostr_nip07_encrypt_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async encrypt operation.
 *
 * Returns: (transfer full): The response, or NULL on error.
 */
GnostrNip07Response *gnostr_nip07_encrypt_finish(GAsyncResult *result,
                                                  GError **error);

/**
 * gnostr_nip07_decrypt:
 * @sender_pubkey: The sender's public key in hex format
 * @ciphertext: The ciphertext to decrypt
 * @use_nip44: If TRUE, use NIP-44 decryption; if FALSE, use NIP-04
 * @error: (out) (optional): Return location for error
 *
 * Request decryption via D-Bus.
 * Equivalent to window.nostr.nip04.decrypt() or window.nostr.nip44.decrypt().
 *
 * Returns: (transfer full): A new response structure containing the plaintext,
 *          or NULL on D-Bus communication error. Caller frees with
 *          gnostr_nip07_response_free().
 */
GnostrNip07Response *gnostr_nip07_decrypt(const char *sender_pubkey,
                                           const char *ciphertext,
                                           gboolean use_nip44,
                                           GError **error);

/**
 * gnostr_nip07_decrypt_async:
 * @sender_pubkey: The sender's public key in hex format
 * @ciphertext: The ciphertext to decrypt
 * @use_nip44: If TRUE, use NIP-44 decryption; if FALSE, use NIP-04
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback to invoke when operation completes
 * @user_data: User data for callback
 *
 * Asynchronous version of gnostr_nip07_decrypt().
 */
void gnostr_nip07_decrypt_async(const char *sender_pubkey,
                                 const char *ciphertext,
                                 gboolean use_nip44,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data);

/**
 * gnostr_nip07_decrypt_finish:
 * @result: A #GAsyncResult
 * @error: (out) (optional): Return location for error
 *
 * Finishes an async decrypt operation.
 *
 * Returns: (transfer full): The response, or NULL on error.
 */
GnostrNip07Response *gnostr_nip07_decrypt_finish(GAsyncResult *result,
                                                  GError **error);

/* ---- Utility Functions ---- */

/**
 * gnostr_nip07_format_unsigned_event:
 * @kind: The event kind
 * @content: The event content
 * @tags_json: (nullable): Tags as JSON array string, or NULL for empty
 * @created_at: Timestamp (0 to use current time)
 *
 * Helper to format an unsigned event for signing.
 * Creates a JSON string suitable for signEvent().
 *
 * Returns: (transfer full): A newly allocated JSON string, or NULL on error.
 *          Caller must free with g_free().
 */
char *gnostr_nip07_format_unsigned_event(gint kind,
                                          const char *content,
                                          const char *tags_json,
                                          gint64 created_at);

/**
 * gnostr_nip07_parse_signed_event:
 * @signed_event_json: The signed event JSON
 * @out_id: (out) (optional): Location for event ID (hex)
 * @out_pubkey: (out) (optional): Location for pubkey (hex)
 * @out_sig: (out) (optional): Location for signature (hex)
 * @out_kind: (out) (optional): Location for event kind
 * @out_created_at: (out) (optional): Location for timestamp
 *
 * Parse a signed event and extract its fields.
 *
 * Returns: TRUE on success, FALSE on parse error.
 */
gboolean gnostr_nip07_parse_signed_event(const char *signed_event_json,
                                          char **out_id,
                                          char **out_pubkey,
                                          char **out_sig,
                                          gint *out_kind,
                                          gint64 *out_created_at);

/**
 * gnostr_nip07_parse_relays:
 * @relays_json: The relay map JSON from getRelays()
 *
 * Parse relay map JSON into a list of relay structures.
 *
 * Returns: (transfer full) (element-type GnostrNip07Relay): A list of relays,
 *          or NULL on error. Free with g_list_free_full(list, (GDestroyNotify)gnostr_nip07_relay_free).
 */
GList *gnostr_nip07_parse_relays(const char *relays_json);

/**
 * gnostr_nip07_service_available:
 *
 * Check if a NIP-07 D-Bus service is available on the session bus.
 *
 * Returns: TRUE if a NIP-07 service is available, FALSE otherwise.
 */
gboolean gnostr_nip07_service_available(void);

/* Error domain */
#define GNOSTR_NIP07_ERROR (gnostr_nip07_error_quark())
GQuark gnostr_nip07_error_quark(void);

/**
 * GnostrNip07Error:
 * @GNOSTR_NIP07_ERROR_NOT_AVAILABLE: No NIP-07 service available
 * @GNOSTR_NIP07_ERROR_USER_REJECTED: User rejected the request
 * @GNOSTR_NIP07_ERROR_INVALID_EVENT: Invalid event format
 * @GNOSTR_NIP07_ERROR_INVALID_PUBKEY: Invalid public key format
 * @GNOSTR_NIP07_ERROR_ENCRYPTION_FAILED: Encryption/decryption failed
 * @GNOSTR_NIP07_ERROR_TIMEOUT: Request timed out
 * @GNOSTR_NIP07_ERROR_FAILED: Generic failure
 */
typedef enum {
  GNOSTR_NIP07_ERROR_NOT_AVAILABLE,
  GNOSTR_NIP07_ERROR_USER_REJECTED,
  GNOSTR_NIP07_ERROR_INVALID_EVENT,
  GNOSTR_NIP07_ERROR_INVALID_PUBKEY,
  GNOSTR_NIP07_ERROR_ENCRYPTION_FAILED,
  GNOSTR_NIP07_ERROR_TIMEOUT,
  GNOSTR_NIP07_ERROR_FAILED
} GnostrNip07Error;

G_END_DECLS

#endif /* GNOSTR_NIP07_EXTENSION_H */
