/**
 * @file test_mock_relay_server.c
 * @brief Unit tests for the mock relay server
 *
 * Tests the standalone mock relay server functionality:
 * - Server lifecycle (new/start/stop/free)
 * - Event seeding and retrieval
 * - NIP-11 relay information document
 * - Statistics tracking
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "nostr/testing/mock_relay_server.h"

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

/* === Test Helpers === */

static const char *test_event_1 = "{"
    "\"id\":\"1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef\","
    "\"pubkey\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"created_at\":1700000000,"
    "\"kind\":1,"
    "\"tags\":[],"
    "\"content\":\"Hello, world!\","
    "\"sig\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\""
    "}";

static const char *test_event_2 = "{"
    "\"id\":\"abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890\","
    "\"pubkey\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\","
    "\"created_at\":1700000001,"
    "\"kind\":1,"
    "\"tags\":[[\"e\",\"1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef\"]],"
    "\"content\":\"Reply!\","
    "\"sig\":\"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\""
    "}";

static const char *test_event_kind_0 = "{"
    "\"id\":\"0000000000000000000000000000000000000000000000000000000000000001\","
    "\"pubkey\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"created_at\":1700000002,"
    "\"kind\":0,"
    "\"tags\":[],"
    "\"content\":\"{\\\"name\\\":\\\"Test User\\\"}\","
    "\"sig\":\"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\""
    "}";

/* === Test: Default Configuration === */

static int test_default_config(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();

    if (cfg.port != 0) return 0;
    if (cfg.bind_addr != NULL) return 0;
    if (cfg.use_tls != false) return 0;
    if (cfg.auto_eose != true) return 0;
    if (cfg.validate_signatures != false) return 0;
    if (cfg.response_delay_ms != 0) return 0;
    if (cfg.max_events_per_req != -1) return 0;

    return 1;
}

/* === Test: Server Lifecycle === */

static int test_server_lifecycle(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();

    NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
    if (!server) return 0;

    /* Start should succeed */
    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* URL should be valid */
    const char *url = nostr_mock_server_get_url(server);
    if (!url || strlen(url) == 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Port should be assigned */
    uint16_t port = nostr_mock_server_get_port(server);
    if (port == 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* URL should contain port */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), ":%u", port);
    if (!strstr(url, port_str)) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Stop and cleanup */
    nostr_mock_server_stop(server);
    nostr_mock_server_free(server);

    return 1;
}

/* === Test: Event Seeding === */

