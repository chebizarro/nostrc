#ifndef NIP42_H
#define NIP42_H

#include "nostr.h"
#include <stdbool.h>
#include <time.h>

typedef struct {
    char* id;
    char* pubkey;
    long created_at;
    int kind;
    nostr_Tags tags;
    char* content;
} nip42_Event;

// CreateUnsignedAuthEvent creates an event which should be sent via an "AUTH" command.
// If the authentication succeeds, the user will be authenticated as pubkey.
nip42_Event* nip42_create_unsigned_auth_event(const char* challenge, const char* pubkey, const char* relay_url);

// ValidateAuthEvent checks whether event is a valid NIP-42 event for given challenge and relay_url.
// The result of the validation is encoded in the ok bool.
bool nip42_validate_auth_event(const nip42_Event* event, const char* challenge, const char* relay_url, char** pubkey);

// Free memory allocated for nip42_Event.
void nip42_event_free(nip42_Event* event);

#endif // NIP42_H
