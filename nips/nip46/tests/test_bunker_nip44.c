/* test_bunker_nip44.c — bead nostrc-pvha
 *
 * Exercises the NIP-46 bunker's default handler for nip44_encrypt and
 * nip44_decrypt over the shared handle_cipher path used by every
 * bunker (real relay or standin). Two round-trips:
 *
 *  1. peer == client — a client asks the bunker to nip44_encrypt against
 *     itself, then nip44_decrypt the ciphertext. Verifies the plaintext
 *     round-trips byte-for-byte.
 *  2. peer == "the account" (self-encrypt in porthome enrollment):
 *     because the bunker's key is the account key in this test, encrypt
 *     against the account pubkey is a self-encrypt. Confirms the seed
 *     hex (64 lowercase chars, wire shape used by
 *     nh_auth_broker_porthome_maybe_enroll_or_unwrap_nip46) survives.
 *
 * Deliberately avoids a live relay pool — hands the ciphertext to
 * nostr_nip46_bunker_handle_cipher directly, same as the sibling
 * test_bunker_connect_auth. */

#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int roundtrip(NostrNip46Session *cli, NostrNip46Session *bun,
                     const char *client_pk, const char *bunker_pk,
                     const char *peer_for_op, const char *plaintext) {
    /* 1. Client builds `nip44_encrypt` request, encrypts it under the
     *    NIP-46 transport (nip44 v2), hands it to the bunker's
     *    handler, decrypts the reply, and reads the ciphertext. */
    const char *enc_params[2] = { peer_for_op, plaintext };
    char *req_json = nostr_nip46_request_build("42", "nip44_encrypt",
                                               enc_params, 2);
    if (!req_json) { printf("build encrypt req failed\n"); return 1; }

    char *cipher_req = NULL;
    if (nostr_nip46_transport_encrypt(cli, bunker_pk, req_json,
                                      &cipher_req) != 0 || !cipher_req) {
        printf("client transport_encrypt failed\n"); free(req_json); return 2;
    }
    free(req_json);

    /* Bunker must accept the connect first so the ACL grants
     * nip44_encrypt. We authorise via handle_cipher's connect branch
     * (default perms include nip44_encrypt/_decrypt when perms is
     * empty). */

    char *cipher_reply = NULL;
    if (nostr_nip46_bunker_handle_cipher(bun, client_pk, cipher_req,
                                         &cipher_reply) != 0 || !cipher_reply) {
        printf("bunker handle_cipher(nip44_encrypt) failed\n");
        free(cipher_req); return 3;
    }
    free(cipher_req);

    char *plain_reply = NULL;
    if (nostr_nip46_transport_decrypt(cli, bunker_pk, cipher_reply,
                                      &plain_reply) != 0 || !plain_reply) {
        printf("client transport_decrypt (encrypt reply) failed\n");
        free(cipher_reply); return 4;
    }
    free(cipher_reply);

    NostrNip46Response resp = {0};
    if (nostr_nip46_response_parse(plain_reply, &resp) != 0 ||
        !resp.result || resp.error) {
        printf("encrypt reply parse failed / had error: '%s'\n",
               plain_reply);
        free(plain_reply); nostr_nip46_response_free(&resp); return 5;
    }
    char *ciphertext_result = strdup(resp.result);
    nostr_nip46_response_free(&resp);
    free(plain_reply);
    if (!ciphertext_result) return 6;

    /* 2. Client builds `nip44_decrypt` request over the same ciphertext
     *    and confirms plaintext round-trips. */
    const char *dec_params[2] = { peer_for_op, ciphertext_result };
    req_json = nostr_nip46_request_build("43", "nip44_decrypt",
                                         dec_params, 2);
    if (!req_json) { free(ciphertext_result); return 7; }

    cipher_req = NULL;
    if (nostr_nip46_transport_encrypt(cli, bunker_pk, req_json,
                                      &cipher_req) != 0 || !cipher_req) {
        printf("client transport_encrypt (decrypt req) failed\n");
        free(req_json); free(ciphertext_result); return 8;
    }
    free(req_json);

    cipher_reply = NULL;
    if (nostr_nip46_bunker_handle_cipher(bun, client_pk, cipher_req,
                                         &cipher_reply) != 0 || !cipher_reply) {
        printf("bunker handle_cipher(nip44_decrypt) failed\n");
        free(cipher_req); free(ciphertext_result); return 9;
    }
    free(cipher_req);
    free(ciphertext_result);

    plain_reply = NULL;
    if (nostr_nip46_transport_decrypt(cli, bunker_pk, cipher_reply,
                                      &plain_reply) != 0 || !plain_reply) {
        printf("client transport_decrypt (decrypt reply) failed\n");
        free(cipher_reply); return 10;
    }
    free(cipher_reply);

    memset(&resp, 0, sizeof resp);
    if (nostr_nip46_response_parse(plain_reply, &resp) != 0 ||
        !resp.result || resp.error) {
        printf("decrypt reply parse failed / had error: '%s'\n",
               plain_reply);
        free(plain_reply); nostr_nip46_response_free(&resp); return 11;
    }

    int equal = strcmp(resp.result, plaintext) == 0;
    if (!equal) {
        printf("plaintext mismatch: want='%s' got='%s'\n",
               plaintext, resp.result);
    }
    nostr_nip46_response_free(&resp);
    free(plain_reply);
    return equal ? 0 : 12;
}

