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
/* Decrypt a NIP-04-wrapped request, dispatch, and return an encrypted response */
int nostr_nip46_bunker_handle_cipher(NostrNip46Session *s,
                                     const char *client_pubkey_hex,
                                     const char *ciphertext,
                                     char **out_cipher_reply);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_BUNKER_H */
