#include "nostr/nip46/nip46_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_bunker_multi_relays(void) {
    const char *pk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?"
                      "relay=wss%3A%2F%2Frelay.one&relay=wss%3A%2F%2Frelay.two%2Fpath&secret=a%20b";
    NostrNip46Session *s = nostr_nip46_client_new(); if (!s) return 1;
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) { nostr_nip46_session_free(s); return 2; }
    char *out_pk = NULL; if (nostr_nip46_session_get_remote_pubkey(s, &out_pk) != 0 || !out_pk) { nostr_nip46_session_free(s); return 3; }
    int rc = 0;
    if (strcmp(out_pk, pk) != 0) { rc = 4; }
    free(out_pk);
    char **relays=NULL; size_t n=0; if (nostr_nip46_session_get_relays(s, &relays, &n) != 0) { nostr_nip46_session_free(s); return 5; }
    if (n < 2) rc = 6;
    if (n >= 1 && strcmp(relays[0], "wss://relay.one") != 0) rc = 7;
    if (n >= 2 && strcmp(relays[1], "wss://relay.two/path") != 0) rc = 8;
    for (size_t i=0;i<n;++i) free(relays[i]); free(relays);
    char *sec=NULL; (void)nostr_nip46_session_get_secret(s, &sec);
    if (!sec || strcmp(sec, "a b") != 0) rc = 9;
    free(sec);
    nostr_nip46_session_free(s);
    if (rc) { printf("test_bunker_multi_relays rc=%d\n", rc); }
    return rc;
}

static int test_invalid_scheme(void) {
    const char *uri = "invalidscheme://deadbeef?relay=wss%3A%2F%2Frelay";
    NostrNip46Session *s = nostr_nip46_client_new(); if (!s) return 1;
    int ok = nostr_nip46_client_connect(s, uri, NULL);
    nostr_nip46_session_free(s);
    if (ok == 0) { printf("invalid scheme accepted\n"); return 2; }
    return 0;
}

static int test_bunker_bad_key(void) {
    const char *uri = "bunker://abcd?relay=wss%3A%2F%2Frelay"; /* too short */
    NostrNip46Session *s = nostr_nip46_client_new(); if (!s) return 1;
    int ok = nostr_nip46_client_connect(s, uri, NULL);
    nostr_nip46_session_free(s);
    if (ok == 0) { printf("bad key accepted\n"); return 2; }
    return 0;
}

int main(void) {
    int rc = 0;
    rc = test_bunker_multi_relays(); if (rc) return 10 + rc;
    rc = test_invalid_scheme(); if (rc) return 20 + rc;
    rc = test_bunker_bad_key(); if (rc) return 30 + rc;
    printf("test_client_connect_edge: OK\n");
    return 0;
}
