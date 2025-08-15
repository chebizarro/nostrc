#include "nostr/nip02/nip02.h"
#include "nostr-event.h"
#include "nostr-tag.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void fill32(unsigned char b[32], unsigned char v){ for (int i=0;i<32;++i) b[i]=v; }

int main(void){
    NostrEvent *ev = nostr_event_new();

    /* Build follow list */
    unsigned char author[32]; fill32(author, 0x01);
    NostrFollowEntry ents[2];
    fill32(ents[0].pubkey, 0xA1); ents[0].relay = "wss://r1"; ents[0].petname = "alice";
    fill32(ents[1].pubkey, 0xB2); ents[1].relay = NULL; ents[1].petname = NULL;
    NostrFollowList fl = { .entries = ents, .count = 2 };
    assert(nostr_nip02_build_follow_list(ev, author, &fl, 111) == 0);
    assert(nostr_event_get_kind(ev) == 3);

    /* Parse back */
    NostrFollowList out = {0};
    assert(nostr_nip02_parse_follow_list(ev, &out) == 0);
    assert(out.count >= 2);
    /* verify one entry matches */
    int seen_alice = 0, seen_b2 = 0;
    for (size_t i=0;i<out.count;++i){
        if (out.entries[i].relay && strcmp(out.entries[i].relay, "wss://r1") == 0) seen_alice = 1;
        int allA1 = 1, allB2 = 1;
        for (int j=0;j<32;++j){ if (out.entries[i].pubkey[j] != 0xA1) { allA1 = 0; break; } }
        for (int j=0;j<32;++j){ if (out.entries[i].pubkey[j] != 0xB2) { allB2 = 0; break; } }
        if (allA1) seen_alice = 1;
        if (allB2) seen_b2 = 1;
    }
    assert(seen_alice && seen_b2);
    nostr_nip02_free_follow_list(&out);

    /* Append with dedup */
    NostrFollowEntry add[2];
    fill32(add[0].pubkey, 0xB2); add[0].relay = "wss://dup"; add[0].petname = NULL; /* dup */
    fill32(add[1].pubkey, 0xC3); add[1].relay = "wss://r3"; add[1].petname = "charlie"; /* new */
    size_t before = nostr_tags_size((NostrTags*)nostr_event_get_tags(ev));
    assert(nostr_nip02_append(ev, add, 2) == 0);
    size_t after = nostr_tags_size((NostrTags*)nostr_event_get_tags(ev));
    assert(after == before + 1);

    nostr_event_free(ev);
    printf("test_nip02 OK\n");
    return 0;
}
