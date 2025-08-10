#include "nostr/nip46/nip46_uri.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int test_bunker_basic(void) {
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?relay=wss%3A%2F%2Frelay.example.com&secret=s3cr3t";
    NostrNip46BunkerURI u; memset(&u, 0, sizeof(u));
    if (nostr_nip46_uri_parse_bunker(uri, &u) != 0) return 1;
    int rc = 0;
    if (!u.remote_signer_pubkey_hex || strlen(u.remote_signer_pubkey_hex) != 64) rc = 2;
    if (u.n_relays != 1) rc = 3;
    if (!u.relays || strcmp(u.relays[0], "wss://relay.example.com") != 0) rc = 4;
    if (!u.secret || strcmp(u.secret, "s3cr3t") != 0) rc = 5;
    nostr_nip46_uri_bunker_free(&u);
    return rc;
}

static int test_connect_multi(void) {
    const char *uri = "nostrconnect://abcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcdefabcd?relay=wss%3A%2F%2Fr1&relay=wss%3A%2F%2Fr2&perms=sign_event%3A1%2Cnip44_encrypt&name=App&url=https%3A%2F%2Fapp.example&image=https%3A%2F%2Fimg";
    NostrNip46ConnectURI u; memset(&u, 0, sizeof(u));
    if (nostr_nip46_uri_parse_connect(uri, &u) != 0) return 1;
    int rc = 0;
    if (!u.client_pubkey_hex || strlen(u.client_pubkey_hex) != 64) rc = 2;
    if (u.n_relays != 2) rc = 3;
    if (!u.relays || strcmp(u.relays[0], "wss://r1") != 0 || strcmp(u.relays[1], "wss://r2") != 0) rc = 4;
    if (!u.perms_csv || strcmp(u.perms_csv, "sign_event:1,nip44_encrypt") != 0) rc = 5;
    if (!u.name || strcmp(u.name, "App") != 0) rc = 6;
    if (!u.url || strcmp(u.url, "https://app.example") != 0) rc = 7;
    if (!u.image || strcmp(u.image, "https://img") != 0) rc = 8;
    nostr_nip46_uri_connect_free(&u);
    return rc;
}

int main(void) {
    int rc = 0;
    rc |= test_bunker_basic();
    rc |= test_connect_multi();
    if (rc == 0) {
        printf("test_nip46_uri: OK\n");
        return 0;
    }
    printf("test_nip46_uri: FAIL rc=%d\n", rc);
    return 1;
}
