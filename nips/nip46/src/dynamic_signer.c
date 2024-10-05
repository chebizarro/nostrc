#include "nip46.h"
#include "nostr.h"
#include "nip04.h"
#include "nip44.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char **session_keys;
    nip46_session_t *sessions;
    int session_count;
    nip46_relay_readwrite_t *relays_to_advertise;
    char* (*get_private_key)(const char *pubkey);
    bool (*authorize_signing)(nostr_event_t event, const char *from, const char *secret);
    void (*on_event_signed)(nostr_event_t event);
    bool (*authorize_encryption)(const char *from, const char *secret);
} dynamic_signer_t;

dynamic_signer_t* create_dynamic_signer(
    char* (*get_private_key)(const char *pubkey),
    bool (*authorize_signing)(nostr_event_t event, const char *from, const char *secret),
    void (*on_event_signed)(nostr_event_t event),
    bool (*authorize_encryption)(const char *from, const char *secret)
) {
    dynamic_signer_t *signer = (dynamic_signer_t*) malloc(sizeof(dynamic_signer_t));
    signer->session_keys = NULL;
    signer->sessions = NULL;
    signer->session_count = 0;
    signer->relays_to_advertise = NULL;
    signer->get_private_key = get_private_key;
    signer->authorize_signing = authorize_signing;
    signer->on_event_signed = on_event_signed;
    signer->authorize_encryption = authorize_encryption;
    return signer;
}

nip46_session_t get_session(dynamic_signer_t *signer, const char *client_pubkey) {
    for (int i = 0; i < signer->session_count; i++) {
        if (strcmp(signer->session_keys[i], client_pubkey) == 0) {
            return signer->sessions[i];
        }
    }
    return nip46_create_session(client_pubkey);
}

void set_session(dynamic_signer_t *signer, const char *client_pubkey, nip46_session_t session) {
    signer->session_keys = (char**) realloc(signer->session_keys, sizeof(char*) * (signer->session_count + 1));
    signer->sessions = (nip46_session_t*) realloc(signer->sessions, sizeof(nip46_session_t) * (signer->session_count + 1));
    signer->session_keys[signer->session_count] = strdup(client_pubkey);
    signer->sessions[signer->session_count] = session;
    signer->session_count++;
}

void handle_request(dynamic_signer_t *signer, nostr_event_t *event) {
    // Implement handle request logic
}
