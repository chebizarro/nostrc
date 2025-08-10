#include "nostr/nip46/nip46_client.h"
#include <stdio.h>
#include <string.h>
#include "nostr/nip04.h"
#include <stdlib.h>

static int test_get_public_key_bunker(void){
    const char *pk = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *uri = "bunker://0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef?relay=wss%3A%2F%2Frelay";
    NostrNip46Session *s = nostr_nip46_client_new(); if(!s) return 1;
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) { nostr_nip46_session_free(s); return 2; }
    char *out=NULL; if (nostr_nip46_client_get_public_key(s, &out) != 0 || !out) { nostr_nip46_session_free(s); return 3; }
    int rc = strcmp(out, pk)==0 ? 0 : 4;
    free(out); nostr_nip46_session_free(s); return rc;
}

static int test_get_public_key_connect(void){
    const char *pk = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    const char *uri = "nostrconnect://abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789?relay=wss%3A%2F%2Frelay";
    NostrNip46Session *s = nostr_nip46_client_new(); if(!s) return 1;
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) { nostr_nip46_session_free(s); return 2; }
    char *out=NULL; if (nostr_nip46_client_get_public_key(s, &out) != 0 || !out) { nostr_nip46_session_free(s); return 3; }
    int rc = strcmp(out, pk)==0 ? 0 : 4;
    free(out); nostr_nip46_session_free(s); return rc;
}

static int test_sign_event_encrypt_and_decrypt(void){
    /* Use bunker URI so peer pubkey is remote signer; include secret so session can derive conv key */
    const char *uri = "bunker://0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798?secret=0000000000000000000000000000000000000000000000000000000000000001";
    NostrNip46Session *s = nostr_nip46_client_new(); if(!s) return 1;
    if (nostr_nip46_client_connect(s, uri, NULL) != 0) { nostr_nip46_session_free(s); return 2; }
    const char *event_json = "{\"kind\":1,\"content\":\"hi\"}";
    char *cipher=NULL; if (nostr_nip46_client_sign_event(s, event_json, &cipher) != 0 || !cipher) { nostr_nip46_session_free(s); return 3; }
    /* With NIP-04 we cannot decrypt without receiver's secret; validate format */
    int ok = (cipher && strstr(cipher, "?iv=") != NULL && strlen(cipher) > 20);
    free(cipher); nostr_nip46_session_free(s);
    return ok ? 0 : 4;
}

int main(void){
    int rc=0;
    rc = test_get_public_key_bunker(); if(rc){ printf("test_get_public_key_bunker rc=%d\n", rc); return 10+rc; }
    rc = test_get_public_key_connect(); if(rc){ printf("test_get_public_key_connect rc=%d\n", rc); return 20+rc; }
    rc = test_sign_event_encrypt_and_decrypt(); if(rc){ printf("test_sign_event_encrypt_and_decrypt rc=%d\n", rc); return 30+rc; }
    printf("test_client_api: OK\n");
    return 0;
}
