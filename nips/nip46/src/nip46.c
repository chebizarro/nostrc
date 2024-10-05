#include "nip46.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nip04.h"
#include "nip44.h"
#include "nostr.h"

#define NIP46_VERSION 2

// Helper function to parse the bunker URL and NIP-05
bool nip46_is_valid_bunker_url(const char *input) {
    return strstr(input, "bunker://") == input;
}

nip46_session_t nip46_create_session(const char *client_pubkey) {
    nip46_session_t session;
    // Initialize session keys here (nip04 and nip44)
    return session;
}

nip46_request_t nip46_parse_request(const nostr_event_t *event, nip46_session_t *session) {
    nip46_request_t request;
    char *plain;
    if (nip44_decrypt(event->content, session->conversation_key, &plain) != 0) {
        if (nip04_decrypt(event->content, session->shared_key, &plain) != 0) {
            // Decryption failed
            return request;
        }
    }
    
    // Parse the decrypted plain text into request
    // Assuming JSON parsing functions are available
    // Example using cJSON:
    // cJSON *json = cJSON_Parse(plain);
    // request.id = cJSON_GetObjectItem(json, "id")->valuestring;
    // request.method = cJSON_GetObjectItem(json, "method")->valuestring;
    // cJSON *params = cJSON_GetObjectItem(json, "params");
    // int params_len = cJSON_GetArraySize(params);
    // request.params_len = params_len;
    // request.params = malloc(sizeof(char*) * params_len);
    // for (int i = 0; i < params_len; i++) {
    //     request.params[i] = strdup(cJSON_GetArrayItem(params, i)->valuestring);
    // }
    // cJSON_Delete(json);
    
    return request;
}

nip46_response_t nip46_make_response(const char *id, const char *requester, const char *result, const char *error, nip46_session_t *session) {
    nip46_response_t response;
    response.id = strdup(id);
    response.result = result ? strdup(result) : NULL;
    response.error = error ? strdup(error) : NULL;
    
    char *jresp = NULL; // Serialize response to JSON
    // Example using cJSON:
    // cJSON *json = cJSON_CreateObject();
    // cJSON_AddStringToObject(json, "id", response.id);
    // if (response.result) cJSON_AddStringToObject(json, "result", response.result);
    // if (response.error) cJSON_AddStringToObject(json, "error", response.error);
    // jresp = cJSON_PrintUnformatted(json);
    // cJSON_Delete(json);
    
    char *ciphertext;
    if (nip04_encrypt(jresp, session->shared_key, &ciphertext) != 0) {
        // Encryption failed
        return response;
    }
    
    nostr_event_t event;
    event.content = ciphertext;
    event.created_at = time(NULL);
    event.kind = NIP46_VERSION;
    event.tags = malloc(sizeof(nostr_tag_t));
    event.tags[0].key = "p";
    event.tags[0].value = strdup(requester);
    
    // Serialize event to eventResponse (assuming event serialization function is available)
    
    return response;
}

nip46_bunker_client_t* nip46_connect_bunker(const char *client_secret_key, const char *bunker_url_or_nip05, nostr_simple_pool_t *pool, void (*on_auth)(char *auth_url)) {
    // Implementation of ConnectBunker
    return NULL;
}

nip46_bunker_client_t* nip46_new_bunker(const char *client_secret_key, const char *target_pubkey, char **relays, int relays_len, nostr_simple_pool_t *pool, void (*on_auth)(char *auth_url)) {
    // Implementation of NewBunker
    return NULL;
}

char* nip46_rpc(nip46_bunker_client_t *client, const char *method, char **params, int params_len) {
    // Implementation of RPC
    return NULL;
}

char* nip46_get_public_key(nip46_bunker_client_t *client) {
    // Implementation of GetPublicKey
    return NULL;
}

char* nip46_sign_event(nip46_bunker_client_t *client, nostr_event_t *event) {
    // Implementation of SignEvent
    return NULL;
}

