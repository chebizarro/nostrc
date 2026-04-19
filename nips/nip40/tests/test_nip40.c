#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr/nip40/nip40.h"

static void test_no_expiration(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Event with no expiration tag */
    int64_t ts = 0;
    int rc = nostr_nip40_get_expiration(ev, &ts);
    assert(rc == -ENOENT);

    /* Not expired */
    assert(!nostr_nip40_is_expired(ev));
    assert(!nostr_nip40_is_expired_at(ev, 9999999999LL));

    /* Relay should accept */
    assert(nostr_nip40_should_relay_accept(ev));

    nostr_event_free(ev);
}

static void test_set_get_expiration(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    int64_t future = (int64_t)time(NULL) + 3600; /* 1 hour from now */
    assert(nostr_nip40_set_expiration(ev, future) == 0);

    int64_t ts = 0;
    int rc = nostr_nip40_get_expiration(ev, &ts);
    assert(rc == 0);
    assert(ts == future);

    nostr_event_free(ev);
}

static void test_is_expired(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Set expiration in the past */
    assert(nostr_nip40_set_expiration(ev, 1000000) == 0);

    assert(nostr_nip40_is_expired(ev));
    assert(nostr_nip40_is_expired_at(ev, 1000000)); /* exact boundary */
    assert(nostr_nip40_is_expired_at(ev, 2000000));
    assert(!nostr_nip40_is_expired_at(ev, 999999));

    /* Relay should reject */
    assert(!nostr_nip40_should_relay_accept(ev));

    nostr_event_free(ev);
}

static void test_not_expired(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Set expiration far in the future */
    int64_t far_future = (int64_t)time(NULL) + 86400 * 365;
    assert(nostr_nip40_set_expiration(ev, far_future) == 0);

    assert(!nostr_nip40_is_expired(ev));
    assert(nostr_nip40_should_relay_accept(ev));

    nostr_event_free(ev);
}

static void test_set_replaces_existing(void) {
    NostrEvent *ev = nostr_event_new();
    assert(ev);

    /* Add an unrelated tag and an expiration tag */
    NostrTags *tags = (NostrTags *)nostr_event_get_tags(ev);
    nostr_tags_append(tags, nostr_tag_new("p", "abc123", NULL));
    nostr_tags_append(tags, nostr_tag_new("expiration", "1000", NULL));

    /* Replace expiration */
    assert(nostr_nip40_set_expiration(ev, 2000) == 0);

    /* Verify only one expiration tag exists and p tag preserved */
    tags = (NostrTags *)nostr_event_get_tags(ev);
    size_t n = nostr_tags_size(tags);
    int exp_count = 0, p_count = 0;
    for (size_t i = 0; i < n; ++i) {
        NostrTag *t = nostr_tags_get(tags, i);
        const char *k = nostr_tag_get(t, 0);
        if (k && strcmp(k, "expiration") == 0) {
            ++exp_count;
            assert(strcmp(nostr_tag_get(t, 1), "2000") == 0);
        } else if (k && strcmp(k, "p") == 0) {
            ++p_count;
        }
    }
    assert(exp_count == 1);
    assert(p_count == 1);

    int64_t ts = 0;
    assert(nostr_nip40_get_expiration(ev, &ts) == 0);
    assert(ts == 2000);

    nostr_event_free(ev);
}

static void test_invalid_inputs(void) {
    /* NULL event */
    int64_t ts = 0;
    assert(nostr_nip40_get_expiration(NULL, &ts) == -EINVAL);
    assert(nostr_nip40_set_expiration(NULL, 1000) == -EINVAL);

    /* NULL output pointer */
    NostrEvent *ev = nostr_event_new();
    assert(nostr_nip40_get_expiration(ev, NULL) == -EINVAL);

    /* Negative timestamp */
    assert(nostr_nip40_set_expiration(ev, -1) == -EINVAL);

    nostr_event_free(ev);
}

int main(void) {
    test_no_expiration();
    test_set_get_expiration();
    test_is_expired();
    test_not_expired();
    test_set_replaces_existing();
    test_invalid_inputs();
    printf("nip40 ok\n");
    return 0;
}
