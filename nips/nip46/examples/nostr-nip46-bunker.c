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

/* Minimal bunker-only demo: issue a bunker URI and handle a connect + sign_event request locally */

static void ensure_json(void)
{
    nostr_json_init();
}

int main(void)
{
    ensure_json();

    /* Bunker has its own secret; here we reuse the same demo key */
    const char *bunker_sk = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *client_pk_sec1 = "0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798";

    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    if (!bunker) { fprintf(stderr, "bunker new failed\n"); return 1; }

    /* Pretend we issued a bunker:// for the client with our secret */
    char bunker_uri[256]; snprintf(bunker_uri, sizeof(bunker_uri), "bunker://%s?secret=%s", client_pk_sec1, bunker_sk);
    if (nostr_nip46_client_connect(bunker, bunker_uri, NULL) != 0) { fprintf(stderr, "bunker config fail\n"); nostr_nip46_session_free(bunker); return 1; }

    /* Receive a connect request granting sign_event */
    const char *cparams[2] = { client_pk_sec1, "sign_event" };
    char *connect_req = nostr_nip46_request_build("c1", "connect", cparams, 2);

    /* In a real server this arrives encrypted with NIP-04; emulate full round-trip using client helpers */
    NostrNip46Session *client = nostr_nip46_client_new();
    if (!client) { fprintf(stderr, "client new fail\n"); free(connect_req); nostr_nip46_session_free(bunker); return 1; }
    if (nostr_nip46_client_connect(client, bunker_uri, NULL) != 0) { fprintf(stderr, "client cfg fail\n"); nostr_nip46_session_free(client); free(connect_req); nostr_nip46_session_free(bunker); return 1; }

    char *cipher=NULL; if (nostr_nip46_client_nip04_encrypt(client, client_pk_sec1, connect_req, &cipher)!=0||!cipher){ fprintf(stderr, "connect encrypt fail\n"); free(connect_req); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(connect_req);

    char *cipher_reply=NULL; if (nostr_nip46_bunker_handle_cipher(bunker, client_pk_sec1, cipher, &cipher_reply)!=0||!cipher_reply){ fprintf(stderr, "bunker handle fail\n"); free(cipher); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(cipher);

    char *plain=NULL; if (nostr_nip46_client_nip04_decrypt(client, client_pk_sec1, cipher_reply, &plain)!=0||!plain){ fprintf(stderr, "connect decrypt fail\n"); free(cipher_reply); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(cipher_reply);

    NostrNip46Response cresp={0}; if(nostr_nip46_response_parse(plain,&cresp)!=0){ fprintf(stderr, "connect resp parse: %s\n", plain); free(plain); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(plain);
    if (cresp.error) { fprintf(stderr, "connect error: %s\n", cresp.error); nostr_nip46_response_free(&cresp); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    printf("connect ok: %s\n", cresp.result?cresp.result:"(null)");
    nostr_nip46_response_free(&cresp);

    /* Now handle a sign_event request end-to-end */
    NostrEvent *ev = nostr_event_new(); ev->kind=1; ev->created_at=456; ev->content=strdup("bunker signing demo");
    char *ev_json = nostr_event_serialize(ev); nostr_event_free(ev);
    const char *sparams[1] = { ev_json };
    char *sign_req = nostr_nip46_request_build("s1", "sign_event", sparams, 1);
    free(ev_json);

    char *scipher=NULL; if (nostr_nip46_client_nip04_encrypt(client, client_pk_sec1, sign_req, &scipher)!=0||!scipher){ fprintf(stderr, "sign encrypt fail\n"); free(sign_req); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(sign_req);

    char *sreply=NULL; if (nostr_nip46_bunker_handle_cipher(bunker, client_pk_sec1, scipher, &sreply)!=0||!sreply){ fprintf(stderr, "sign bunker handle fail\n"); free(scipher); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(scipher);

    char *splain=NULL; if (nostr_nip46_client_nip04_decrypt(client, client_pk_sec1, sreply, &splain)!=0||!splain){ fprintf(stderr, "sign decrypt fail\n"); free(sreply); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(sreply);

    NostrNip46Response sresp={0}; if(nostr_nip46_response_parse(splain,&sresp)!=0){ fprintf(stderr, "sign resp parse: %s\n", splain); free(splain); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    free(splain);
    if (sresp.error) { fprintf(stderr, "sign error: %s\n", sresp.error); nostr_nip46_response_free(&sresp); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }

    NostrEvent *sev = nostr_event_new();
    if (nostr_event_deserialize(sev, sresp.result)!=0) { fprintf(stderr, "signed event parse fail\n"); nostr_event_free(sev); nostr_nip46_response_free(&sresp); nostr_nip46_session_free(client); nostr_nip46_session_free(bunker); return 1; }
    if (!nostr_event_check_signature(sev)) { fprintf(stderr, "signature verify failed\n"); }
    else { printf("bunker signed event ok (id=%s)\n", sev->id?sev->id:"(nil)"); }
    nostr_event_free(sev);
    nostr_nip46_response_free(&sresp);

    printf("done.\n");

    nostr_nip46_session_free(client);
    nostr_nip46_session_free(bunker);
    return 0;
}
