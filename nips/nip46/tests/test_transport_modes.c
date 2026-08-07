#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nostr/nip46/nip46_bunker.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_envelope.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip04.h"
#include "nostr-keys.h"

static const char *CLIENT_SK =
    "0000000000000000000000000000000000000000000000000000000000000001";
static const char *BUNKER_SK =
    "0000000000000000000000000000000000000000000000000000000000000002";

static void assert_wire_mode(NostrNip46TransportMode mode, const char *cipher) {
    assert(cipher);
    if (mode == NOSTR_NIP46_TRANSPORT_NIP44_V2) {
        assert(strncmp(cipher, "v=2:", 4) != 0);
        assert(strstr(cipher, "?iv=") == NULL);
    } else if (mode == NOSTR_NIP46_TRANSPORT_NIP04_LEGACY) {
        assert(strstr(cipher, "?iv=") != NULL);
    } else {
        assert(strncmp(cipher, "v=2:", 4) == 0);
    }
}

static void exercise_mode(NostrNip46TransportMode mode,
                          const char *client_pk, const char *bunker_pk) {
    NostrNip46Session *client = nostr_nip46_client_new();
    NostrNip46Session *bunker = nostr_nip46_bunker_new(NULL);
    assert(client && bunker);
    assert(nostr_nip46_client_set_secret(client, CLIENT_SK) == 0);
    assert(nostr_nip46_client_set_secret(bunker, BUNKER_SK) == 0);
    assert(nostr_nip46_session_set_transport_mode(client, mode) == 0);
    assert(nostr_nip46_session_set_transport_mode(bunker, mode) == 0);
    assert(nostr_nip46_session_get_transport_mode(client) == mode);
    assert(strcmp(nostr_nip46_transport_mode_name(mode), "unsupported") != 0);

    const char *params[] = { bunker_pk };
    char *request = nostr_nip46_request_build(
        "transport-1", "get_public_key", params, 1);
    assert(request);

    NostrEvent *request_event = NULL;
    assert(nostr_nip46_build_encrypted_request_event(
               client, client_pk, bunker_pk, request, &request_event) == 0);
    assert_wire_mode(mode, nostr_event_get_content(request_event));
    nostr_event_free(request_event);

    char *cipher_request = NULL;
    assert(nostr_nip46_transport_encrypt(
               client, bunker_pk, request, &cipher_request) == 0);
    assert_wire_mode(mode, cipher_request);

    char *cipher_reply = NULL;
    assert(nostr_nip46_bunker_handle_cipher(
               bunker, client_pk, cipher_request, &cipher_reply) == 0);
    assert_wire_mode(mode, cipher_reply);

    char *plain_reply = NULL;
    assert(nostr_nip46_transport_decrypt(
               client, bunker_pk, cipher_reply, &plain_reply) == 0);
    NostrNip46Response response = {0};
    assert(nostr_nip46_response_parse(plain_reply, &response) == 0);
    assert(response.id && strcmp(response.id, "transport-1") == 0);

    nostr_nip46_response_free(&response);
    free(plain_reply);
    free(cipher_reply);
    free(cipher_request);
    free(request);
    nostr_nip46_session_free(bunker);
    nostr_nip46_session_free(client);
}

int main(void) {
    char *client_pk = nostr_key_get_public(CLIENT_SK);
    char *bunker_pk = nostr_key_get_public(BUNKER_SK);
    assert(client_pk && bunker_pk);

    NostrNip46Session *defaults = nostr_nip46_client_new();
    assert(defaults);
    assert(nostr_nip46_session_get_transport_mode(defaults) ==
           NOSTR_NIP46_TRANSPORT_NIP44_V2);
    assert(nostr_nip46_session_set_transport_mode(
               defaults, (NostrNip46TransportMode)99) != 0);
    nostr_nip46_session_free(defaults);

    exercise_mode(NOSTR_NIP46_TRANSPORT_NIP44_V2, client_pk, bunker_pk);
    if (nostr_nip04_legacy_decrypt_enabled()) {
        exercise_mode(NOSTR_NIP46_TRANSPORT_NIP04_LEGACY, client_pk, bunker_pk);
    } else {
        /* Strict AEAD-only builds must refuse the legacy transport outright
         * instead of accepting a mode that can never decrypt. */
        NostrNip46Session *strict = nostr_nip46_client_new();
        assert(strict);
        assert(nostr_nip46_session_set_transport_mode(
                   strict, NOSTR_NIP46_TRANSPORT_NIP04_LEGACY) != 0);
        assert(nostr_nip46_session_get_transport_mode(strict) ==
               NOSTR_NIP46_TRANSPORT_NIP44_V2);
        nostr_nip46_session_free(strict);
    }
    exercise_mode(NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION,
                  client_pk, bunker_pk);

    /* Ciphertext shape never overrides the configured session policy. */
    NostrNip46Session *sender = nostr_nip46_client_new();
    NostrNip46Session *receiver = nostr_nip46_client_new();
    assert(sender && receiver);
    assert(nostr_nip46_client_set_secret(sender, CLIENT_SK) == 0);
    assert(nostr_nip46_client_set_secret(receiver, BUNKER_SK) == 0);
    assert(nostr_nip46_session_set_transport_mode(
               sender,
               NOSTR_NIP46_TRANSPORT_NIP04_AEAD_V2_EXTENSION) == 0);
    char *cipher = NULL;
    assert(nostr_nip46_transport_encrypt(
               sender, bunker_pk, "mode mismatch", &cipher) == 0);
    char *plain = (char *)0x1;
    assert(nostr_nip46_transport_decrypt(
               receiver, client_pk, cipher, &plain) != 0);
    assert(plain == NULL);
    /* A hybrid value must not cross the configured NIP-04 shape check:
     * older code saw ?iv= and then dispatched the v=2 prefix as AEAD. */
    if (nostr_nip04_legacy_decrypt_enabled()) {
        assert(nostr_nip46_session_set_transport_mode(
                   receiver, NOSTR_NIP46_TRANSPORT_NIP04_LEGACY) == 0);
        size_t hybrid_len = strlen(cipher) + strlen("?iv=AAAAAAAAAAAAAAAAAAAAAA==") + 1;
        char *hybrid = malloc(hybrid_len);
        assert(hybrid);
        snprintf(hybrid, hybrid_len, "%s?iv=AAAAAAAAAAAAAAAAAAAAAA==", cipher);
        plain = (char *)0x1;
        assert(nostr_nip46_transport_decrypt(
                   receiver, client_pk, hybrid, &plain) != 0);
        assert(plain == NULL);
        free(hybrid);
    }
    free(cipher);
    nostr_nip46_session_free(receiver);
    nostr_nip46_session_free(sender);

    free(bunker_pk);
    free(client_pk);
    puts("test_transport_modes: OK");
    return 0;
}
