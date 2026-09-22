#ifndef NOSTR_NIP46_CLIENT_H
#define NOSTR_NIP46_CLIENT_H

#include "nostr/nip46/nip46_types.h"
#include "nostr/nip46/nip46_uri.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* nostrc-5wj9: Async RPC callback and error types.
 * The callback fires on a background thread — callers must marshal
 * to the GTK/GLib main thread if they need to touch UI.
 *
 * result_json: heap-allocated result on success (caller frees with free()), NULL on error.
 * error_msg:   heap-allocated error string on failure (caller frees with free()), NULL on success.
 * Ownership transfers exactly once to a non-NULL callback. The const qualifier
 * is retained for source compatibility; cast away const only when passing the
 * allocation to free(). With a NULL callback the library discards both values.
 */
typedef void (*NostrNip46AsyncCallback)(NostrNip46Session *session,
                                        const char *result_json,
                                        const char *error_msg,
                                        void *user_data);

/* Default RPC request timeout in milliseconds */
#define NOSTR_NIP46_DEFAULT_TIMEOUT_MS 30000

/* Client session (no GLib) */
NostrNip46Session *nostr_nip46_client_new(void);

/* bunker_uri may be bunker:// or nostrconnect:// per spec. requested_perms_csv optional.
 * URI secret= is retained as a connect authorization token. For a client
 * bunker:// session an independent transport key is generated locally; callers
 * may replace it explicitly with nostr_nip46_client_set_secret(). */
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

/* nostrc-prkl: Tune the client-side RPC rate limit (signer-relay flood
 * protection). All RPC round-trips (sign_event, nip04/nip44 encrypt/decrypt
 * RPC variants, connect, get_public_key) share one gate per session:
 *  - max_inflight: maximum concurrent RPC round-trips (<= 0 keeps the
 *    built-in default of 4);
 *  - min_interval_ms: minimum milliseconds between request publishes
 *    (0 keeps the built-in default of 150 ms).
 * Callers beyond the cap block until a slot frees; bursts are serialized
 * into an evenly paced trickle instead of flooding the signer relays. */
void nostr_nip46_client_set_rate_limit(NostrNip46Session *s,
                                       int max_inflight,
                                       uint32_t min_interval_ms);

/* Algorithm-specific transport compatibility helpers using s->secret.
 * New network paths should use nostr_nip46_transport_encrypt/decrypt so the
 * session-owned negotiated mode is applied consistently. These explicit
 * helpers remain for compatibility/tests and never mutate session policy.
 * Do NOT use them for user content — use the _rpc variants below instead. */
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

/* Binary-safe NIP-44 content encrypt/decrypt via remote signer RPC.
 *
 * NIP-46 params are JSON strings, so a plaintext that is not valid UTF-8
 * cannot survive the _rpc pair above — it is mangled before it ever reaches
 * the bunker. These carry the plaintext side as standard base64
 * (nip44_encrypt_b64 / nip44_decrypt_b64); the ciphertext is an ordinary
 * NIP-44 v2 payload either way, so a recipient decrypts it with whichever
 * lane suits its own payload. Use these for fixed-width binary payloads
 * whose width is the format signal (Concord CORD-06 rekey blobs, for
 * example), where a substituted byte is not a recoverable error.
 *
 * A bunker without the capability returns an error rather than encrypting
 * base64 text as if it were the plaintext.
 *
 * On success *out_ciphertext / *out_plaintext are freshly allocated and the
 * caller frees them with free(). */
int nostr_nip46_client_nip44_encrypt_b64_rpc(NostrNip46Session *s, const char *peer_pubkey_hex,
                                             const uint8_t *plaintext, size_t plaintext_len,
                                             char **out_ciphertext);
int nostr_nip46_client_nip44_decrypt_b64_rpc(NostrNip46Session *s, const char *peer_pubkey_hex,
                                             const char *ciphertext,
                                             uint8_t **out_plaintext, size_t *out_plaintext_len);

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

/* C2 (nostrc-ot2c.3): Per-request cancellation handle + deadline options.
 *
 * NostrNip46CancelHandle is an opaque, thread-safe, refcounted handle that
 * lets a caller cancel a specific in-flight RPC without racing the
 * session-wide cancel_all path. It is safe to hold across threads and to
 * call `_cancel()` from any thread (including a signal handler-adjacent
 * context such as GCancellable::cancelled).
 *
 * A single handle may be attached to at most one active request at a time;
 * cancelling it fires the wake-up on the currently-attached request (if
 * any) and marks the handle so that any subsequent RPC started with the
 * same handle also aborts immediately.
 *
 * Lifetime: caller owns a reference from _new(); pass to the RPC via
 * NostrNip46RequestOptions.cancel_handle; when done, call _unref(). The
 * RPC internally takes an extra ref for the duration of the wait.
 */
typedef struct NostrNip46CancelHandle NostrNip46CancelHandle;

NostrNip46CancelHandle *nostr_nip46_cancel_handle_new(void);
NostrNip46CancelHandle *nostr_nip46_cancel_handle_ref(NostrNip46CancelHandle *h);
void nostr_nip46_cancel_handle_unref(NostrNip46CancelHandle *h);
void nostr_nip46_cancel_handle_cancel(NostrNip46CancelHandle *h);
int  nostr_nip46_cancel_handle_is_cancelled(const NostrNip46CancelHandle *h);

