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

/* Common lifecycle */
void nostr_nip46_session_free(NostrNip46Session *s);

/* Introspection for tests and simple clients (returned strings/arrays are malloc'd) */
int  nostr_nip46_session_get_remote_pubkey(const NostrNip46Session *s, char **out_hex);
int  nostr_nip46_session_get_client_pubkey(const NostrNip46Session *s, char **out_hex);
int  nostr_nip46_session_get_secret(const NostrNip46Session *s, char **out_secret);
int  nostr_nip46_session_get_relays(const NostrNip46Session *s, char ***out_relays, size_t *out_n);
int  nostr_nip46_session_take_last_reply_json(NostrNip46Session *s, char **out_json);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_TYPES_H */
