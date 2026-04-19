#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip75/nip75.h"

static void test_parse_goal(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP75_KIND_ZAP_GOAL);
    nostr_event_set_content(ev, "Help fund my project!");

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("amount", "100000000", NULL));
    nostr_tags_append(tags, nostr_tag_new("closed_at", "1700000000", NULL));
    nostr_tags_append(tags, nostr_tag_new("image", "https://example.com/goal.jpg", NULL));
    nostr_tags_append(tags, nostr_tag_new("summary", "Project funding", NULL));
    nostr_tags_append(tags, nostr_tag_new("r", "https://example.com/project", NULL));

    NostrNip75Goal goal;
    assert(nostr_nip75_parse(ev, &goal) == 0);
    assert(goal.amount == 100000000);
    assert(goal.closed_at == 1700000000);
    assert(strcmp(goal.image, "https://example.com/goal.jpg") == 0);
    assert(strcmp(goal.summary, "Project funding") == 0);
    assert(strcmp(goal.url, "https://example.com/project") == 0);

    nostr_event_free(ev);
}

static void test_parse_goal_minimal(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("amount", "50000", NULL));

    NostrNip75Goal goal;
    assert(nostr_nip75_parse(ev, &goal) == 0);
    assert(goal.amount == 50000);
    assert(goal.closed_at == 0);
    assert(goal.image == NULL);
    assert(goal.summary == NULL);
    assert(goal.url == NULL);

    nostr_event_free(ev);
}

static void test_parse_goal_no_amount(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("summary", "No amount", NULL));

    NostrNip75Goal goal;
    assert(nostr_nip75_parse(ev, &goal) == -ENOENT);

    nostr_event_free(ev);
}

static void test_validate(void) {
    NostrEvent *ev = nostr_event_new();
    nostr_event_set_kind(ev, NOSTR_NIP75_KIND_ZAP_GOAL);

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("amount", "100000", NULL));

    /* Create relays tag manually */
    NostrTag *relay_tag = nostr_tag_new("relays", "wss://relay1.example.com", NULL);
    nostr_tag_append(relay_tag, "wss://relay2.example.com");
    nostr_tags_append(tags, relay_tag);

    assert(nostr_nip75_validate(ev));

    /* Wrong kind */
    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip75_validate(ev));

    nostr_event_free(ev);

    /* Missing relays */
    NostrEvent *ev2 = nostr_event_new();
    nostr_event_set_kind(ev2, NOSTR_NIP75_KIND_ZAP_GOAL);
    NostrTags *tags2 = (NostrTags *)nostr_event_get_tags(ev2);
    nostr_tags_append(tags2, nostr_tag_new("amount", "100000", NULL));
    assert(!nostr_nip75_validate(ev2));
    nostr_event_free(ev2);
}

static void test_is_zap_goal(void) {
    NostrEvent *ev = nostr_event_new();

    nostr_event_set_kind(ev, NOSTR_NIP75_KIND_ZAP_GOAL);
    assert(nostr_nip75_is_zap_goal(ev));

    nostr_event_set_kind(ev, 1);
    assert(!nostr_nip75_is_zap_goal(ev));

    nostr_event_free(ev);
}

static void test_get_relays(void) {
    NostrEvent *ev = nostr_event_new();

    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    NostrTag *relay_tag = nostr_tag_new("relays", "wss://relay1.example.com", NULL);
    nostr_tag_append(relay_tag, "wss://relay2.example.com");
    nostr_tag_append(relay_tag, "wss://relay3.example.com");
    nostr_tags_append(tags, relay_tag);

    const char *relays[4];
    size_t count = 0;
    assert(nostr_nip75_get_relays(ev, relays, 4, &count) == 0);
    assert(count == 3);
    assert(strcmp(relays[0], "wss://relay1.example.com") == 0);
    assert(strcmp(relays[1], "wss://relay2.example.com") == 0);
    assert(strcmp(relays[2], "wss://relay3.example.com") == 0);

    /* Test max limit */
    count = 0;
    assert(nostr_nip75_get_relays(ev, relays, 2, &count) == 0);
    assert(count == 2);

    nostr_event_free(ev);
}