/* Per-request options carried into the _opts sync/async entrypoints below.
 *   - deadline_ms: absolute deadline on CLOCK_MONOTONIC in milliseconds. 0
 *     falls back to the session default (nostr_nip46_client_get_timeout).
 *     The deadline covers the entire RPC (rate-limit gate, publish, and
 *     response wait); once reached the call returns with an error.
 *   - cancel_handle: optional cancellation handle. When cancelled the
 *     response wait is interrupted at the next 500 ms poll boundary at
 *     the latest, and the pending request is torn down safely.
 * Both fields may be zero/NULL to fall back to legacy behaviour. */
typedef struct {
    int64_t deadline_ms;
    NostrNip46CancelHandle *cancel_handle;
} NostrNip46RequestOptions;

/* Sync _opts variants. On success, *out_signed_event_json / *out_result /
 * *out_user_pubkey_hex is a fresh heap allocation the caller frees with
 * free(). Old signatures above remain wrappers around these. */
int nostr_nip46_client_sign_event_opts(NostrNip46Session *s,
                                       const char *event_json,
                                       const NostrNip46RequestOptions *opts,
                                       char **out_signed_event_json);
int nostr_nip46_client_connect_rpc_opts(NostrNip46Session *s,
                                        const char *connect_secret,
                                        const char *perms,
                                        const NostrNip46RequestOptions *opts,
                                        char **out_result);
int nostr_nip46_client_get_public_key_rpc_opts(NostrNip46Session *s,
                                               const NostrNip46RequestOptions *opts,
                                               char **out_user_pubkey_hex);

/* Async _opts variants. Ownership of the cancel handle (if any) stays with
 * the caller; the RPC takes its own reference for the duration. */
void nostr_nip46_client_sign_event_async_opts(NostrNip46Session *s,
                                              const char *event_json,
                                              const NostrNip46RequestOptions *opts,
                                              NostrNip46AsyncCallback callback,
                                              void *user_data);
void nostr_nip46_client_connect_rpc_async_opts(NostrNip46Session *s,
                                               const char *connect_secret,
                                               const char *perms,
                                               const NostrNip46RequestOptions *opts,
                                               NostrNip46AsyncCallback callback,
                                               void *user_data);
void nostr_nip46_client_get_public_key_rpc_async_opts(NostrNip46Session *s,
                                                      const NostrNip46RequestOptions *opts,
                                                      NostrNip46AsyncCallback callback,
                                                      void *user_data);

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

/* nostrc-z1fb Phase 1: client-initiated (`nostrconnect://`) QR login helpers.
 *
 * The client mints an ephemeral secp256k1 keypair and a 16-byte random
 * pairing secret (RAND_bytes), populates `s` (client_pubkey_hex from the
 * derived pubkey, secret = ephemeral private key, connect_token = the
 * 32-hex-char pairing secret, relays = supplied list) and emits a
 * `nostrconnect://<pk>?relay=…&secret=…&perms=…&name=…` URI ready for QR
 * rendering. Perms / name are optional (pass NULL to omit).
 *
 * The URI embeds the pairing secret; treat *out_uri as sensitive at every
 * callsite. The ephemeral private key never leaves the session (in
 * s->secret, wiped in session_destroy).
 *
 * Returns 0 on success, -1 on error. */
int nostr_nip46_client_new_qr_session(NostrNip46Session *s,
                                      const char *const *relays,
                                      size_t n_relays,
                                      const char *perms_csv,
                                      const char *name,
                                      char **out_uri);

/* nostrc-z1fb Phase 1: wait for an unsolicited `connect` from a signer that
 * scanned this session's `nostrconnect://` URI.
 *
 * The client must already have `client_start()` running so the persistent
 * per-relay subscription is up (C3 subscription-before-publish). This call
 * arms an "unsolicited-connect" waiter alongside the pending-request table
 * so `nip46_persistent_client_cb` routes a signer-initiated `connect`
 * (which carries an id the client never issued) to this waiter instead of
 * dropping it.
 *
 * Accepts BOTH interop shapes emitted by real signers:
 *   {"id":"…","method":"connect","params":[<client_pk>,<secret>,<perms>]}
 *   {"id":"…","result":"<secret>"}
 *
 * The secret is compared in constant time; on match the waiter records the
 * event author as the signer pubkey (via nostr_nip46_client_set_signer_pubkey)
 * and returns 0 with *out_signer_pubkey_hex filled with a fresh heap copy
 * the caller must free(). Wrong / absent secret → dropped silently, the
 * wait continues. The secret is single-use: on first successful match the
 * waiter is invalidated so a replay cannot re-adopt a different signer.
 *
 * Returns 0 on success, -1 on timeout / error / cancellation. */
int nostr_nip46_client_await_connect(NostrNip46Session *s,
                                     const char *expected_secret,
                                     uint32_t timeout_ms,
                                     char **out_signer_pubkey_hex);

/* nostrc-z1fb Phase 1: test-only ingest hook.
 *
 * Deterministic tests do NOT run a real relay pool, so they cannot exercise
 * `await_connect` via `nip46_persistent_client_cb`. This helper accepts a
 * plaintext NIP-46 request/response as if it had just been decrypted and
 * runs the same dispatch (pending-request delivery first, then unsolicited-
 * connect waiter). Not for production callers.
 *
 * Returns 1 if dispatched to the connect waiter, 0 if dispatched to a
 * pending request (or dropped), -1 on invalid input. */
int nostr_nip46_client_test_ingest_plaintext(NostrNip46Session *s,
                                             const char *sender_pubkey_hex,
                                             const char *plaintext_json);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_CLIENT_H */
