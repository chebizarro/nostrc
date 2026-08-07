#ifndef NOSTR_NIP46_TYPES_H
#define NOSTR_NIP46_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NOSTR_EVENT_KIND_NIP46 24133

/* Requests/Responses follow spec JSON, kept as strings */
typedef struct {
    char *id;
    char *method;
    char **params;
    size_t n_params;
} NostrNip46Request;

typedef struct {
    char *id;
    char *result; /* may be JSON-stringified */
    char *error;  /* optional */
} NostrNip46Response;

/* Opaque session */
typedef struct NostrNip46Session NostrNip46Session;

/* One transport policy is owned by each peer session. NIP-44 v2 is the safe
 * default. The NIP-04 modes must be selected explicitly after negotiation:
 * legacy is original unauthenticated CBC; AEAD v2 is a nostrc extension and
 * must not be inferred from generic NIP-04 support. */
typedef enum {
    NOSTR_NIP46_TRANSPORT_NIP44_V2 = 1,
    NOSTR_NIP46_TRANSPORT_NIP04_LEGACY = 2,
    NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION = 3
} NostrNip46TransportMode;

/* Common lifecycle.
 *
 * nostr_nip46_session_free() is the OWNER teardown: it stops transport,
 * flushes/joins the async RPC workers, drains in-flight callers, then drops
 * the owner reference. Memory is released once the last reference is gone.
 *
 * nostrc-13gf: Short-lived borrowers (e.g. worker threads that snapshot the
 * session pointer from shared state) should take a reference for the
 * duration of their use so teardown can never free memory out from under
 * them:
 *   NostrNip46Session *ref = nostr_nip46_session_ref(shared_ptr);
 *   ... use ref ...
 *   nostr_nip46_session_unref(ref);
 */
void nostr_nip46_session_free(NostrNip46Session *s);
NostrNip46Session *nostr_nip46_session_ref(NostrNip46Session *s);
void nostr_nip46_session_unref(NostrNip46Session *s);

/* Introspection for tests and simple clients (returned strings/arrays are malloc'd) */
int  nostr_nip46_session_get_remote_pubkey(const NostrNip46Session *s, char **out_hex);
int  nostr_nip46_session_get_client_pubkey(const NostrNip46Session *s, char **out_hex);
int  nostr_nip46_session_get_secret(const NostrNip46Session *s, char **out_secret);
int  nostr_nip46_session_get_relays(const NostrNip46Session *s, char ***out_relays, size_t *out_n);
int  nostr_nip46_session_set_relays(NostrNip46Session *s, const char *const *relays, size_t n_relays);
int  nostr_nip46_session_take_last_reply_json(NostrNip46Session *s, char **out_json);

/* Configure/query the negotiated transport for this peer session. The setter
 * rejects unknown values. Call it before starting transport or processing
 * peer ciphertext. */
int nostr_nip46_session_set_transport_mode(
    NostrNip46Session *s, NostrNip46TransportMode mode);
NostrNip46TransportMode nostr_nip46_session_get_transport_mode(
    const NostrNip46Session *s);
const char *nostr_nip46_transport_mode_name(NostrNip46TransportMode mode);

/* Unified local transport crypto. Both persistent RPC and bunker request/reply
 * paths use these helpers, so request and response always share the session's
 * configured mode. Returned strings are malloc-allocated. */
int nostr_nip46_transport_encrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *plaintext, char **out_ciphertext);
int nostr_nip46_transport_decrypt(
    NostrNip46Session *s, const char *peer_pubkey_hex,
    const char *ciphertext, char **out_plaintext);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_TYPES_H */
