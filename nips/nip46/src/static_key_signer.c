#include "nip46.h"
#include "nostr.h"
#include "nip04.h"
#include "nip44.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *secret_key;
    char **session_keys;
    nip46_session_t *sessions;
    int session_count;
    nip46_relay_readwrite_t *relays_to_advertise;
    bool (*authorize_request)(bool harmless, const char *from, const char *secret);
} static_key_signer_t;

static_key_signer_t* create_static_key_signer(const char *secret_key) {
    static_key_signer_t *signer = (static_key_signer_t*) malloc(sizeof(static_key_signer_t));
    signer->secret_key = strdup(secret_key);
    signer->session_keys = NULL;
    signer->sessions = NULL;
    signer->session_count = 0;
    signer->relays_to_advertise = NULL;
    signer->authorize_request = NULL;
    return signer;
}

nip46_session_t get_or_create_session(static_key_signer_t *signer, const char *client_pubkey) {
    for (int i = 0; i < signer->session_count; i++) {
        if (strcmp(signer->session_keys[i], client_pubkey) == 0) {
            return signer->sessions[i];
        }
    }

    nip46_session_t session;
    // Compute shared and conversation keys
    // ...

    set_session(signer, client_pubkey, session);
    return session;
}

void set_session(static_key_signer_t *signer, const char *client_pubkey, nip46_session_t session) {
    signer->session_keys = (char**) realloc(signer->session_keys, sizeof(char*) * (signer->session_count + 1));
    signer->sessions = (nip46_session_t*) realloc(signer->sessions, sizeof(nip46_session_t) * (signer->session_count + 1));
    signer->session_keys[signer->session_count] = strdup(client_pubkey);
    signer->sessions[signer->session_count] = session;
    signer->session_count++;
}

void handle_request(static_key_signer_t *signer, nostr_event_t *event) {
    // Implement handle request logic
}
