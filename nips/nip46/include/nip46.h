#ifndef NIP46_H
#define NIP46_H

#include <stdlib.h>
#include <stdbool.h>
#include "nostr.h"

#define NIP46_VERSION 2

typedef struct {
    char *id;
    char *method;
    char **params;
    int params_len;
} nip46_request_t;

typedef struct {
    char *id;
    char *error;
    char *result;
} nip46_response_t;

typedef struct {
    uint8_t shared_key[32];
    uint8_t conversation_key[32];
} nip46_session_t;

typedef struct {
    bool read;
    bool write;
} nip46_relay_readwrite_t;

typedef struct {
    char *client_secret_key;
    char *target;
    char **relays;
    int relays_len;
    uint8_t shared_secret[32];
    nostr_event_t *listeners;
    nostr_event_t *expecting_auth;
    char *id_prefix;
    void (*on_auth)(char *auth_url);
} nip46_bunker_client_t;

bool nip46_is_valid_bunker_url(const char *input);
nip46_session_t nip46_create_session(const char *client_pubkey);
nip46_request_t nip46_parse_request(const nostr_event_t *event, nip46_session_t *session);
nip46_response_t nip46_make_response(const char *id, const char *requester, const char *result, const char *error, nip46_session_t *session);
nip46_bunker_client_t* nip46_connect_bunker(const char *client_secret_key, const char *bunker_url_or_nip05, nostr_simple_pool_t *pool, void (*on_auth)(char *auth_url));
nip46_bunker_client_t* nip46_new_bunker(const char *client_secret_key, const char *target_pubkey, char **relays, int relays_len, nostr_simple_pool_t *pool, void (*on_auth)(char *auth_url));
char* nip46_rpc(nip46_bunker_client_t *client, const char *method, char **params, int params_len);
char* nip46_get_public_key(nip46_bunker_client_t *client);
char* nip46_sign_event(nip46_bunker_client_t *client, nostr_event_t *event);

#endif // NIP46_H
