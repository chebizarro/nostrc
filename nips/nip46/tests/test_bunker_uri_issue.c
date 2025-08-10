#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_uri.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    const char *rs_pub = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *relays[] = {"wss://relay.one", "wss://relay.two/path?x=y"};
    const char *secret = "top secret value";
    char *uri = NULL;

    NostrNip46Session *s = nostr_nip46_bunker_new(NULL);
    if (!s) { printf("session_new failed\n"); return 1; }
    if (nostr_nip46_bunker_issue_bunker_uri(s, rs_pub, relays, 2, secret, &uri) != 0 || !uri) {
        printf("issue_bunker_uri failed\n");
        nostr_nip46_session_free(s);
        return 2;
    }

    NostrNip46BunkerURI parsed;
    if (nostr_nip46_uri_parse_bunker(uri, &parsed) != 0) {
        printf("parse_bunker failed\n");
        free(uri);
        nostr_nip46_session_free(s);
        return 3;
    }

    int rc = 0;
    if (!parsed.remote_signer_pubkey_hex || strcmp(parsed.remote_signer_pubkey_hex, rs_pub) != 0) {
        printf("pubkey mismatch\n"); rc = 4;
    }
    if (parsed.n_relays < 2) { printf("relays size mismatch\n"); rc = 5; }
    if (!parsed.secret || strcmp(parsed.secret, secret) != 0) { printf("secret mismatch\n"); rc = 6; }

    nostr_nip46_uri_bunker_free(&parsed);
    free(uri);
    nostr_nip46_session_free(s);

    if (rc) return rc;
    printf("test_bunker_uri_issue: OK\n");
    return 0;
}
