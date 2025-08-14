#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include "nostr-event.h"
#include "json.h"
#include "nostr-keys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int string_eq(const char *a, const char *b){ return a && b && strcmp(a,b)==0; }

int main(void){
    /* Ensure JSON provider is initialized */
    nostr_json_init();

    const char *client_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *client_pk_sec1 = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";
    const char *bunker_sk = client_sk;
    const char *bunker_pk_sec1 = client_pk_sec1;

    NostrNip46Session *bun = nostr_nip46_bunker_new(NULL); if(!bun){ printf("bun new fail\n"); return 1; }
    char uri_bunker[256]; snprintf(uri_bunker, sizeof(uri_bunker), "bunker://%s?secret=%s", client_pk_sec1, bunker_sk);
    if (nostr_nip46_client_connect(bun, uri_bunker, NULL) != 0){ printf("bun set secret fail\n"); nostr_nip46_session_free(bun); return 2; }

    NostrNip46Session *cli = nostr_nip46_client_new(); if(!cli){ printf("cli new fail\n"); nostr_nip46_session_free(bun); return 3; }
    char uri_client[256]; snprintf(uri_client, sizeof(uri_client), "bunker://%s?secret=%s", bunker_pk_sec1, client_sk);
    if (nostr_nip46_client_connect(cli, uri_client, NULL) != 0){ printf("cli connect fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 4; }

    /* First, connect with permission to sign_event */
    {
        const char *cparams[2] = { client_pk_sec1, "sign_event" };
        char *creq = nostr_nip46_request_build("c2", "connect", cparams, 2);
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

    /* Build an unsigned event and request signing */
    NostrEvent *ev = nostr_event_new();
    ev->kind = 1; ev->created_at = 123; ev->content = strdup("hello");
    char *ev_json = nostr_event_serialize(ev);
    nostr_event_free(ev);
    if (!ev_json){ printf("serialize unsigned event fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 5; }

    const char *params[1] = { ev_json };
    char *req_json = nostr_nip46_request_build("11", "sign_event", params, 1);
    free(ev_json);
    if (!req_json){ printf("req build fail\n"); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 6; }

    char *cipher_req=NULL; if (nostr_nip46_client_nip04_encrypt(cli, bunker_pk_sec1, req_json, &cipher_req) != 0 || !cipher_req){
        printf("encrypt fail\n"); free(req_json); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 7;
    }
    free(req_json);

    char *cipher_reply=NULL; if (nostr_nip46_bunker_handle_cipher(bun, client_pk_sec1, cipher_req, &cipher_reply) != 0 || !cipher_reply){
        printf("bunker handle fail\n"); free(cipher_req); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 8;
    }
    free(cipher_req);

    char *plain=NULL; if (nostr_nip46_client_nip04_decrypt(cli, bunker_pk_sec1, cipher_reply, &plain) != 0 || !plain){
        printf("decrypt fail\n"); free(cipher_reply); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 9;
    }
    free(cipher_reply);

    NostrNip46Response resp={0}; if (nostr_nip46_response_parse(plain, &resp) != 0){ printf("resp parse fail: %s\n", plain); free(plain); nostr_nip46_session_free(cli); nostr_nip46_session_free(bun); return 10; }
    free(plain);

    int rc=0;
    if (!resp.id || !string_eq(resp.id, "11")) { printf("id mismatch\n"); rc=11; }
    if (resp.error) { printf("unexpected error: %s\n", resp.error); rc=12; }
    if (!resp.result) { printf("missing signed event JSON\n"); rc=13; }

    /* Validate returned event: pubkey matches bunker and signature verifies */
    NostrEvent *sev = nostr_event_new();
    if (!sev) { rc=14; }
    if (rc==0 && nostr_event_deserialize(sev, resp.result) != 0) { printf("signed event deserialize fail: %s\n", resp.result); rc=15; }

    char *expect_bunker_pk_x = nostr_key_get_public(bunker_sk);
    if (rc==0 && (!sev->pubkey || !expect_bunker_pk_x || strcmp(sev->pubkey, expect_bunker_pk_x)!=0)) {
        printf("pubkey mismatch: got='%s' expect='%s'\n", sev->pubkey?sev->pubkey:"(null)", expect_bunker_pk_x?expect_bunker_pk_x:"(null)");
        rc=16;
    }
    if (rc==0 && !nostr_event_check_signature(sev)) { printf("signature verify failed: %s\n", resp.result); rc=17; }

    free(expect_bunker_pk_x);
    nostr_event_free(sev);
    nostr_nip46_response_free(&resp);
    nostr_nip46_session_free(cli);
    nostr_nip46_session_free(bun);
    nostr_json_cleanup();

    if (rc==0) printf("test_bunker_sign_event_real: OK\n");
    return rc;
}
