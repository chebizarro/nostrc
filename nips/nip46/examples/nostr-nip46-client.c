#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include "nostr-event.h"
#include "nostr-keys.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void ensure_json()
{
    nostr_json_init();
}

int main(void)
{
    ensure_json();

    /* Demo keys: use same key for client and bunker to keep it simple */
    const char *sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *pk_sec1 = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";

    /* Create bunker and client sessions */
    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    if (!bunker) { fprintf(stderr, "bunker new failed\n"); return 1; }
    NostrNip46Session *client = nostr_nip46_client_new();
    if (!client) { fprintf(stderr, "client new failed\n"); nostr_nip46_session_free(bunker); return 1; }

    /* Configure secrets via bunker:// URI (no network used in this example) */
    char bunker_uri[256]; snprintf(bunker_uri, sizeof(bunker_uri), "bunker://%s?secret=%s", pk_sec1, sk);
    if (nostr_nip46_client_connect(bunker, bunker_uri, NULL) != 0) { fprintf(stderr, "bunker connect fail\n"); goto done; }
    if (nostr_nip46_client_connect(client, bunker_uri, NULL) != 0) { fprintf(stderr, "client connect fail\n"); goto done; }

    /* 1) Connect with ACL: allow sign_event for this client */
    {
        const char *cparams[2] = { pk_sec1, "sign_event" };
        char *req = nostr_nip46_request_build("c1", "connect", cparams, 2);
        if (!req) { fprintf(stderr, "connect build fail\n"); goto done; }
        char *cipher=NULL; if (nostr_nip46_client_nip04_encrypt(client, pk_sec1, req, &cipher)!=0||!cipher){ fprintf(stderr, "connect encrypt fail\n"); free(req); goto done; }
        free(req);
        char *cipher_reply=NULL; if (nostr_nip46_bunker_handle_cipher(bunker, pk_sec1, cipher, &cipher_reply)!=0||!cipher_reply){ fprintf(stderr, "connect bunker handle fail\n"); free(cipher); goto done; }
        free(cipher);
        char *plain=NULL; if (nostr_nip46_client_nip04_decrypt(client, pk_sec1, cipher_reply, &plain)!=0||!plain){ fprintf(stderr, "connect decrypt fail\n"); free(cipher_reply); goto done; }
        free(cipher_reply);
        NostrNip46Response resp={0}; if(nostr_nip46_response_parse(plain,&resp)!=0){ fprintf(stderr, "connect resp parse: %s\n", plain); free(plain); goto done; }
        free(plain);
        if (resp.error) { fprintf(stderr, "connect error: %s\n", resp.error); nostr_nip46_response_free(&resp); goto done; }
        printf("connect ok: %s\n", resp.result?resp.result:"(null)");
        nostr_nip46_response_free(&resp);
    }

    /* 2) Build unsigned event, request signing, and verify */
    NostrEvent *ev = nostr_event_new();
    ev->kind = 1; ev->created_at = 123; ev->content = strdup("hello from example");
    char *ev_json = nostr_event_serialize(ev);
    nostr_event_free(ev);
    if (!ev_json) { fprintf(stderr, "unsigned serialize fail\n"); goto done; }

    const char *params[1] = { ev_json };
    char *sreq = nostr_nip46_request_build("11", "sign_event", params, 1);
    free(ev_json);
    if (!sreq) { fprintf(stderr, "sign build fail\n"); goto done; }

    char *scipher=NULL; if (nostr_nip46_client_nip04_encrypt(client, pk_sec1, sreq, &scipher)!=0||!scipher){ fprintf(stderr, "sign encrypt fail\n"); free(sreq); goto done; }
    free(sreq);

    char *sreply=NULL; if (nostr_nip46_bunker_handle_cipher(bunker, pk_sec1, scipher, &sreply)!=0||!sreply){ fprintf(stderr, "bunker handle fail\n"); free(scipher); goto done; }
    free(scipher);

    char *splain=NULL; if (nostr_nip46_client_nip04_decrypt(client, pk_sec1, sreply, &splain)!=0||!splain){ fprintf(stderr, "sign decrypt fail\n"); free(sreply); goto done; }
    free(sreply);

    NostrNip46Response sresp={0}; if (nostr_nip46_response_parse(splain, &sresp)!=0){ fprintf(stderr, "sign resp parse fail: %s\n", splain); free(splain); goto done; }
    free(splain);

    if (sresp.error) {
        fprintf(stderr, "sign error: %s\n", sresp.error);
        nostr_nip46_response_free(&sresp); goto done;
    }
    if (!sresp.result) { fprintf(stderr, "missing signed event\n"); nostr_nip46_response_free(&sresp); goto done; }

    NostrEvent *sev = nostr_event_new();
    if (nostr_event_deserialize(sev, sresp.result)!=0) { fprintf(stderr, "signed event deserialize fail\n"); nostr_event_free(sev); nostr_nip46_response_free(&sresp); goto done; }
    char *expect_pk = nostr_key_get_public(sk);
    if (!sev->pubkey || !expect_pk || strcmp(sev->pubkey, expect_pk)!=0) {
        fprintf(stderr, "pubkey mismatch: got=%s expect=%s\n", sev->pubkey?sev->pubkey:"(null)", expect_pk?expect_pk:"(null)");
    } else if (!nostr_event_check_signature(sev)) {
        fprintf(stderr, "signature verify failed\n");
    } else {
        printf("signed event ok (id=%s)\n", sev->id?sev->id:"(nil)");
    }
    free(expect_pk);
    nostr_event_free(sev);
    nostr_nip46_response_free(&sresp);

    printf("done.\n");

done:
    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
    return 0;
}
