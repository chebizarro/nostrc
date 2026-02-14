#ifndef NOSTR_NIP46_CLIENT_H
#define NOSTR_NIP46_CLIENT_H

#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nostrc-5wj9: Async RPC callback and error types.
 * The callback fires on a background thread — callers must marshal
 * to the GTK/GLib main thread if they need to touch UI.
 *
 * result_json: heap-allocated result on success (caller frees), NULL on error.
 * error_msg:   heap-allocated error string on failure (caller frees), NULL on success.
 */
typedef void (*NostrNip46AsyncCallback)(NostrNip46Session *session,
                                        const char *result_json,
                                        const char *error_msg,
                                        void *user_data);

/* Default RPC request timeout in milliseconds */
#define NOSTR_NIP46_DEFAULT_TIMEOUT_MS 30000

/* Client session (no GLib) */
NostrNip46Session *nostr_nip46_client_new(void);

/* bunker_uri may be bunker:// or nostrconnect:// per spec. requested_perms_csv optional. */
int nostr_nip46_client_connect(NostrNip46Session *s,
                               const char *bunker_uri,
                               const char *requested_perms_csv);

int nostr_nip46_client_get_public_key(NostrNip46Session *s, char **out_user_pubkey_hex);

/* nostrc-nip46-rpc: Send connect RPC to remote signer.
 * This must be called after parsing bunker:// URI but before other operations.
 * The session must have: remote_pubkey_hex, secret (client key), relays.
 * connect_secret: the secret= value from bunker URI (may be NULL)
 * perms: requested permissions CSV (may be NULL)
 * On success, out_result contains "ack" or the connect secret. Caller must free. */
int nostr_nip46_client_connect_rpc(NostrNip46Session *s,
                                   const char *connect_secret,
                                   const char *perms,
                                   char **out_result);

/* nostrc-nip46-rpc: Send get_public_key RPC to remote signer.
 * Returns the user's actual pubkey (may differ from remote_signer_pubkey).
 * On success, out_user_pubkey_hex contains hex pubkey. Caller must free. */
int nostr_nip46_client_get_public_key_rpc(NostrNip46Session *s, char **out_user_pubkey_hex);

/* Set the remote signer's pubkey (received after connect handshake) */
int nostr_nip46_client_set_signer_pubkey(NostrNip46Session *s, const char *signer_pubkey_hex);

/* nostrc-1wfi: Set the client's secret key directly (for ECDH encryption).
 * This is the client's secp256k1 private key, NOT the URI's secret param.
 * The secret_hex must be a 64-character hex string (32 bytes). */
int nostr_nip46_client_set_secret(NostrNip46Session *s, const char *secret_hex);

int nostr_nip46_client_sign_event(NostrNip46Session *s, const char *event_json, char **out_signed_event_json);
int nostr_nip46_client_ping(NostrNip46Session *s);

/* NIP-46 TRANSPORT-LEVEL local-crypto using s->secret (client communication key).
 * Use ONLY for encrypting/decrypting NIP-46 protocol messages (kind 24133).
 * Do NOT use for user content — use the _rpc variants below instead. */
int nostr_nip46_client_nip04_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext);
int nostr_nip46_client_nip04_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext);
int nostr_nip46_client_nip44_encrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext);
int nostr_nip46_client_nip44_decrypt(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext);

/* nostrc-u1qh: NIP-46 CONTENT encrypt/decrypt via REMOTE SIGNER RPC.
 * Delegates to the remote signer which holds the user's actual private key.
 * The client NEVER has the user's key — s->secret is the NIP-46 transport key only.
 * Use these for all user content encryption (DMs, NIP-44 encrypted content, etc.). */
int nostr_nip46_client_nip04_encrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext);
int nostr_nip46_client_nip04_decrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext);
int nostr_nip46_client_nip44_encrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *plaintext, char **out_ciphertext);
int nostr_nip46_client_nip44_decrypt_rpc(NostrNip46Session *s, const char *peer_pubkey_hex, const char *ciphertext, char **out_plaintext);

/* nostrc-j2yu: Persistent connection API.
 * Start a persistent relay connection for efficient RPC calls.
 * This should be called after nostr_nip46_client_connect() has parsed the bunker URI.
 * Once started, all RPC calls reuse the same relay connections instead of
 * connecting/disconnecting per call.
 * Returns 0 on success, -1 on failure. */
int nostr_nip46_client_start(NostrNip46Session *s);

/* nostrc-j2yu: Stop the persistent relay connection.
 * Safe to call multiple times or if never started. */
void nostr_nip46_client_stop(NostrNip46Session *s);

/* nostrc-j2yu: Check if the persistent client pool is running.
 * Returns 1 if running, 0 if not. */
int nostr_nip46_client_is_running(NostrNip46Session *s);

void nostr_nip46_session_free(NostrNip46Session *s);

/* nostrc-5wj9: Configurable request timeout.
 * Set the timeout for RPC requests in milliseconds.
 * Pass 0 to reset to NOSTR_NIP46_DEFAULT_TIMEOUT_MS. */
void nostr_nip46_client_set_timeout(NostrNip46Session *s, uint32_t timeout_ms);

/* nostrc-5wj9: Get the current request timeout in milliseconds. */
uint32_t nostr_nip46_client_get_timeout(const NostrNip46Session *s);

/* nostrc-5wj9: Async RPC API.
 * These functions return immediately and invoke callback on completion.
 * The callback fires on a background thread.
 * Pass NULL for callback to fire-and-forget (result is discarded). */

void nostr_nip46_client_sign_event_async(NostrNip46Session *s,
                                          const char *event_json,
                                          NostrNip46AsyncCallback callback,
                                          void *user_data);

void nostr_nip46_client_connect_rpc_async(NostrNip46Session *s,
                                           const char *connect_secret,
                                           const char *perms,
                                           NostrNip46AsyncCallback callback,
                                           void *user_data);

void nostr_nip46_client_get_public_key_rpc_async(NostrNip46Session *s,
                                                  NostrNip46AsyncCallback callback,
                                                  void *user_data);

/* nostrc-5wj9: Cancel all pending async RPC requests.
 * Callbacks for cancelled requests will fire with error_msg="cancelled". */
void nostr_nip46_client_cancel_all(NostrNip46Session *s);

/* nostrc-32yf: Session state machine.
 * State transitions:
 *   DISCONNECTED -> CONNECTING  (client_start called)
 *   CONNECTING   -> CONNECTED   (relay connected, subscription active)
 *   CONNECTING   -> DISCONNECTED (connection timeout)
 *   CONNECTED    -> STOPPING    (client_stop called)
 *   STOPPING     -> DISCONNECTED (cleanup complete)
 */
typedef enum {
    NOSTR_NIP46_STATE_DISCONNECTED = 0,
    NOSTR_NIP46_STATE_CONNECTING,
    NOSTR_NIP46_STATE_CONNECTED,
    NOSTR_NIP46_STATE_STOPPING
} NostrNip46State;

/* nostrc-32yf: Query the current session state. */
NostrNip46State nostr_nip46_client_get_state_public(const NostrNip46Session *s);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_CLIENT_H */