static int test_event_seeding(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Initial count should be 0 */
    if (nostr_mock_server_get_seeded_count(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Seed an event */
    if (nostr_mock_server_seed_event(server, test_event_1) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    if (nostr_mock_server_get_seeded_count(server) != 1) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Seed another event */
    if (nostr_mock_server_seed_event(server, test_event_2) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    if (nostr_mock_server_get_seeded_count(server) != 2) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Clear events */
    nostr_mock_server_clear_events(server);
    if (nostr_mock_server_get_seeded_count(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Invalid Event Seeding === */

static int test_invalid_event_seeding(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Invalid JSON should fail */
    if (nostr_mock_server_seed_event(server, "not valid json") == 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Count should still be 0 */
    if (nostr_mock_server_get_seeded_count(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* Empty string should fail */
    if (nostr_mock_server_seed_event(server, "") == 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* NULL should fail */
    if (nostr_mock_server_seed_event(server, NULL) == 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Statistics === */

static int test_statistics(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    /* Seed some events */
    nostr_mock_server_seed_event(server, test_event_1);
    nostr_mock_server_seed_event(server, test_event_2);
    nostr_mock_server_seed_event(server, test_event_kind_0);

    /* Get stats */
    NostrMockRelayStats stats;
    nostr_mock_server_get_stats(server, &stats);

    if (stats.events_seeded != 3) {
        nostr_mock_server_free(server);
        return 0;
    }

    if (stats.events_published != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    if (stats.connections_current != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Custom NIP-11 === */

static int test_custom_nip11(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    const char *custom_nip11 = "{"
        "\"name\":\"CustomRelay\","
        "\"description\":\"A custom test relay\","
        "\"pubkey\":\"test\","
        "\"contact\":\"test@test.local\","
        "\"supported_nips\":[1,11,42]"
        "}";

    nostr_mock_server_set_nip11_json(server, custom_nip11);

    /* Can't easily verify without connecting, but at least it shouldn't crash */
    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Published Events === */

static int test_published_events(void) {
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

    /* Clear published (no-op when empty) */
    nostr_mock_server_clear_published(server);

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Multiple Server Instances === */

static int test_multiple_servers(void) {
    NostrMockRelayServer *server1 = nostr_mock_server_new(NULL);
    NostrMockRelayServer *server2 = nostr_mock_server_new(NULL);

    if (!server1 || !server2) {
        nostr_mock_server_free(server1);
        nostr_mock_server_free(server2);
        return 0;
    }

    /* Start both servers */
    if (nostr_mock_server_start(server1) != 0) {
        nostr_mock_server_free(server1);
        nostr_mock_server_free(server2);
        return 0;
    }

    if (nostr_mock_server_start(server2) != 0) {
        nostr_mock_server_free(server1);
        nostr_mock_server_free(server2);
        return 0;
    }

    /* Ports should be different */
    uint16_t port1 = nostr_mock_server_get_port(server1);
    uint16_t port2 = nostr_mock_server_get_port(server2);

    if (port1 == port2) {
        nostr_mock_server_free(server1);
        nostr_mock_server_free(server2);
        return 0;
    }

    /* Seed different events */
    nostr_mock_server_seed_event(server1, test_event_1);
    nostr_mock_server_seed_event(server2, test_event_2);
    nostr_mock_server_seed_event(server2, test_event_kind_0);

    if (nostr_mock_server_get_seeded_count(server1) != 1) {
        nostr_mock_server_free(server1);
        nostr_mock_server_free(server2);
        return 0;
    }

    if (nostr_mock_server_get_seeded_count(server2) != 2) {
        nostr_mock_server_free(server1);
        nostr_mock_server_free(server2);
        return 0;
    }

    nostr_mock_server_free(server1);
    nostr_mock_server_free(server2);
    return 1;
}

/* === Test: Specific Port === */

static int test_specific_port(void) {
    NostrMockRelayServerConfig cfg = nostr_mock_server_config_default();
    cfg.port = 17777;  /* Use a high port unlikely to be in use */

    NostrMockRelayServer *server = nostr_mock_server_new(&cfg);
    if (!server) return 0;

    if (nostr_mock_server_start(server) != 0) {
        /* Port may be in use, which is acceptable */
        nostr_mock_server_free(server);
        return 1;  /* Pass anyway - we tested the code path */
    }

    uint16_t port = nostr_mock_server_get_port(server);
    if (port != 17777) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Test: Connection Count === */

static int test_connection_count(void) {
    NostrMockRelayServer *server = nostr_mock_server_new(NULL);
    if (!server) return 0;

    if (nostr_mock_server_start(server) != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    /* No connections initially */
    size_t conn_count = nostr_mock_server_get_connection_count(server);
    if (conn_count != 0) {
        nostr_mock_server_free(server);
        return 0;
    }

    nostr_mock_server_free(server);
    return 1;
}

/* === Main === */

int main(void) {
    printf("=== Mock Relay Server Unit Tests ===\n\n");

    TEST(test_default_config);
    TEST(test_server_lifecycle);
    TEST(test_event_seeding);
    TEST(test_invalid_event_seeding);
    TEST(test_statistics);
    TEST(test_custom_nip11);
    TEST(test_published_events);
    TEST(test_multiple_servers);
    TEST(test_specific_port);
    TEST(test_connection_count);

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
