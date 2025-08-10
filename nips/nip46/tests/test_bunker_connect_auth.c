#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int string_eq(const char *a, const char *b){ return a && b && strcmp(a,b)==0; }

static int allow_all(const char *client_pubkey_hex, const char *perms_csv, void *ud){ (void)client_pubkey_hex; (void)perms_csv; (void)ud; return 1; }
static int deny_all(const char *client_pubkey_hex, const char *perms_csv, void *ud){ (void)client_pubkey_hex; (void)perms_csv; (void)ud; return 0; }

static int run_once(int allow){
    const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *client_pk_sec1 = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";
    const char *bunker_sk = client_sk;
    const char *bunker_pk_sec1 = client_pk_sec1;

    NostrNip46BunkerCallbacks cbs = {0};
    cbs.authorize_cb = allow ? allow_all : deny_all;

    NostrNip46Session *bun = nostr_nip46_bunker_new(&cbs); if(!bun){ printf("bun new fail\n"); return 1; }
    char uri_bunker[256]; snprintf(uri_bunker, sizeof(uri_bunker), "bunker://%s?secret=%s", client_pk_sec1, bunker_sk);
    if (nostr_nip46_client_connect(bun, uri_bunker, NULL) != 0){ printf("bun set secret fail\n"); nostr_nip46_session_free(bun); return 2; }

    NostrNip46Session *cli = nostr_nip46_client_new(); if(!cli){ printf("cli new fail\n"); nostr_nip46_session_free(bun); return 3; }
    char uri_client[256]; snprintf(uri_client, sizeof(uri_client), "bunker://%s?secret=%s", bunker_pk_sec1, client_sk);
    if (nostr_nip46_client_connect(cli, uri_client, NULL) != 0){ printf("cli connect fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 4; }

    /* Build connect request params: [client_pubkey, perms] */
    const char *params[2] = { client_pk_sec1, "sign_event" };
    char *req_json = nostr_nip46_request_build("7", "connect", params, 2);
    if (!req_json){ printf("req build fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 5; }

    char *cipher_req=NULL; if (nostr_nip46_client_nip04_encrypt(cli, bunker_pk_sec1, req_json, &cipher_req) != 0 || !cipher_req){
        printf("encrypt fail\n"); free(req_json); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 6;
    }
    free(req_json);

    char *cipher_reply=NULL; if (nostr_nip46_bunker_handle_cipher(bun, client_pk_sec1, cipher_req, &cipher_reply) != 0 || !cipher_reply){
        printf("bunker handle fail\n"); free(cipher_req); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 7;
    }
    free(cipher_req);

    char *plain=NULL; if (nostr_nip46_client_nip04_decrypt(cli, bunker_pk_sec1, cipher_reply, &plain) != 0 || !plain){
        printf("decrypt fail\n"); free(cipher_reply); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 8;
    }
    free(cipher_reply);

    NostrNip46Response resp={0}; if (nostr_nip46_response_parse(plain, &resp) != 0){ printf("resp parse fail: %s\n", plain); free(plain); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 9; }
    free(plain);

    int rc=0;
    if (!resp.id || !string_eq(resp.id, "7")) { printf("id mismatch\n"); rc=10; }
    if (allow) {
        if (resp.error) { printf("unexpected error: %s\n", resp.error); rc=11; }
        if (!resp.result || strcmp(resp.result, "ack")!=0) { printf("expected ack, got '%s'\n", resp.result?resp.result:"(null)"); rc=12; }
    } else {
        if (!resp.error || strcmp(resp.error, "denied")!=0) { printf("expected denied error\n"); rc=13; }
    }

    nostr_nip46_response_free(&resp);
    nostr_nip46_session_free(cli);
    nostr_nip46_session_free(bun);
    return rc;
}

int main(void){
    int rc = run_once(1); if (rc){ printf("connect allow failed rc=%d\n", rc); return rc; }
    rc = run_once(0); if (rc){ printf("connect deny failed rc=%d\n", rc); return rc; }
    printf("test_bunker_connect_auth: OK\n");
    return 0;
}
