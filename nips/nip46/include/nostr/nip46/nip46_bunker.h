#ifndef NOSTR_NIP46_BUNKER_H
#define NOSTR_NIP46_BUNKER_H

#include "nostr/nip46/nip46_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*NostrNip46AuthorizeFn)(const char *client_pubkey_hex, const char *perms_csv, void *user_data);
typedef char *(*NostrNip46SignFn)(const char *event_json, void *user_data); /* return malloc'd json or NULL */

typedef struct {
    NostrNip46AuthorizeFn authorize_cb;
    NostrNip46SignFn sign_cb;
    void *user_data;
} NostrNip46BunkerCallbacks;

/* Opt-in deferred server API. Relay-delivered requests are decrypted and bound
 * to a signature-verified event author/recipient. Direct handle_cipher callers
 * must verify the outer event themselves (see below). Retain the borrowed
 * request before returning; complete/unref it on any thread. Completion is
 * once-only and is suppressed after stop. Result is a JSON token, not raw text.
 * Install handlers/transport before listen; stop before replacing handlers. */
typedef struct NostrNip46BunkerRequest NostrNip46BunkerRequest;
typedef void (*NostrNip46RequestHandler)(NostrNip46BunkerRequest *, void *);
typedef void (*NostrNip46BunkerRelayHandler)(const char *relay, const char *state, void *);
int nostr_nip46_bunker_get_transport_pubkey(const NostrNip46Session *, char **out_hex);
int nostr_nip46_bunker_set_transport_key(NostrNip46Session *, const unsigned char key[32]);
int nostr_nip46_bunker_set_request_handler(NostrNip46Session *, NostrNip46RequestHandler, void *);
int nostr_nip46_bunker_set_relay_handler(NostrNip46Session *, NostrNip46BunkerRelayHandler, void *);
void nostr_nip46_bunker_stop(NostrNip46Session *);
NostrNip46BunkerRequest *nostr_nip46_bunker_request_ref(NostrNip46BunkerRequest *);
void nostr_nip46_bunker_request_unref(NostrNip46BunkerRequest *);
const NostrNip46Request *nostr_nip46_bunker_request_rpc(const NostrNip46BunkerRequest *);
const char *nostr_nip46_bunker_request_client(const NostrNip46BunkerRequest *);
const char *nostr_nip46_bunker_request_event_id(const NostrNip46BunkerRequest *);
int64_t nostr_nip46_bunker_request_created_at(const NostrNip46BunkerRequest *);
int nostr_nip46_bunker_request_complete(NostrNip46BunkerRequest *, const char *result_json, const char *error);
/* Called once on the publication worker or stopping thread; marshal any
 * thread-affine work. Enqueue success transfers data to notify; failure does not. */
typedef void (*NostrNip46PublishNotify)(int accepted, void *data);
typedef struct NostrNip46CancelHandle NostrNip46CancelHandle;
int nostr_nip46_bunker_request_complete_full(NostrNip46BunkerRequest *, const char *result_json,
    const char *error, NostrNip46CancelHandle *, NostrNip46PublishNotify, void *data);

NostrNip46Session *nostr_nip46_bunker_new(const NostrNip46BunkerCallbacks *cbs);
int nostr_nip46_bunker_listen(NostrNip46Session *s, const char *const *relays, size_t n_relays);
int nostr_nip46_bunker_issue_bunker_uri(NostrNip46Session *s, const char *remote_signer_pubkey_hex, const char *const *relays, size_t n_relays, const char *secret, char **out_uri);
int nostr_nip46_bunker_reply(NostrNip46Session *s, const NostrNip46Request *req, const char *result_or_json, const char *error_or_null);
/* Decrypt with the session-owned transport mode and dispatch. Legacy mode
 * returns an encrypted response (0); request-aware mode returns deferred (1)
 * with *out_cipher_reply == NULL. Direct callers MUST authenticate the outer
 * event signature, author and recipient before passing client_pubkey_hex.
 * Direct requests have no event ID; use relay delivery for replay-safe adapters. */
int nostr_nip46_bunker_handle_cipher(NostrNip46Session *s,
                                     const char *client_pubkey_hex,
                                     const char *ciphertext,
                                     char **out_cipher_reply);

/* nostrc-z1fb Phase 1: consume a `nostrconnect://` URI (the headless "phone
 * signer" side of QR login).
 *
 * Parses the URI (client_pubkey, relays, secret, perms), sets the bunker's
 * transport identity from the session's existing bunker secret, calls
 * `nostr_nip46_bunker_listen()` on the URI's relays (subscription BEFORE
 * publish, per C3), grants the client ACL for the requested perms, and
 * publishes a NIP-46 `connect` request event addressed to the client
 * pubkey. The event payload is
 *   {"id":"…","method":"connect","params":[<client_pk>,<secret>,<perms>]}
 * — the same shape a mobile signer emits when it scans the QR.
 *
 * After this returns 0 the bunker keeps listening; incoming `sign_event` /
 * `get_public_key` are handled by the existing `bunker_handle_cipher` path
 * with the ACL just granted (secret is never re-checked — the client
 * validates it on receipt of this event).
 *
 * With a request-aware handler installed, no ACL is granted and an explicit
 * transport key must already be installed. Admission remains the handler's job.
 * Returns 0 on success, -1 on error. */
int nostr_nip46_bunker_connect_to_client(NostrNip46Session *s, const char *nostrconnect_uri);
/* Relay URL of the authenticated delivery, NULL for direct-cipher callers. */
const char *nostr_nip46_bunker_request_relay(const NostrNip46BunkerRequest *request);
/* Queue the signer-initiated connect RESPONSE (result = client secret).
 * The worker checks cancellation before encryption and transport handoff. */
int nostr_nip46_bunker_connect_response(NostrNip46Session *s, const char *client_hex, const char *secret,
    const char *const *approved_relays, size_t n_relays, NostrNip46CancelHandle *cancel,
    NostrNip46PublishNotify notify, void *data);
/* Owner-thread, request-handler mode only. Preserve unchanged relay subscriptions
 * and in-flight requests. URLs must already have passed the embedding policy. */
int nostr_nip46_bunker_reconfigure_relays(NostrNip46Session *s, const char *const *relays, size_t n_relays);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_BUNKER_H */
