#include "nip42.h"
#include <stdio.h>

int main() {
    // Create an unsigned authentication event
    nip42_Event* auth_event = nip42_create_unsigned_auth_event("challenge-string", "public-key", "https://relay.example.com");
    if (!auth_event) {
        printf("Failed to create authentication event.\n");
        return 1;
    }

    // Validate the authentication event
    char* pubkey = NULL;
    if (nip42_validate_auth_event(auth_event, "challenge-string", "https://relay.example.com", &pubkey)) {
        printf("Authentication event is valid. Pubkey: %s\n", pubkey);
        free(pubkey);
    } else {
        printf("Authentication event is invalid.\n");
    }

    // Free the authentication event
    nip42_event_free(auth_event);

    return 0;
}
