#include "nostr/nip46/nip46_client.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_bunker_connect(void) {
    const char *rs_pub = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?relay=wss%3A%2F%2Frelay.one&secret=sec";
    NostrNip46Session *s = nostr_nip46_client_new();
    if (!s) { printf("session new fail\n"); return 1; }
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) { printf("connect fail\n"); nostr_nip46_session_free(s); return 2; }
    char *pub = NULL; if (nostr_nip46_session_get_remote_pubkey(s, &pub) != 0 || !pub) { printf("get remote pub fail\n"); nostr_nip46_session_free(s); return 3; }
    if (strcmp(pub, rs_pub) != 0) { printf("remote pub mismatch\n"); free(pub); nostr_nip46_session_free(s); return 4; }
    free(pub);
    char **relays = NULL; size_t n = 0; if (nostr_nip46_session_get_relays(s, &relays, &n) != 0) { printf("get relays fail\n"); nostr_nip46_session_free(s); return 5; }
    if (n < 1 || !relays[0] || strcmp(relays[0], "wss://relay.one") != 0) { printf("relay mismatch\n"); for (size_t i=0;i<n;++i) free(relays[i]); free(relays); nostr_nip46_session_free(s); return 6; }
    for (size_t i=0;i<n;++i) free(relays[i]); free(relays);
    char *sec = NULL; (void)nostr_nip46_session_get_secret(s, &sec); if (!sec || strcmp(sec, "sec") != 0) { printf("secret mismatch\n"); free(sec); nostr_nip46_session_free(s); return 7; }
    free(sec);
    nostr_nip46_session_free(s);
    return 0;
}

static int test_connect_connect(void) {
    const char *cli_pub = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789?relay=wss%3A%2F%2Frelay.two";
    NostrNip46Session *s = nostr_nip46_client_new(); if (!s) return 1;
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) { nostr_nip46_session_free(s); return 2; }
    char *pub = NULL; if (nostr_nip46_session_get_client_pubkey(s, &pub) != 0 || !pub) { nostr_nip46_session_free(s); return 3; }
    if (strcmp(pub, cli_pub) != 0) { free(pub); nostr_nip46_session_free(s); return 4; }
    free(pub);
    char **relays = NULL; size_t n = 0; (void)nostr_nip46_session_get_relays(s, &relays, &n);
    if (n < 1 || strcmp(relays[0], "wss://relay.two") != 0) { for (size_t i=0;i<n;++i) free(relays[i]); free(relays); nostr_nip46_session_free(s); return 5; }
    for (size_t i=0;i<n;++i) free(relays[i]); free(relays);
    nostr_nip46_session_free(s);
    return 0;
}

int main(void) {
    int rc = 0;
    rc = test_bunker_connect(); if (rc) { printf("test_bunker_connect fail %d\n", rc); return rc + 10; }
    rc = test_connect_connect(); if (rc) { printf("test_connect_connect fail %d\n", rc); return rc + 20; }
    printf("test_client_connect: OK\n");
    return 0;
}
