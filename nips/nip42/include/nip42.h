#ifndef NIP42_H
#define NIP42_H

#include "nostr.h"
#include <stdbool.h>
#include <time.h>

#define NIP42_KIND_CLIENT_AUTHENTICATION 22242

typedef struct {
    char* id;
    char* pubkey;
    long created_at;
    int kind;
    NostrTags* tags;
    char* content;
    char* sig;
} nip42_Event;

// CreateUnsignedAuthEvent creates an unsigned kind-22242 event which should be sent via an "AUTH" command after signing.
// If the authentication succeeds, the user will be authenticated as pubkey.
nip42_Event* nip42_create_unsigned_auth_event(const char* challenge, const char* pubkey, const char* relay_url);

// ValidateAuthEvent checks whether event is a valid NIP-42 event for given challenge and relay_url.
// The result of the validation is encoded in the ok bool.
bool nip42_validate_auth_event(const nip42_Event* event, const char* challenge, const char* relay_url, char** pubkey);

// Parse a relay AUTH frame (["AUTH", "<challenge>"]) or already-extracted challenge string.
// Allocates *out_challenge on success; caller must free it.
bool nip42_parse_challenge(const char* raw_msg, char** out_challenge);

// Build a signed AUTH response frame: ["AUTH", <signed kind-22242 event>].
// Allocates *out_json on success; caller must free it.
bool nip42_build_auth_response(const char* challenge, const char* relay_url,
                               const char* secret_key_hex, char** out_json);

// Free memory allocated for nip42_Event.
void nip42_event_free(nip42_Event* event);

#endif // NIP42_H
