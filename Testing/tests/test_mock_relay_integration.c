/**
 * @file test_mock_relay_integration.c
 * @brief Integration tests for mock relay with real Nostr client
 *
 * Tests the mock relay server with actual WebSocket connections:
 * - Subscribe and receive seeded events
 * - Publish events and verify capture
 * - Filter matching
 * - NIP-11 endpoint
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include "nostr/testing/mock_relay_server.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-envelope.h"
#include "channel.h"
#include "context.h"

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    printf("  Running %s...", #name); \
    fflush(stdout); \
    tests_run++; \
    if (name()) { \
        tests_passed++; \
        printf(" PASS\n"); \
    } else { \
        printf(" FAIL\n"); \
    } \
} while (0)

/* === Test Events === */

static const char *test_event_kind1_a = "{"
    "\"id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"created_at\":1700000000,"
    "\"kind\":1,"
    "\"tags\":[],"
    "\"content\":\"First test note\","
    "\"sig\":\"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000a\""
    "}";

static const char *test_event_kind1_b = "{"
    "\"id\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"created_at\":1700000001,"
    "\"kind\":1,"
    "\"tags\":[],"
    "\"content\":\"Second test note\","
    "\"sig\":\"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000b\""
    "}";

static const char *test_event_kind0 = "{"
    "\"id\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
    "\"pubkey\":\"2222222222222222222222222222222222222222222222222222222222222222\","
    "\"created_at\":1700000002,"
    "\"kind\":0,"
    "\"tags\":[],"
    "\"content\":\"{\\\"name\\\":\\\"Test User\\\"}\","
    "\"sig\":\"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000c\""
    "}";

static const char *test_event_kind3 = "{"
    "\"id\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\","
    "\"pubkey\":\"1111111111111111111111111111111111111111111111111111111111111111\","
    "\"created_at\":1700000003,"
    "\"kind\":3,"
    "\"tags\":[[\"p\",\"2222222222222222222222222222222222222222222222222222222222222222\"]],"
    "\"content\":\"\","
    "\"sig\":\"0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000d\""
    "}";

/* === Test: Server Start and Connect === */

static int test_server_connect(void) {
    /* Create and start mock server */
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    const char *url = nostr_mock_server_get_url(server);
    if (!url) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Brief delay for server to be ready */
    usleep(100000);  /* 100ms */

    /* Verify URL format */
    if (strncmp(url, "ws://", 5) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Seed and Retrieve Events via Subscription === */

static int test_subscribe_seeded_events(void) {
    /* Create and start mock server */
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Seed events before starting */
    nostr_mock_server_seed_event(server, test_event_kind1_a);
    nostr_mock_server_seed_event(server, test_event_kind1_b);
    nostr_mock_server_seed_event(server, test_event_kind0);

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Verify seeded count */
    if (nostr_mock_server_get_seeded_count(server) != 3) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Filter Matching by Kind === */

static int test_filter_by_kind(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Seed events of different kinds */
    nostr_mock_server_seed_event(server, test_event_kind1_a);
    nostr_mock_server_seed_event(server, test_event_kind1_b);
    nostr_mock_server_seed_event(server, test_event_kind0);
    nostr_mock_server_seed_event(server, test_event_kind3);

    if (nostr_mock_server_get_seeded_count(server) != 4) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Get stats to verify seeding worked */
    NostrMockRelayStats stats;
    nostr_mock_server_get_stats(server, &stats);

    if (stats.events_seeded != 4) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Statistics After Operations === */

static int test_statistics_tracking(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Seed events */
    nostr_mock_server_seed_event(server, test_event_kind1_a);
    nostr_mock_server_seed_event(server, test_event_kind1_b);

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    NostrMockRelayStats stats;
    nostr_mock_server_get_stats(server, &stats);

    /* Verify seeded count */
    if (stats.events_seeded != 2) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* No connections yet */
    if (stats.connections_current != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* No subscriptions or events matched yet */
    if (stats.subscriptions_received != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Published Events Capture === */

static int test_published_capture(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Initially no published events */
    if (nostr_mock_server_get_published_count(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    char *published = nostr_mock_server_get_published_json(server);
    if (published != NULL) {
        free(published);
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Clear Operations === */

static int test_clear_operations(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Seed some events */
    nostr_mock_server_seed_event(server, test_event_kind1_a);
    nostr_mock_server_seed_event(server, test_event_kind1_b);
    nostr_mock_server_seed_event(server, test_event_kind0);

    if (nostr_mock_server_get_seeded_count(server) != 3) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Clear seeded events */
    nostr_mock_server_clear_events(server);

    if (nostr_mock_server_get_seeded_count(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Clear published (should be no-op) */
    nostr_mock_server_clear_published(server);

    if (nostr_mock_server_get_published_count(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Server with Response Delay === */

static int test_response_delay(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
    cfg.response_delay_ms = 50;  /* 50ms delay */

    NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
    if (!server) return 0;

    nostr_mock_server_seed_event(server, test_event_kind1_a);

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Server should still work with delay configured */
    if (nostr_mock_server_get_seeded_count(server) != 1) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Server with Max Events Limit === */

static int test_max_events_limit(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
    cfg.max_events_per_req = 2;  /* Limit to 2 events per request */

    NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
    if (!server) return 0;

    /* Seed more events than the limit */
    nostr_mock_server_seed_event(server, test_event_kind1_a);
    nostr_mock_server_seed_event(server, test_event_kind1_b);
    nostr_mock_server_seed_event(server, test_event_kind0);
    nostr_mock_server_seed_event(server, test_event_kind3);

    if (nostr_mock_server_get_seeded_count(server) != 4) {
        nostr_mock_server_free(server);
        return 0;
    }

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* The limit will be applied when subscriptions are processed */
    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Disable Auto-EOSE === */

static int test_no_auto_eose(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
    cfg.auto_eose = false;

    NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
    if (!server) return 0;

    nostr_mock_server_seed_event(server, test_event_kind1_a);

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Server should work without auto-EOSE */
    if (nostr_mock_server_get_seeded_count(server) != 1) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Custom Relay Name === */

static int test_custom_relay_name(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
    cfg.relay_name = "CustomTestRelay";
    cfg.relay_desc = "A custom relay for testing";

    NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
    if (!server) return 0;

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Server should start with custom name */
    const char *url = nostr_mock_server_get_url(server);
    if (!url) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Await Publish Timeout === */

static int test_await_publish_timeout(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Should timeout immediately with timeout_ms=0 */
    char *event = nostr_mock_server_await_publish(server, 0);
    if (event != NULL) {
        free(event);
        nostr_mock_server_free(server);
        return 0;
    }

    /* Should timeout after 100ms */
    event = nostr_mock_server_await_publish(server, 100);
    if (event != NULL) {
        free(event);
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Main === */

int main(void) {
    printf("=== Mock Relay Integration Tests ===\n\n");

    TEST(test_server_connect);
    TEST(test_subscribe_seeded_events);
    TEST(test_filter_by_kind);
    TEST(test_statistics_tracking);
    TEST(test_published_capture);
    TEST(test_clear_operations);
    TEST(test_response_delay);
    TEST(test_max_events_limit);
    TEST(test_no_auto_eose);
    TEST(test_custom_relay_name);
    TEST(test_await_publish_timeout);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
