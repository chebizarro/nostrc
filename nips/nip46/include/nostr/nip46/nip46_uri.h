#ifndef NOSTR_NIP46_URI_H
#define NOSTR_NIP46_URI_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* bunker://<rs-pubkey>?relay=...&secret=... */
typedef struct {
    char *remote_signer_pubkey_hex; /* required */
    char **relays; size_t n_relays; /* optional */
    char *secret; /* optional */
} NostrNip46BunkerURI;

/* nostrconnect://<client-pubkey>?relay=...&secret=...&perms=...&name=...&url=...&image=... */
typedef struct {
    char *client_pubkey_hex; /* required */
    char **relays; size_t n_relays; /* optional */
    char *secret; /* optional */
    char *perms_csv; /* optional */
    char *name; char *url; char *image; /* optional */
} NostrNip46ConnectURI;

int nostr_nip46_uri_parse_bunker(const char *uri, NostrNip46BunkerURI *out);
int nostr_nip46_uri_parse_connect(const char *uri, NostrNip46ConnectURI *out);

void nostr_nip46_uri_bunker_free(NostrNip46BunkerURI *u);
void nostr_nip46_uri_connect_free(NostrNip46ConnectURI *u);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_URI_H */
