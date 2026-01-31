/**
 * test_event_priority.c - Tests for NostrEventPriority classification (nostrc-7u2)
 */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "nostr-event.h"
#include "nostr-tag.h"

static void expect(int cond, const char *msg) {
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); }
}

int main(void) {
    printf("Testing event priority classification...\n");

    /* Test 1: DM (kind 4) should be CRITICAL */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 4);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_CRITICAL, "DM (kind 4) should be CRITICAL");
        nostr_event_free(ev);
    }

    /* Test 2: Gift wrap (kind 1059) should be CRITICAL */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 1059);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_CRITICAL, "Gift wrap (kind 1059) should be CRITICAL");
        nostr_event_free(ev);
    }

    /* Test 3: Zap (kind 9735) should be CRITICAL */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 9735);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_CRITICAL, "Zap (kind 9735) should be CRITICAL");
        nostr_event_free(ev);
    }

    /* Test 4: Reaction (kind 7) should be LOW */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 7);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_LOW, "Reaction (kind 7) should be LOW");
        nostr_event_free(ev);
    }

    /* Test 5: Repost (kind 6) should be LOW */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 6);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_LOW, "Repost (kind 6) should be LOW");
        nostr_event_free(ev);
    }

    /* Test 6: Regular note (kind 1) with no tags should be NORMAL */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 1);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_NORMAL, "Note without tags should be NORMAL");
        nostr_event_free(ev);
    }

    /* Test 7: Reply (kind 1 with "e" tag) should be HIGH */
    {
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 1);
        NostrTag *e_tag = nostr_tag_new("e", "abc123", NULL);
        NostrTags *tags = nostr_tags_new(1, e_tag);
        nostr_event_set_tags(ev, tags);
        NostrEventPriority p = nostr_event_get_priority(ev, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_HIGH, "Reply (kind 1 with e tag) should be HIGH");
        nostr_event_free(ev);
    }

    /* Test 8: Mention of user (kind 1 with matching "p" tag) should be CRITICAL */
    {
        const char *my_pubkey = "deadbeef1234567890abcdef1234567890abcdef1234567890abcdef12345678";
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 1);
        NostrTag *p_tag = nostr_tag_new("p", my_pubkey, NULL);
        NostrTags *tags = nostr_tags_new(1, p_tag);
        nostr_event_set_tags(ev, tags);
        NostrEventPriority p = nostr_event_get_priority(ev, my_pubkey);
        expect(p == NOSTR_EVENT_PRIORITY_CRITICAL, "Mention of user should be CRITICAL");
        nostr_event_free(ev);
    }

    /* Test 9: Note mentioning someone else should be NORMAL */
    {
        const char *my_pubkey = "deadbeef1234567890abcdef1234567890abcdef1234567890abcdef12345678";
        const char *other_pubkey = "1111111111111111111111111111111111111111111111111111111111111111";
        NostrEvent *ev = nostr_event_new();
        nostr_event_set_kind(ev, 1);
        NostrTag *p_tag = nostr_tag_new("p", other_pubkey, NULL);
        NostrTags *tags = nostr_tags_new(1, p_tag);
        nostr_event_set_tags(ev, tags);
        NostrEventPriority p = nostr_event_get_priority(ev, my_pubkey);
        expect(p == NOSTR_EVENT_PRIORITY_NORMAL, "Note mentioning others should be NORMAL");
        nostr_event_free(ev);
    }

    /* Test 10: NULL event returns NORMAL */
    {
        NostrEventPriority p = nostr_event_get_priority(NULL, NULL);
        expect(p == NOSTR_EVENT_PRIORITY_NORMAL, "NULL event should be NORMAL");
    }

    printf("OK - all event priority tests passed\n");
    return 0;
}
