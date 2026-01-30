/**
 * test_brown_list.c - Tests for relay brown list (nostrc-py1)
 */

#include "nostr-brown-list.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Test basic lifecycle */
void test_brown_list_lifecycle(void) {
    printf("  test_brown_list_lifecycle...");

    NostrBrownList *list = nostr_brown_list_new();
    assert(list != NULL);

    /* Check default configuration */
    assert(nostr_brown_list_get_threshold(list) == 3);
    assert(nostr_brown_list_get_timeout(list) == 1800);

    nostr_brown_list_free(list);

    /* Test custom configuration */
    list = nostr_brown_list_new_with_config(5, 3600);
    assert(list != NULL);
    assert(nostr_brown_list_get_threshold(list) == 5);
    assert(nostr_brown_list_get_timeout(list) == 3600);

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test configuration setters */
void test_brown_list_config(void) {
    printf("  test_brown_list_config...");

    NostrBrownList *list = nostr_brown_list_new();
    assert(list != NULL);

    /* Set threshold */
    nostr_brown_list_set_threshold(list, 10);
    assert(nostr_brown_list_get_threshold(list) == 10);

    /* Test minimum threshold */
    nostr_brown_list_set_threshold(list, 0);
    assert(nostr_brown_list_get_threshold(list) == 1);  /* Should clamp to minimum */

    /* Set timeout */
    nostr_brown_list_set_timeout(list, 7200);
    assert(nostr_brown_list_get_timeout(list) == 7200);

    /* Test minimum timeout */
    nostr_brown_list_set_timeout(list, 10);
    assert(nostr_brown_list_get_timeout(list) == 60);  /* Should clamp to minimum */

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test failure recording */
void test_brown_list_failures(void) {
    printf("  test_brown_list_failures...");

    NostrBrownList *list = nostr_brown_list_new_with_config(3, 60);
    assert(list != NULL);

    const char *url = "wss://failing.relay";

    /* Simulate network is up */
    nostr_brown_list_update_connected_count(list, 1);

    /* Initially not browned */
    assert(!nostr_brown_list_is_browned(list, url));
    assert(nostr_brown_list_get_failure_count(list, url) == 0);

    /* First failure */
    bool browned = nostr_brown_list_record_failure(list, url);
    assert(!browned);  /* Not yet at threshold */
    assert(nostr_brown_list_get_failure_count(list, url) == 1);

    /* Second failure */
    browned = nostr_brown_list_record_failure(list, url);
    assert(!browned);
    assert(nostr_brown_list_get_failure_count(list, url) == 2);

    /* Third failure - should brown list */
    browned = nostr_brown_list_record_failure(list, url);
    assert(browned);
    assert(nostr_brown_list_is_browned(list, url));
    assert(nostr_brown_list_should_skip(list, url));

    /* Check time remaining */
    int remaining = nostr_brown_list_get_time_remaining(list, url);
    assert(remaining > 0 && remaining <= 60);

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test success recording */
void test_brown_list_success(void) {
    printf("  test_brown_list_success...");

    NostrBrownList *list = nostr_brown_list_new_with_config(2, 60);
    assert(list != NULL);

    const char *url = "wss://flaky.relay";

    /* Simulate network is up */
    nostr_brown_list_update_connected_count(list, 1);

    /* Record some failures */
    nostr_brown_list_record_failure(list, url);
    nostr_brown_list_record_failure(list, url);
    assert(nostr_brown_list_is_browned(list, url));

    /* Record success - should clear */
    nostr_brown_list_record_success(list, url);
    assert(!nostr_brown_list_is_browned(list, url));
    assert(nostr_brown_list_get_failure_count(list, url) == 0);

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test network health check */
void test_brown_list_network_health(void) {
    printf("  test_brown_list_network_health...");

    NostrBrownList *list = nostr_brown_list_new_with_config(2, 60);
    assert(list != NULL);

    const char *url = "wss://test.relay";

    /* No connected relays - should not brown list even with failures */
    nostr_brown_list_update_connected_count(list, 0);

    nostr_brown_list_record_failure(list, url);
    nostr_brown_list_record_failure(list, url);

    /* Should NOT be browned because network is down */
    assert(!nostr_brown_list_is_browned(list, url));

    /* Now network comes up */
    nostr_brown_list_update_connected_count(list, 1);

    /* Next failure should trigger brown list */
    nostr_brown_list_record_failure(list, url);
    assert(nostr_brown_list_is_browned(list, url));

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test manual clear */
void test_brown_list_clear(void) {
    printf("  test_brown_list_clear...");

    NostrBrownList *list = nostr_brown_list_new_with_config(2, 60);
    assert(list != NULL);

    const char *url1 = "wss://relay1.test";
    const char *url2 = "wss://relay2.test";

    nostr_brown_list_update_connected_count(list, 1);

    /* Brown both relays */
    nostr_brown_list_record_failure(list, url1);
    nostr_brown_list_record_failure(list, url1);
    nostr_brown_list_record_failure(list, url2);
    nostr_brown_list_record_failure(list, url2);

    assert(nostr_brown_list_is_browned(list, url1));
    assert(nostr_brown_list_is_browned(list, url2));

    /* Clear one relay */
    bool cleared = nostr_brown_list_clear_relay(list, url1);
    assert(cleared);
    assert(!nostr_brown_list_is_browned(list, url1));
    assert(nostr_brown_list_is_browned(list, url2));

    /* Clear all */
    nostr_brown_list_clear_all(list);
    assert(!nostr_brown_list_is_browned(list, url2));

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test statistics */
void test_brown_list_stats(void) {
    printf("  test_brown_list_stats...");

    NostrBrownList *list = nostr_brown_list_new_with_config(2, 60);
    assert(list != NULL);

    nostr_brown_list_update_connected_count(list, 1);

    /* Add some relays in various states */
    /* For "healthy" relay, we need to first have a failure then success to create an entry */
    nostr_brown_list_record_failure(list, "wss://healthy.relay");
    nostr_brown_list_record_success(list, "wss://healthy.relay");

    nostr_brown_list_record_failure(list, "wss://failing.relay");

    nostr_brown_list_record_failure(list, "wss://browned.relay");
    nostr_brown_list_record_failure(list, "wss://browned.relay");

    NostrBrownListStats stats;
    nostr_brown_list_get_stats(list, &stats);

    assert(stats.total_entries == 3);
    assert(stats.browned_count == 1);
    assert(stats.failing_count == 1);
    assert(stats.healthy_count == 1);

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test iterator */
void test_brown_list_iterator(void) {
    printf("  test_brown_list_iterator...");

    NostrBrownList *list = nostr_brown_list_new_with_config(2, 60);
    assert(list != NULL);

    nostr_brown_list_update_connected_count(list, 1);

    /* Add some browned relays */
    nostr_brown_list_record_failure(list, "wss://relay1.test");
    nostr_brown_list_record_failure(list, "wss://relay1.test");
    nostr_brown_list_record_failure(list, "wss://relay2.test");
    nostr_brown_list_record_failure(list, "wss://relay2.test");

    /* Also add a non-browned relay */
    nostr_brown_list_record_failure(list, "wss://relay3.test");

    /* Iterate only browned relays */
    NostrBrownListIterator *iter = nostr_brown_list_iterator_new(list, true);
    assert(iter != NULL);

    int count = 0;
    const char *url;
    int failure_count;
    int time_remaining;

    while (nostr_brown_list_iterator_next(iter, &url, &failure_count, &time_remaining)) {
        count++;
        assert(url != NULL);
        assert(time_remaining > 0);
    }

    assert(count == 2);  /* Only browned relays */

    nostr_brown_list_iterator_free(iter);

    /* Iterate all relays */
    iter = nostr_brown_list_iterator_new(list, false);
    count = 0;

    while (nostr_brown_list_iterator_next(iter, &url, NULL, NULL)) {
        count++;
    }

    assert(count == 3);  /* All relays */

    nostr_brown_list_iterator_free(iter);

    nostr_brown_list_free(list);

    printf(" PASS\n");
}

/* Test expiry */
void test_brown_list_expiry(void) {
    printf("  test_brown_list_expiry...");

    /* Use very short timeout for testing */
    NostrBrownList *list = nostr_brown_list_new_with_config(1, 60);  /* 60s minimum */
    assert(list != NULL);

    nostr_brown_list_update_connected_count(list, 1);

    const char *url = "wss://expiring.relay";

    nostr_brown_list_record_failure(list, url);
    assert(nostr_brown_list_is_browned(list, url));

    /* Note: We can't actually test expiry without sleeping for 60+ seconds
     * In production, the timeout would be configurable to shorter values for testing.
     * For now, just verify the time remaining is reasonable. */
    int remaining = nostr_brown_list_get_time_remaining(list, url);
    assert(remaining > 0 && remaining <= 60);

    /* Test manual expiry function */
    int expired = nostr_brown_list_expire_stale(list);
    assert(expired == 0);  /* Nothing expired yet */

    nostr_brown_list_free(list);

    printf(" PASS (skipped timing test)\n");
}

int main(void) {
    printf("Running brown list tests (nostrc-py1)...\n");

    test_brown_list_lifecycle();
    test_brown_list_config();
    test_brown_list_failures();
    test_brown_list_success();
    test_brown_list_network_health();
    test_brown_list_clear();
    test_brown_list_stats();
    test_brown_list_iterator();
    test_brown_list_expiry();

    printf("All brown list tests passed!\n");
    return 0;
}
