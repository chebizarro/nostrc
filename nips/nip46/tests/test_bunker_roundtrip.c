#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include "nostr-keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int string_eq(const char *a, const char *b){ return a && b && strcmp(a,b)==0; }

int main(void){
    /* Use well-known test key: sk=1, pk_compressed=0279BE66...; pk_xonly=79be66... */
    const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *client_pk_sec1 = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";
    /* Bunker uses same keypair for simplicity */
    const char *bunker_sk = client_sk;
    const char *bunker_pk_sec1 = client_pk_sec1;

    /* Client session: connect to bunker URI with remote pub and our secret */
    char uri_client[256];
    snprintf(uri_client, sizeof(uri_client), "bunker://%s?secret=%s", bunker_pk_sec1, client_sk);
    NostrNip46Session *cli = nostr_nip46_client_new(); if(!cli){ printf("cli new fail\n"); return 1; }
    if (nostr_nip46_client_connect(cli, uri_client, NULL) != 0){ printf("cli connect fail\n"); nostr_nip46_session_free(cli); return 2; }

    /* Bunker session: set its own secret via the same parser */
    char uri_bunker[256];
    snprintf(uri_bunker, sizeof(uri_bunker), "bunker://%s?secret=%s", client_pk_sec1, bunker_sk);
    NostrNip46Session *bun = nostr_nip46_bunker_new(NULL); if(!bun){ printf("bun new fail\n"); nostr_nip46_session_free(cli); return 3; }
    if (nostr_nip46_client_connect(bun, uri_bunker, NULL) != 0){ printf("bun set secret fail\n"); nostr_nip46_session_free(bun); nostr_nip46_session_free(cli); return 4; }

    /* Build a get_public_key request */
    char *req_json = nostr_nip46_request_build("1", "get_public_key", NULL, 0);
    if (!req_json){ printf("req build fail\n"); nostr_nip46_session_free(bun); nostr_nip46_session_free(cli); return 5; }

    /* Encrypt request to bunker using client */
    char *cipher_req = NULL; if (nostr_nip46_client_nip04_encrypt(cli, bunker_pk_sec1, req_json, &cipher_req) != 0 || !cipher_req){
        printf("client encrypt fail\n"); free(req_json); nostr_nip46_session_free(bun); nostr_nip46_session_free(cli); return 6;
    }
    free(req_json);

    /* Bunker handles and encrypts reply */
    char *cipher_reply = NULL; if (nostr_nip46_bunker_handle_cipher(bun, client_pk_sec1, cipher_req, &cipher_reply) != 0 || !cipher_reply){
        printf("bunker handle fail\n"); free(cipher_req); nostr_nip46_session_free(bun); nostr_nip46_session_free(cli); return 7;
    }
    free(cipher_req);

    /* Client decrypts reply */
    char *plain_reply = NULL; if (nostr_nip46_client_nip04_decrypt(cli, bunker_pk_sec1, cipher_reply, &plain_reply) != 0 || !plain_reply){
        printf("client decrypt fail\n"); free(cipher_reply); nostr_nip46_session_free(bun); nostr_nip46_session_free(cli); return 8;
    }
    free(cipher_reply);

    /* Parse reply and check it matches bunker's x-only public key */
    NostrNip46Response resp = {0}; if (nostr_nip46_response_parse(plain_reply, &resp) != 0){
        printf("resp parse fail: %s\n", plain_reply); free(plain_reply); nostr_nip46_session_free(bun); nostr_nip46_session_free(cli); return 9;
    }
    free(plain_reply);

    int rc = 0;
    if (!resp.id || !string_eq(resp.id, "1")) { printf("id mismatch\n"); rc = 10; }
    if (resp.error) { printf("unexpected error: %s\n", resp.error); rc = 11; }
    char *expect_bunker_pk_x = nostr_key_get_public(bunker_sk);
    if (!resp.result || !expect_bunker_pk_x || !string_eq(resp.result, expect_bunker_pk_x)) {
        printf("result mismatch: got='%s' expect='%s'\n", resp.result?resp.result:"(null)", expect_bunker_pk_x?expect_bunker_pk_x:"(null)");
        rc = 12;
    }

    free(expect_bunker_pk_x);
    nostr_nip46_response_free(&resp);
    nostr_nip46_session_free(bun);
    nostr_nip46_session_free(cli);

    if (rc==0) printf("test_bunker_roundtrip: OK\n");
    return rc;
}