static void test_expired(void) {
    NostrNip75Goal goal = { .amount = 100000, .closed_at = 1700000000 };

    /* Before deadline */
    assert(!nostr_nip75_is_expired(&goal, 1699999999));
    /* After deadline */
    assert(nostr_nip75_is_expired(&goal, 1700000001));
    /* No deadline */
    NostrNip75Goal open = { .amount = 100000, .closed_at = 0 };
    assert(!nostr_nip75_is_expired(&open, 9999999999LL));
}

static void test_complete(void) {
    assert(nostr_nip75_is_complete(100000, 100000));
    assert(nostr_nip75_is_complete(150000, 100000));
    assert(!nostr_nip75_is_complete(50000, 100000));
    assert(!nostr_nip75_is_complete(0, 100000));
}

static void test_progress_percent(void) {
    assert(fabs(nostr_nip75_progress_percent(50000, 100000) - 50.0) < 0.01);
    assert(fabs(nostr_nip75_progress_percent(100000, 100000) - 100.0) < 0.01);
    assert(fabs(nostr_nip75_progress_percent(0, 100000) - 0.0) < 0.01);
    assert(fabs(nostr_nip75_progress_percent(150000, 100000) - 150.0) < 0.01);
    assert(fabs(nostr_nip75_progress_percent(0, 0) - 0.0) < 0.01);
}

static void test_create_goal(void) {
    NostrEvent *ev = nostr_event_new();

    NostrNip75Goal goal = {
        .amount = 500000000,
        .closed_at = 1700000000,
        .image = "https://example.com/goal.png",
        .summary = "Help us build",
        .url = "https://example.com",
        .a_tag = NULL,
    };

    assert(nostr_nip75_create_goal(ev, &goal, "Build a relay!") == 0);
    assert(nostr_event_get_kind(ev) == NOSTR_NIP75_KIND_ZAP_GOAL);
    assert(strcmp(nostr_event_get_content(ev), "Build a relay!") == 0);

    /* Add relays */
    const char *relays[] = { "wss://relay1.example.com", "wss://relay2.example.com" };
    assert(nostr_nip75_add_relays(ev, relays, 2) == 0);
    assert(nostr_nip75_validate(ev));

    /* Round-trip parse */
    NostrNip75Goal parsed;
    assert(nostr_nip75_parse(ev, &parsed) == 0);
    assert(parsed.amount == 500000000);
    assert(parsed.closed_at == 1700000000);
    assert(strcmp(parsed.image, "https://example.com/goal.png") == 0);

    const char *r[4];
    size_t r_count = 0;
    assert(nostr_nip75_get_relays(ev, r, 4, &r_count) == 0);
    assert(r_count == 2);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    assert(!nostr_nip75_is_zap_goal(NULL));
    assert(!nostr_nip75_validate(NULL));

    NostrNip75Goal goal;
    assert(nostr_nip75_parse(NULL, &goal) == -EINVAL);

    const char *r[2];
    size_t count;
    assert(nostr_nip75_get_relays(NULL, r, 2, &count) == -EINVAL);

    assert(nostr_nip75_create_goal(NULL, &goal, "x") == -EINVAL);
    assert(nostr_nip75_add_relays(NULL, r, 1) == -EINVAL);
}

int main(void) {
    test_parse_goal();
    test_parse_goal_minimal();
    test_parse_goal_no_amount();
    test_validate();
    test_is_zap_goal();
    test_get_relays();
    test_expired();
    test_complete();
    test_progress_percent();
    test_create_goal();
    test_invalid_inputs();
    printf("nip75 ok\n");
    return 0;
}
