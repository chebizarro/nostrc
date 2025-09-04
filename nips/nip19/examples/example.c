// Updated NIP-19 examples using canonical APIs and unified pointer helpers
#include "nostr/nip19/nip19.h"
#include <stdio.h>
#include <stdlib.h>

static void example_nprofile(void) {
    NostrNProfileConfig cfg = {
        .public_key = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d",
    };
    NostrPointer *p = NULL; char *bech = NULL;
    if (nostr_pointer_from_nprofile_config(&cfg, &p) == 0 &&
        nostr_pointer_to_bech32(p, &bech) == 0) {
        printf("nprofile: %s\n", bech);
    }
    free(bech); nostr_pointer_free(p);
}

static void example_nevent(void) {
    const char *relays[] = { "wss://r.x.com" };
    NostrNEventConfig cfg = {
        .id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        .author = NULL, .kind = 1,
        .relays = relays, .relays_count = 1,
    };
    NostrPointer *p = NULL; char *bech = NULL;
    if (nostr_pointer_from_nevent_config(&cfg, &p) == 0 &&
        nostr_pointer_to_bech32(p, &bech) == 0) {
        printf("nevent: %s\n", bech);
    }
    free(bech); nostr_pointer_free(p);
}

static void example_naddr(void) {
    NostrNAddrConfig cfg = {
        .identifier = "my-d-tag",
        .public_key = "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d",
        .kind = 30023,
    };
    NostrPointer *p = NULL; char *bech = NULL;
    if (nostr_pointer_from_naddr_config(&cfg, &p) == 0 &&
        nostr_pointer_to_bech32(p, &bech) == 0) {
        printf("naddr: %s\n", bech);
    }
    free(bech); nostr_pointer_free(p);
}

static void example_nrelay_multi(void) {
    const char *relays[] = { "wss://r.x.com", "wss://relay.example.com" };
    char *bech = NULL;
    if (nostr_nip19_encode_nrelay_multi(relays, 2, &bech) == 0) {
        printf("nrelay (2): %s\n", bech);
        free(bech);
    }
}

static void example_parse_roundtrip(const char *bech) {
    NostrPointer *p = NULL; char *out = NULL;
    if (nostr_pointer_parse(bech, &p) == 0) {
        if (nostr_pointer_to_bech32(p, &out) == 0) {
            printf("roundtrip: %s\n", out);
        }
    }
    free(out); nostr_pointer_free(p);
}

int main(void) {
    example_nprofile();
    example_nevent();
    example_naddr();
    example_nrelay_multi();
    // Try parsing a bech32 back into a pointer (replace with your own)
    example_parse_roundtrip("nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69uhhytnc9e3k7mgpz4mhxue69uhkg6nzv9ejuumpv34kytnrdaksjlyr9p");
    return 0;
}
