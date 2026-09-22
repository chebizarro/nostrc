#ifndef NOSTR_NIP46_BUNKER_H
#define NOSTR_NIP46_BUNKER_H

#include "nostr/nip46/nip46_types.h"

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

NostrNip46Session *nostr_nip46_bunker_new(const NostrNip46BunkerCallbacks *cbs);
int nostr_nip46_bunker_listen(NostrNip46Session *s, const char *const *relays, size_t n_relays);
int nostr_nip46_bunker_issue_bunker_uri(NostrNip46Session *s, const char *remote_signer_pubkey_hex, const char *const *relays, size_t n_relays, const char *secret, char **out_uri);
int nostr_nip46_bunker_reply(NostrNip46Session *s, const NostrNip46Request *req, const char *result_or_json, const char *error_or_null);
/* Decrypt with the session-owned transport mode, dispatch, and encrypt the response with that same mode. */
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
 * Returns 0 on success, -1 on error. */
int nostr_nip46_bunker_connect_to_client(NostrNip46Session *s, const char *nostrconnect_uri);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_BUNKER_H */
