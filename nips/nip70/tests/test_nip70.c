#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip70/nip70.h"

static void test_not_protected(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Event with no tags is not protected */
    assert(!nostr_nip70_is_protected(ev));
    assert(nostr_nip70_can_rebroadcast(ev));

    /* Add some unrelated tags */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));
    nostr_tags_append(tags, nostr_tag_new("e", "def456", NULL));

    assert(!nostr_nip70_is_protected(ev));
    assert(nostr_nip70_can_rebroadcast(ev));

    nostr_event_free(ev);
}

static void test_is_protected(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Add protection tag */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));
    nostr_tags_append(tags, nostr_tag_new("-", NULL));

    assert(nostr_nip70_is_protected(ev));
    assert(!nostr_nip70_can_rebroadcast(ev));

    nostr_event_free(ev);
}

static void test_dash_with_value_not_protected(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* A "-" tag with a value is NOT a protection tag per NIP-70 */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("-", "some-value", NULL));

    assert(!nostr_nip70_is_protected(ev));
    assert(nostr_nip70_can_rebroadcast(ev));

    nostr_event_free(ev);
}

static void test_add_protection(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Add an unrelated tag first */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));

    assert(!nostr_nip70_is_protected(ev));

    /* Add protection */
    assert(nostr_nip70_add_protection(ev) == 0);
    assert(nostr_nip70_is_protected(ev));

    /* Verify p tag is still present */
    tags = (NostrTags *)nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    int p_count = 0, dash_count = 0;
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "p") == 0) ++p_count;
        if (k && strcmp(k, "-") == 0 && nostr_tag_size(t) == 1) ++dash_count;
    }
    assert(p_count == 1);
    assert(dash_count == 1);

    nostr_event_free(ev);
}

static void test_add_protection_idempotent(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Add protection twice */
    assert(nostr_nip70_add_protection(ev) == 0);
    assert(nostr_nip70_add_protection(ev) == 0);

    /* Should still have exactly one protection tag */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    int dash_count = 0;
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "-") == 0 && nostr_tag_size(t) == 1) ++dash_count;
    }
    assert(dash_count == 1);

    nostr_event_free(ev);
}

static void test_remove_protection(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Set up event with p tag and protection */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));
    nostr_tags_append(tags, nostr_tag_new("-", NULL));

    assert(nostr_nip70_is_protected(ev));

    /* Remove protection */
    assert(nostr_nip70_remove_protection(ev) == 0);
    assert(!nostr_nip70_is_protected(ev));
    assert(nostr_nip70_can_rebroadcast(ev));

    /* Verify p tag is still present */
    tags = (NostrTags *)nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    assert(n == 1);
    const char *k = nostr_tag_get(nostr_tags_get(tags, 0), 0);
    assert(strcmp(k, "p") == 0);

    nostr_event_free(ev);
}

static void test_remove_protection_no_op(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Remove from unprotected event should be a no-op */
    assert(nostr_nip70_remove_protection(ev) == 0);
    assert(!nostr_nip70_is_protected(ev));

    nostr_event_free(ev);
}

static void test_has_embedded_protected(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Kind 6 (repost) with embedded protected event in content */
    nostr_event_set_kind(ev, 6);
    nostr_event_set_content(ev,
        "{\"id\":\"abc\",\"pubkey\":\"def\","
        "\"tags\":[[\"p\",\"xyz\"],[\"-\"]],"
        "\"content\":\"hello\"}");

    assert(nostr_nip70_has_embedded_protected(ev));

    nostr_event_free(ev);
}

static void test_has_embedded_not_protected(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Kind 6 repost without protection tag */
    nostr_event_set_kind(ev, 6);
    nostr_event_set_content(ev,
        "{\"id\":\"abc\",\"pubkey\":\"def\","
        "\"tags\":[[\"p\",\"xyz\"]],"
        "\"content\":\"hello\"}");

    assert(!nostr_nip70_has_embedded_protected(ev));

    nostr_event_free(ev);
}

static void test_embedded_non_repost_ignored(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Kind 1 (regular note) — not a repost, embedded check should return false */
    nostr_event_set_kind(ev, 1);
    nostr_event_set_content(ev,
        "{\"tags\":[[\"p\",\"xyz\"],[\"-\"]],"
        "\"content\":\"hello\"}");

    assert(!nostr_nip70_has_embedded_protected(ev));

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    /* NULL event */
    assert(!nostr_nip70_is_protected(NULL));
    assert(!nostr_nip70_has_embedded_protected(NULL));
    assert(!nostr_nip70_can_rebroadcast(NULL));
    assert(nostr_nip70_add_protection(NULL) == -EINVAL);
    assert(nostr_nip70_remove_protection(NULL) == -EINVAL);
}

int main(void) {
    test_not_protected();
    test_is_protected();
    test_dash_with_value_not_protected();
    test_add_protection();
    test_add_protection_idempotent();
    test_remove_protection();
    test_remove_protection_no_op();
    test_has_embedded_protected();
    test_has_embedded_not_protected();
    test_embedded_non_repost_ignored();
    test_invalid_inputs();
    printf("nip70 ok\n");
    return 0;
}
