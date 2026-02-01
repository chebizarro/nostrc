#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *fake_sign(const char *event_json, void *ud){
    (void)ud;
    /* Return a simple wrapped object to prove pass-through */
    size_t n = strlen(event_json);
    size_t out_size = n + 20;
    char *out = (char*)malloc(out_size);
    if (!out) return NULL;
    snprintf(out, out_size, "{\"signed\":%s}", event_json);
    return out;
}

int main(void){
    const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *client_pk_sec1 = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";
    const char *bunker_sk = client_sk;
    const char *bunker_pk_sec1 = client_pk_sec1;

    NostrNip46BunkerCallbacks cbs = {0};
    cbs.sign_cb = fake_sign;

    NostrNip46Session *bun = nostr_nip46_bunker_new(&cbs); if(!bun){ printf("bun new fail\n"); return 1; }
    char uri_bunker[256]; snprintf(uri_bunker, sizeof(uri_bunker), "bunker://%s?secret=%s", client_pk_sec1, bunker_sk);
    if (nostr_nip46_client_connect(bun, uri_bunker, NULL) != 0){ printf("bun set secret fail\n"); nostr_nip46_session_free(bun); return 2; }

    NostrNip46Session *cli = nostr_nip46_client_new(); if(!cli){ printf("cli new fail\n"); nostr_nip46_session_free(bun); return 3; }
    char uri_client[256]; snprintf(uri_client, sizeof(uri_client), "bunker://%s?secret=%s", bunker_pk_sec1, client_sk);
    if (nostr_nip46_client_connect(cli, uri_client, NULL) != 0){ printf("cli connect fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 4; }

    /* Connect with permission to sign_event */
    {
        const char *cparams[2] = { client_pk_sec1, "sign_event" };
        char *creq = nostr_nip46_request_build("c1", "connect", cparams, 2);
        if (!creq){ printf("connect req build fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 5; }
        char *ccipher=NULL; if (nostr_nip46_client_nip04_encrypt(cli, bunker_pk_sec1, creq, &ccipher) != 0 || !ccipher){ printf("connect encrypt fail\n"); free(creq); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 6; }
        free(creq);
        char *ccipher_reply=NULL; if (nostr_nip46_bunker_handle_cipher(bun, client_pk_sec1, ccipher, &ccipher_reply) != 0 || !ccipher_reply){ printf("connect bunker handle fail\n"); free(ccipher); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 7; }
        free(ccipher);
        char *cplain=NULL; if (nostr_nip46_client_nip04_decrypt(cli, bunker_pk_sec1, ccipher_reply, &cplain) != 0 || !cplain){ printf("connect decrypt fail\n"); free(ccipher_reply); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 8; }
        free(ccipher_reply);
        NostrNip46Response cresp={0}; if (nostr_nip46_response_parse(cplain, &cresp) != 0){ printf("connect resp parse fail: %s\n", cplain); free(cplain); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 9; }
        free(cplain);
        if (cresp.error){ printf("connect returned error: %s\n", cresp.error); nostr_nip46_response_free(&cresp); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 9; }
        if (!cresp.result){ printf("connect missing result\n"); nostr_nip46_response_free(&cresp); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 9; }
        nostr_nip46_response_free(&cresp);
    }

    const char *event_json = "{\"kind\":1,\"content\":\"hi\"}";
    const char *params[1] = { event_json };
    char *req_json = nostr_nip46_request_build("9", "sign_event", params, 1);
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
    if (!resp.id || strcmp(resp.id, "9")!=0) { printf("id mismatch\n"); rc=10; }
    if (resp.error) { printf("unexpected error: %s\n", resp.error); rc=11; }
    if (!resp.result || strstr(resp.result, "\"signed\":") == NULL) { printf("signed wrapper missing: got '%s'\n", resp.result?resp.result:"(null)"); rc=12; }

    nostr_nip46_response_free(&resp);
    nostr_nip46_session_free(cli);
    nostr_nip46_session_free(bun);
    if (rc==0) printf("test_bunker_sign_event_cb: OK\n");
    return rc;
}
