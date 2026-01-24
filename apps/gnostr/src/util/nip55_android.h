/**
 * NIP-55 Android Signer Application Support
 *
 * NIP-55 defines how Android apps communicate with signer applications
 * via Android Intents. This module provides the CLIENT-side implementation
 * that can:
 *   1. Detect if running on Android (via environment)
 *   2. Generate intent URIs for Android signer apps
 *   3. Parse responses from signer apps
 *
 * Intent URI format:
 *   intent://sign?event=<unsigned-event-json>#Intent;scheme=nostrsigner;package=<signer-package>;end
 *
 * Common intents:
 *   - nostrsigner://sign        - sign an event
 *   - nostrsigner://encrypt     - NIP-04/44 encrypt
 *   - nostrsigner://decrypt     - NIP-04/44 decrypt
 *   - nostrsigner://get_public_key - get user's pubkey
 *
 * This is primarily a protocol implementation for future Android/Termux builds.
 */

#ifndef GNOSTR_NIP55_ANDROID_H
#define GNOSTR_NIP55_ANDROID_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * GnostrAndroidSignerRequest:
 *
 * Types of requests that can be made to an Android signer application.
 */
typedef enum {
  GNOSTR_ANDROID_SIGNER_SIGN = 0,       /* Sign an event */
  GNOSTR_ANDROID_SIGNER_ENCRYPT,        /* NIP-04/44 encrypt */
  GNOSTR_ANDROID_SIGNER_DECRYPT,        /* NIP-04/44 decrypt */
  GNOSTR_ANDROID_SIGNER_GET_PUBLIC_KEY  /* Get user's public key */
} GnostrAndroidSignerRequest;

/**
 * GnostrAndroidEncryptionType:
 *
 * Encryption algorithm types for encrypt/decrypt requests.
 */
typedef enum {
  GNOSTR_ANDROID_ENCRYPTION_NIP04 = 0,  /* NIP-04 (legacy) encryption */
  GNOSTR_ANDROID_ENCRYPTION_NIP44       /* NIP-44 encryption */
} GnostrAndroidEncryptionType;

/**
 * GnostrAndroidSignerResponse:
 *
 * Parsed response from an Android signer application.
 */
typedef struct {
  gboolean success;          /* Whether the operation succeeded */
  char *error_message;       /* Error message if failed (owned) */

  /* For sign responses */
  char *signature;           /* Event signature (64 byte hex, owned) */
  char *signed_event_json;   /* Full signed event JSON if provided (owned) */

  /* For encrypt/decrypt responses */
  char *result_text;         /* Encrypted/decrypted text (owned) */

  /* For get_public_key responses */
  char *pubkey_hex;          /* Public key in hex format (owned) */
  char *npub;                /* Public key in bech32 npub format (owned) */
} GnostrAndroidSignerResponse;

/**
 * Known Android signer package names
 */
#define GNOSTR_ANDROID_SIGNER_AMBER "com.greenart7c3.nostrsigner"
#define GNOSTR_ANDROID_SIGNER_KEYS  "com.example.nostrkeys"  /* Placeholder */

/**
 * gnostr_android_signer_available:
 *
 * Check if the application is running on Android where signer apps may be available.
 * This checks for Android environment indicators such as:
 *   - ANDROID_ROOT environment variable
 *   - /system/build.prop file existence
 *   - Termux environment (PREFIX=/data/data/com.termux/...)
 *
 * Returns: TRUE if running on Android, FALSE otherwise
 */
gboolean gnostr_android_signer_available(void);

/**
 * gnostr_android_is_termux:
 *
 * Check if running in Termux environment specifically.
 * Termux is an Android terminal emulator that can run GTK apps.
 *
 * Returns: TRUE if running in Termux, FALSE otherwise
 */
gboolean gnostr_android_is_termux(void);

/**
 * gnostr_android_build_sign_intent:
 * @unsigned_event_json: The unsigned event JSON to sign
 * @signer_package: (nullable): The signer app package name, or NULL for default (Amber)
 * @callback_uri: (nullable): Callback URI for the result, or NULL
 *
 * Build an Android intent URI for signing an event.
 * The intent follows the NIP-55 format:
 *   intent://sign?event=<url-encoded-json>#Intent;scheme=nostrsigner;package=<pkg>;end
 *
 * Returns: (transfer full): A newly allocated intent URI string, or NULL on error.
 *          Caller must free with g_free().
 */
