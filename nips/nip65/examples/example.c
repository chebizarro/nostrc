/**
 * NIP-65 Relay List Metadata - Example Usage
 *
 * This example demonstrates how to:
 * 1. Create and manage a relay list
 * 2. Build a kind 10002 event
 * 3. Parse relay information from an event
 * 4. Query relays by read/write capability
 */
#include "nostr/nip65/nip65.h"
#include "nostr-event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_relay_list(const NostrRelayList *list) {
    if (!list || list->count == 0) {
        printf("  (empty)\n");
        return;
    }

    for (size_t i = 0; i < list->count; ++i) {
        const NostrRelayEntry *e = &list->entries[i];
        const char *perm_str;
        switch (e->permission) {
            case NOSTR_RELAY_PERM_READ: perm_str = "read"; break;
            case NOSTR_RELAY_PERM_WRITE: perm_str = "write"; break;
            default: perm_str = "read/write"; break;
        }
        printf("  %zu. %s (%s)\n", i + 1, e->url, perm_str);
    }
}

static void print_string_array(char **arr, size_t count) {
    if (!arr || count == 0) {
        printf("  (none)\n");
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        printf("  - %s\n", arr[i]);
    }
}

int main(void) {
    printf("=== NIP-65 Relay List Metadata Example ===\n\n");

    /* Step 1: Create a relay list */
    printf("1. Creating a relay list...\n");
    NostrRelayList *my_relays = nostr_nip65_list_new();

    /* Add relays with different permissions */
    nostr_nip65_add_relay(my_relays, "wss://relay.damus.io", NOSTR_RELAY_PERM_READWRITE);
    nostr_nip65_add_relay(my_relays, "wss://nos.lol", NOSTR_RELAY_PERM_READWRITE);
    nostr_nip65_add_relay(my_relays, "wss://relay.snort.social", NOSTR_RELAY_PERM_READ);
    nostr_nip65_add_relay(my_relays, "wss://nostr.wine", NOSTR_RELAY_PERM_READ);
    nostr_nip65_add_relay(my_relays, "wss://purplepag.es", NOSTR_RELAY_PERM_WRITE);

    printf("My relay list:\n");
    print_relay_list(my_relays);
    printf("\n");

    /* Step 2: Query by capability */
    printf("2. Querying relays by capability...\n");

    size_t count;
    char **read_relays = nostr_nip65_get_read_relays(my_relays, &count);
    printf("Read-capable relays (%zu):\n", count);
    print_string_array(read_relays, count);
    nostr_nip65_free_string_array(read_relays);

    char **write_relays = nostr_nip65_get_write_relays(my_relays, &count);
    printf("Write-capable relays (%zu):\n", count);
    print_string_array(write_relays, count);
    nostr_nip65_free_string_array(write_relays);
    printf("\n");

    /* Step 3: Build a kind 10002 event */
    printf("3. Building NIP-65 event (kind 10002)...\n");

    NostrEvent *ev = nostr_event_new();

    /* Example pubkey (in real usage, this comes from the user's keypair) */
    unsigned char author_pk[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };

    int rc = nostr_nip65_create_relay_list(ev, author_pk, my_relays, (uint32_t)time(NULL));
    if (rc == 0) {
        printf("Event created successfully!\n");
        printf("  Kind: %d\n", nostr_event_get_kind(ev));
        printf("  Pubkey: %s\n", nostr_event_get_pubkey(ev));
        printf("  Content: '%s' (empty for NIP-65)\n", nostr_event_get_content(ev));
    } else {
        printf("Failed to create event: %d\n", rc);
    }
    printf("\n");

    /* Step 4: Parse relay list from event */
    printf("4. Parsing relay list from event...\n");

    NostrRelayList *parsed = NULL;
    rc = nostr_nip65_parse_relay_list(ev, &parsed);
    if (rc == 0) {
        printf("Parsed relay list (%zu entries):\n", parsed->count);
        print_relay_list(parsed);
    } else {
        printf("Failed to parse: %d\n", rc);
    }
    printf("\n");

    /* Step 5: Modify and update */
    printf("5. Modifying relay list...\n");

    /* Remove a relay */
    rc = nostr_nip65_remove_relay(parsed, "wss://relay.snort.social");
    if (rc == 0) {
        printf("Removed wss://relay.snort.social\n");
    }

    /* Add a new relay */
    rc = nostr_nip65_add_relay(parsed, "wss://relay.nostr.band", NOSTR_RELAY_PERM_READ);
    if (rc == 0) {
        printf("Added wss://relay.nostr.band (read)\n");
    }

    /* Update the event */
    rc = nostr_nip65_update_relay_list(ev, parsed);
    if (rc == 0) {
        printf("Event updated with new relay list\n");
    }

    printf("\nFinal relay list:\n");
    print_relay_list(parsed);

    /* Step 6: URL validation and normalization */
    printf("\n6. URL validation examples...\n");

    const char *test_urls[] = {
        "wss://relay.example.com",
        "WSS://RELAY.EXAMPLE.COM/",
        "wss://relay.example.com:8080",
        "https://not-a-relay.com",
        "invalid-url",
        NULL
    };

    for (int i = 0; test_urls[i]; ++i) {
        bool valid = nostr_nip65_is_valid_relay_url(test_urls[i]);
        char *normalized = nostr_nip65_normalize_url(test_urls[i]);
        printf("  '%s'\n", test_urls[i]);
        printf("    Valid: %s\n", valid ? "yes" : "no");
        printf("    Normalized: %s\n", normalized ? normalized : "(invalid)");
        free(normalized);
    }

    /* Cleanup */
    nostr_nip65_list_free(my_relays);
    nostr_nip65_list_free(parsed);
    nostr_event_free(ev);

    printf("\n=== Example complete ===\n");
    return 0;
}
