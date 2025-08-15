#include "nostr/nip02/nip02.h"
#include "nostr-event.h"
#include <stdio.h>
#include <string.h>

static void set32(unsigned char out[32], unsigned char v) {
    memset(out, v, 32);
}

int main(void) {
    NostrEvent *ev = nostr_event_new();
    if (!ev) return 1;

    unsigned char author[32]; set32(author, 0xAA);

    NostrFollowEntry entries[2];
    memset(entries, 0, sizeof(entries));
    set32(entries[0].pubkey, 0x11);
    entries[0].relay = NULL;     /* optional */
    entries[0].petname = NULL;   /* optional */
    set32(entries[1].pubkey, 0x22);
    entries[1].relay = "wss://relay.example";
    entries[1].petname = "alice";

    NostrFollowList list = { .entries = entries, .count = 2 };

    if (nostr_nip02_build_follow_list(ev, author, &list, 123456789) != 0) return 2;

    /* Parse back */
    NostrFollowList parsed = {0};
    if (nostr_nip02_parse_follow_list(ev, &parsed) != 0) return 3;

    /* Append one duplicate and one new */
    NostrFollowEntry add[2]; memset(add, 0, sizeof(add));
    set32(add[0].pubkey, 0x22); /* duplicate */
    set32(add[1].pubkey, 0x33); /* new */
    int appended = nostr_nip02_append(ev, add, 2);
    if (appended < 0) return 4;

    /* Cleanup */
    nostr_nip02_free_follow_list(&parsed);
    nostr_event_free(ev);
    return 0;
}
