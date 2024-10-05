#include <nostr/nostr.h>
#include <nostr/nip04.h>
#include <nostr/nip05.h>
#include <nostr/nip06.h>
#include <nostr/nip10.h>
#include <nostr/nip11.h>
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

    // Clean up
    nostr_json_cleanup();

    return 0;
}
