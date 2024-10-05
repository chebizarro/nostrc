#include <nostr/nostr.h>
#include <nostr/nip04.h>
#include <nostr/nip05.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Set up and initialize the JSON interface
    nostr_set_json_interface(&cjson_interface);
    nostr_json_init();

    // Example NIP-05 identifier
    const char *identifier = "example@domain.com";

    // Validate identifier
    if (is_valid_identifier(identifier)) {
        printf("Valid NIP-05 identifier\n");
    } else {
        printf("Invalid NIP-05 identifier\n");
    }

    // Query identifier
    char *pubkey;
    char **relays;
    if (query_identifier(identifier, &pubkey, &relays) == 0) {
        printf("Public Key: %s\n", pubkey);
        if (relays) {
            for (int i = 0; relays[i] != NULL; i++) {
                printf("Relay: %s\n", relays[i]);
                free(relays[i]);
            }
            free(relays);
        }
        free(pubkey);
    } else {
        printf("Failed to query identifier\n");
    }

    // Clean up
    nostr_json_cleanup();

    return 0;
}
