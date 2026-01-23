/**
 * NIP-18 Reposts Example
 *
 * This example demonstrates how to use the NIP-18 library to:
 * - Create repost events (kind 6)
 * - Create generic reposts (kind 16)
 * - Create quote reposts (kind 1 with q-tag)
 * - Parse repost events
 */

#include "nostr/nip18/nip18.h"
#include "nostr-event.h"
#include <stdio.h>
#include <string.h>

/* Helper to print hex */
static void print_hex(const unsigned char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

int main(void) {
    printf("NIP-18 Reposts Example\n");
    printf("======================\n\n");

    /* Example event ID and pubkey (normally from a real event) */
    unsigned char event_id[32] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef
    };

    unsigned char author_pk[32] = {
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
        0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10
    };

    /* 1. Create a simple repost (kind 6) */
    printf("1. Creating a note repost (kind 6):\n");
    NostrEvent *repost = nostr_nip18_create_repost_from_id(
        event_id,
        author_pk,
        "wss://relay.damus.io",
        NULL  /* No embedded JSON */
    );

    if (repost) {
        printf("   Kind: %d\n", nostr_event_get_kind(repost));
        printf("   Is repost: %s\n", nostr_nip18_is_repost(repost) ? "yes" : "no");
        printf("   Is note repost: %s\n", nostr_nip18_is_note_repost(repost) ? "yes" : "no");
        nostr_event_free(repost);
    }
    printf("\n");

    /* 2. Create a generic repost (kind 16) for a long-form article */
    printf("2. Creating a generic repost (kind 16) for long-form article:\n");
    NostrEvent *generic_repost = nostr_nip18_create_generic_repost_from_id(
        event_id,
        author_pk,
        30023,  /* Long-form content kind */
        "wss://relay.nostr.band",
        NULL
    );

    if (generic_repost) {
        printf("   Kind: %d\n", nostr_event_get_kind(generic_repost));
        printf("   Is generic repost: %s\n", nostr_nip18_is_generic_repost(generic_repost) ? "yes" : "no");

        /* Parse to verify k-tag */
        NostrRepostInfo info;
        if (nostr_nip18_parse_repost(generic_repost, &info) == 0) {
            printf("   Reposted kind: %d\n", info.repost_kind);
            nostr_nip18_repost_info_clear(&info);
        }
        nostr_event_free(generic_repost);
    }
    printf("\n");

    /* 3. Create a quote repost */
    printf("3. Creating a quote post (kind 1 with q-tag):\n");
    NostrEvent *quote = nostr_event_new();
    nostr_event_set_kind(quote, 1);
    nostr_event_set_content(quote, "This is such a great post! Everyone should see this.");

    if (nostr_nip18_add_q_tag(quote, event_id, "wss://relay.damus.io", author_pk) == 0) {
        printf("   Kind: %d\n", nostr_event_get_kind(quote));
        printf("   Has quote: %s\n", nostr_nip18_has_quote(quote) ? "yes" : "no");
        printf("   Content: %s\n", nostr_event_get_content(quote));

        /* Parse the quote info */
        NostrQuoteInfo quote_info;
        if (nostr_nip18_get_quote(quote, &quote_info) == 0) {
            printf("   Quoted event ID: ");
            print_hex(quote_info.quoted_event_id, 32);
            printf("\n");
            if (quote_info.relay_hint) {
                printf("   Relay hint: %s\n", quote_info.relay_hint);
            }
            nostr_nip18_quote_info_clear(&quote_info);
        }
    }
    nostr_event_free(quote);
    printf("\n");

    /* 4. Parse an existing repost */
    printf("4. Parsing a repost event:\n");
    NostrEvent *to_parse = nostr_nip18_create_repost_from_id(
        event_id,
        author_pk,
        "wss://nos.lol",
        "{\"kind\":1,\"content\":\"Hello world!\"}"
    );

    if (to_parse) {
        NostrRepostInfo info;
        if (nostr_nip18_parse_repost(to_parse, &info) == 0) {
            printf("   Has repost event: %s\n", info.has_repost_event ? "yes" : "no");
            printf("   Has repost pubkey: %s\n", info.has_repost_pubkey ? "yes" : "no");
            printf("   Reposted kind: %d\n", info.repost_kind);
            if (info.relay_hint) {
                printf("   Relay hint: %s\n", info.relay_hint);
            }
            if (info.embedded_json) {
                printf("   Embedded JSON: %s\n", info.embedded_json);
            }
            printf("   Repost event ID: ");
            print_hex(info.repost_event_id, 32);
            printf("\n");
            nostr_nip18_repost_info_clear(&info);
        }
        nostr_event_free(to_parse);
    }

    printf("\nExample complete!\n");
    return 0;
}
