/* native_messaging.h - NIP-07 browser extension native messaging support
 *
 * Implements Chrome/Firefox native messaging protocol for NIP-07 browser extension
 * communication. Provides getPublicKey, signEvent, getRelays, nip04.encrypt/decrypt.
 *
 * Native messaging protocol:
 * - Reads 4-byte little-endian length prefix + JSON from stdin
 * - Writes 4-byte little-endian length prefix + JSON to stdout
 * - Each message is a JSON object with method, params, id
 */
#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Error codes */
typedef enum {
  NM_OK = 0,
  NM_ERR_IO,              /* I/O error reading/writing */
  NM_ERR_MSG_TOO_LARGE,   /* Message exceeds size limit */
  NM_ERR_INVALID_JSON,    /* Message is not valid JSON */
  NM_ERR_INVALID_REQUEST, /* Request missing required fields */
  NM_ERR_UNKNOWN_METHOD,  /* Unknown NIP-07 method */
  NM_ERR_USER_DENIED,     /* User denied the request */
  NM_ERR_NO_KEY,          /* No key available */
  NM_ERR_SIGN_FAILED,     /* Signing failed */
  NM_ERR_ENCRYPT_FAILED,  /* Encryption failed */
  NM_ERR_DECRYPT_FAILED,  /* Decryption failed */
  NM_ERR_INTERNAL         /* Internal error */
} NativeMessagingError;

/* NIP-07 method types */
typedef enum {
  NIP07_METHOD_UNKNOWN = 0,
  NIP07_METHOD_GET_PUBLIC_KEY,
  NIP07_METHOD_SIGN_EVENT,
  NIP07_METHOD_GET_RELAYS,
  NIP07_METHOD_NIP04_ENCRYPT,
  NIP07_METHOD_NIP04_DECRYPT,
  NIP07_METHOD_NIP44_ENCRYPT,
  NIP07_METHOD_NIP44_DECRYPT
} Nip07Method;

/* Request structure */
typedef struct {
  gchar *id;              /* Request ID for response correlation */
  Nip07Method method;     /* NIP-07 method */
  gchar *method_str;      /* Original method string */

  /* Method-specific params */
  union {
    struct {
      gchar *event_json;  /* Event JSON for sign_event */
    } sign_event;

    struct {
      gchar *pubkey;      /* Peer public key (hex) */
      gchar *plaintext;   /* Text to encrypt */
    } encrypt;

    struct {
      gchar *pubkey;      /* Peer public key (hex) */
      gchar *ciphertext;  /* Text to decrypt */
    } decrypt;
  } params;

  /* Origin info for policy decisions */
  gchar *origin;          /* Browser extension origin */
} NativeMessagingRequest;

/* Response structure */
typedef struct {
  gchar *id;              /* Request ID */
  gboolean success;       /* Whether request succeeded */

  /* On success, one of these is set */
  gchar *result_str;      /* String result (pubkey, signature, etc.) */
  gchar *result_json;     /* JSON result (event, relays) */

  /* On failure */
  NativeMessagingError error_code;
  gchar *error_message;
} NativeMessagingResponse;

/* Callback for authorization prompts
 * Returns TRUE if user approves, FALSE if denied
 */
typedef gboolean (*NativeMessagingAuthorizeCb)(const NativeMessagingRequest *req,
                                               const gchar *preview,
                                               gpointer user_data);

/* Context for native messaging handler */
typedef struct _NativeMessagingContext NativeMessagingContext;

/* Create a new native messaging context
 * @identity: npub or key_id to use for signing (NULL for default)
 */
NativeMessagingContext *native_messaging_context_new(const gchar *identity);

/* Free the context */
void native_messaging_context_free(NativeMessagingContext *ctx);

/* Set authorization callback for interactive approval */
void native_messaging_set_authorize_cb(NativeMessagingContext *ctx,
                                       NativeMessagingAuthorizeCb cb,
                                       gpointer user_data);

/* Set auto-approve mode (for specific event kinds)
 * @kinds: NULL-terminated array of kind numbers to auto-approve
 */
void native_messaging_set_auto_approve_kinds(NativeMessagingContext *ctx,
                                             const gint *kinds);

/* Read a single message from stdin
 * Returns: allocated JSON string, or NULL on EOF/error
 */
gchar *native_messaging_read_message(NativeMessagingError *out_error);

/* Write a response message to stdout
 * @response: Response structure to serialize
 * Returns: 0 on success, error code on failure
 */
NativeMessagingError native_messaging_write_response(const NativeMessagingResponse *response);

/* Write a raw JSON string to stdout
 * @json: JSON string to write
 * Returns: 0 on success, error code on failure
 */
NativeMessagingError native_messaging_write_json(const gchar *json);

/* Parse a request message
 * @json: JSON string from native_messaging_read_message
 * @out_req: Output request structure (caller frees with native_messaging_request_free)
 * Returns: 0 on success, error code on failure
 */
NativeMessagingError native_messaging_parse_request(const gchar *json,
                                                    NativeMessagingRequest **out_req);

/* Process a request and generate response
 * @ctx: Native messaging context
 * @req: Request to process
 * @out_resp: Output response (caller frees with native_messaging_response_free)
 * Returns: 0 on success, error code on failure
 */
NativeMessagingError native_messaging_process_request(NativeMessagingContext *ctx,
                                                      const NativeMessagingRequest *req,
                                                      NativeMessagingResponse **out_resp);

/* Run the main message loop (blocking)
 * Reads messages from stdin, processes them, writes responses to stdout.
 * Returns when stdin is closed or on fatal error.
 * @ctx: Native messaging context
 * Returns: 0 on clean shutdown, error code on failure
 */
NativeMessagingError native_messaging_run(NativeMessagingContext *ctx);

/* Free a request structure */
void native_messaging_request_free(NativeMessagingRequest *req);

/* Free a response structure */
void native_messaging_response_free(NativeMessagingResponse *resp);

/* Build a success response */
NativeMessagingResponse *native_messaging_response_success(const gchar *id,
                                                           const gchar *result);

/* Build a success response with JSON result */
NativeMessagingResponse *native_messaging_response_success_json(const gchar *id,
                                                                const gchar *result_json);

/* Build an error response */
NativeMessagingResponse *native_messaging_response_error(const gchar *id,
                                                         NativeMessagingError code,
                                                         const gchar *message);

/* Get error message for error code */
const gchar *native_messaging_error_message(NativeMessagingError code);

/* Maximum message size (1MB, per Chrome's limit) */
#define NATIVE_MESSAGING_MAX_SIZE (1024 * 1024)

G_END_DECLS