int main(void) {
    /* Use fixed test key pair (BIP-340 xonly is derived by the library).
     * The client and bunker use the same secret so the account-pubkey
     * self-encrypt case in the porthome flow is exercised end-to-end
     * (client == bunker == account). */
    const char *sk_hex = "0000000000000000000000000000000000000000000000000000000000000001";
    const char *xonly = "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798";

    NostrNip46Session *bun = nostr_nip46_bunker_new(NULL);
    if (!bun) { printf("bun new fail\n"); return 1; }
    if (nostr_nip46_client_set_secret(bun, sk_hex) != 0) return 1;

    NostrNip46Session *cli = nostr_nip46_client_new();
    if (!cli) { printf("cli new fail\n"); return 1; }
    if (nostr_nip46_client_set_secret(cli, sk_hex) != 0) return 1;

    /* Establish the ACL: connect with no perms (server grants the
     * default list, which INCLUDES nip44_encrypt/nip44_decrypt — see
     * acl_set_perms in nip46_session.c). */
    {
        const char *cp[3] = { xonly, "", "" };
        char *req = nostr_nip46_request_build("1", "connect", cp, 3);
        if (!req) return 2;
        char *cipher = NULL;
        if (nostr_nip46_transport_encrypt(cli, xonly, req, &cipher) != 0) {
            free(req); return 2;
        }
        free(req);
        char *reply_c = NULL;
        int rc = nostr_nip46_bunker_handle_cipher(bun, xonly, cipher,
                                                  &reply_c);
        free(cipher);
        if (rc != 0 || !reply_c) { printf("connect handle failed\n"); return 2; }
        free(reply_c);
    }

    /* Round-trip a 64-hex payload (the exact shape porthome uses to
     * wrap a 32-byte seed as a UTF-8 JSON string param). */
    const char *seed_hex =
        "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
    int rc = roundtrip(cli, bun, xonly, xonly, xonly, seed_hex);
    if (rc) {
        printf("roundtrip(seed_hex) failed rc=%d\n", rc);
        nostr_nip46_session_free(cli); nostr_nip46_session_free(bun);
        return 20 + rc;
    }

    /* Round-trip a short opaque string. */
    rc = roundtrip(cli, bun, xonly, xonly, xonly, "hello porthome");
    if (rc) {
        printf("roundtrip(hello) failed rc=%d\n", rc);
        nostr_nip46_session_free(cli); nostr_nip46_session_free(bun);
        return 40 + rc;
    }

    nostr_nip46_session_free(cli);
    nostr_nip46_session_free(bun);
    printf("test_bunker_nip44: OK\n");
    return 0;
}
