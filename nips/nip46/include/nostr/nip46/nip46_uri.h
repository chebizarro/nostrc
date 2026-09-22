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
    char *secret; /* optional connect authorization token, never a private key */
} NostrNip46BunkerURI;

/* nostrconnect://<client-pubkey>?relay=...&secret=...&perms=...&name=...&url=...&image=... */
typedef struct {
    char *client_pubkey_hex; /* required */
    char **relays; size_t n_relays; /* optional */
    char *secret; /* optional connect authorization token, never a private key */
    char *perms_csv; /* optional */
    char *name; char *url; char *image; /* optional */
} NostrNip46ConnectURI;

int nostr_nip46_uri_parse_bunker(const char *uri, NostrNip46BunkerURI *out);
int nostr_nip46_uri_parse_connect(const char *uri, NostrNip46ConnectURI *out);

/* nostrc-z1fb Phase 1: build a nostrconnect:// URI from a caller-populated
 * NostrNip46ConnectURI. Percent-encodes every value (relay URLs, secret,
 * perms, name, url, image); the client_pubkey_hex is emitted verbatim.
 *
 * Requires: in->client_pubkey_hex non-NULL and hex-valid. Any relay whose
 * pointer is NULL is skipped. The caller owns *out_uri (free() when done).
 * The URI is NEVER logged automatically because it embeds the connect
 * secret; treat it as sensitive at every callsite.
 * Returns 0 on success, -1 on error. */
int nostr_nip46_uri_build_connect(const NostrNip46ConnectURI *in, char **out_uri);

void nostr_nip46_uri_bunker_free(NostrNip46BunkerURI *u);
void nostr_nip46_uri_connect_free(NostrNip46ConnectURI *u);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_NIP46_URI_H */
