#include <nostr/nostr.h>
#include <nostr/nip04.h>
#include <nostr/nip05.h>
#include <nostr/nip06.h>
#include <nostr/nip10.h>
#include <nostr/nip11.h>
#include <nostr/nip13.h>
#include "nostr-event.h"
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Set up and initialize the JSON interface
    nostr_set_json_interface(&cjson_interface);
    nostr_json_init();

    // Example NIP-11 usage
    const char *url = "https://relay.example.com";
    RelayInformationDocument *info = fetch_relay_info(url);
    if (info) {
        printf("Relay Name: %s\n", info->name);
        printf("Relay Description: %s\n", info->description);
        free_relay_info(info);
    } else {
        printf("Failed to fetch relay info\n");
    }

    // Example NIP-13 usage
    Event event;
    event_init(&event);
    event.pubkey = "public_key";
    event.content = "Hello, Nostr!";
    event.created_at = time(NULL);

    int difficulty = 20;
    time_t timeout = 10;
    int result = nip13_generate(&event, difficulty, timeout);
    if (result == 0) {
        printf("Proof of work successful! Event ID: %s\n", nostr_event_get_id((NostrEvent*)&event));
    } else if (result == NIP13_ERR_GENERATE_TIMEOUT) {
        printf("Failed to generate proof of work: timeout\n");
    } else {
        printf("Failed to generate proof of work\n");
    }

    // Clean up
    nostr_json_cleanup();

    return 0;
}