char *gnostr_android_build_sign_intent(const char *unsigned_event_json,
                                        const char *signer_package,
                                        const char *callback_uri);

/**
 * gnostr_android_build_encrypt_intent:
 * @plaintext: The text to encrypt
 * @recipient_pubkey: The recipient's public key in hex format
 * @encryption_type: The encryption algorithm to use (NIP-04 or NIP-44)
 * @signer_package: (nullable): The signer app package name, or NULL for default
 * @callback_uri: (nullable): Callback URI for the result, or NULL
 *
 * Build an Android intent URI for encrypting text.
 *
 * Returns: (transfer full): A newly allocated intent URI string, or NULL on error.
 *          Caller must free with g_free().
 */
char *gnostr_android_build_encrypt_intent(const char *plaintext,
                                           const char *recipient_pubkey,
                                           GnostrAndroidEncryptionType encryption_type,
                                           const char *signer_package,
                                           const char *callback_uri);

/**
 * gnostr_android_build_decrypt_intent:
 * @ciphertext: The text to decrypt
 * @sender_pubkey: The sender's public key in hex format
 * @encryption_type: The encryption algorithm that was used (NIP-04 or NIP-44)
 * @signer_package: (nullable): The signer app package name, or NULL for default
 * @callback_uri: (nullable): Callback URI for the result, or NULL
 *
 * Build an Android intent URI for decrypting text.
 *
 * Returns: (transfer full): A newly allocated intent URI string, or NULL on error.
 *          Caller must free with g_free().
 */
char *gnostr_android_build_decrypt_intent(const char *ciphertext,
                                           const char *sender_pubkey,
                                           GnostrAndroidEncryptionType encryption_type,
                                           const char *signer_package,
                                           const char *callback_uri);

/**
 * gnostr_android_build_get_public_key_intent:
 * @signer_package: (nullable): The signer app package name, or NULL for default
 * @callback_uri: (nullable): Callback URI for the result, or NULL
 *
 * Build an Android intent URI for requesting the user's public key.
 *
 * Returns: (transfer full): A newly allocated intent URI string, or NULL on error.
 *          Caller must free with g_free().
 */
char *gnostr_android_build_get_public_key_intent(const char *signer_package,
                                                  const char *callback_uri);

/**
 * gnostr_android_parse_response:
 * @response_uri: The callback URI received from the signer
 * @request_type: The type of request that was made
 *
 * Parse a response URI from an Android signer application.
 * The response format depends on the request type and signer implementation.
 *
 * Returns: (transfer full): A newly allocated response structure, or NULL on error.
 *          Caller must free with gnostr_android_signer_response_free().
 */
GnostrAndroidSignerResponse *gnostr_android_parse_response(const char *response_uri,
                                                            GnostrAndroidSignerRequest request_type);

/**
 * gnostr_android_parse_sign_response_json:
 * @json_str: JSON string containing the signed event or signature
 *
 * Parse a JSON response from a sign request.
 * The JSON may contain either:
 *   - A full signed event object
 *   - An object with just a "sig" field
 *
 * Returns: (transfer full): A newly allocated response structure, or NULL on error.
 *          Caller must free with gnostr_android_signer_response_free().
 */
GnostrAndroidSignerResponse *gnostr_android_parse_sign_response_json(const char *json_str);

/**
 * gnostr_android_signer_response_free:
 * @response: The response to free
 *
 * Free a signer response structure.
 */
void gnostr_android_signer_response_free(GnostrAndroidSignerResponse *response);

/**
 * gnostr_android_signer_request_to_string:
 * @request: The request type
 *
 * Get a string representation of a request type for debugging.
 *
 * Returns: A static string (do not free)
 */
const char *gnostr_android_signer_request_to_string(GnostrAndroidSignerRequest request);

/**
 * gnostr_android_encryption_type_to_string:
 * @type: The encryption type
 *
 * Get a string representation of an encryption type.
 *
 * Returns: A static string (do not free)
 */
const char *gnostr_android_encryption_type_to_string(GnostrAndroidEncryptionType type);

G_END_DECLS

#endif /* GNOSTR_NIP55_ANDROID_H */
