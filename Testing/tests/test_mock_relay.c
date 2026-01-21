/**
 * @file test_mock_relay.c
 * @brief Unit tests for the in-process mock relay
 *
 * Tests the mock relay functionality including:
 * - Basic lifecycle (create, attach, start, stop, free)
 * - Event seeding and filter matching
 * - Publication capture
 * - Response injection
 * - Fault injection
 * - Statistics tracking
 */

#include "nostr/testing/mock_relay.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "go.h"
#include "context.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        printf("  Running: %s...", #name); \
        fflush(stdout); \
        tests_run++; \
        name(); \
        tests_passed++; \
        printf(" PASSED\n"); \
    } while (0)

#define ASSERT(cond) \
    do { \
        if (!(cond)) { \
            printf(" FAILED\n"); \
            printf("    Assertion failed: %s\n", #cond); \
            printf("    at %s:%d\n", __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_TRUE(x) ASSERT((x))
#define ASSERT_FALSE(x) ASSERT(!(x))
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)

/* Helper: Create a test event */
static NostrEvent *make_test_event(int kind, const char *content, int64_t created_at) {
    NostrEvent *event = nostr_event_new();
    ASSERT_NOT_NULL(event);

    nostr_event_set_kind(event, kind);
    nostr_event_set_content(event, content);
    nostr_event_set_created_at(event, created_at);

    /* Set a dummy pubkey and id */
    nostr_event_set_pubkey(event, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

    /* Generate a simple event ID based on content hash */
    char id[65];
    snprintf(id, sizeof(id), "%064x", (unsigned int)(created_at ^ kind));
    event->id = strdup(id);

    return event;
}

/* === Lifecycle Tests === */

static void test_mock_relay_create_default(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_create_with_config(void) {
    NostrMockRelayConfig config = nostr_mock_relay_config_default();
    config.response_delay_ms = 10;
    config.auto_eose = false;
    config.validate_signatures = true;

    NostrMockRelay *mock = nostr_mock_relay_new(&config);
    ASSERT_NOT_NULL(mock);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_free_null(void) {
    /* Should not crash */
    nostr_mock_relay_free(NULL);
}

static void test_mock_relay_attach_detach(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", &err);
    ASSERT_NOT_NULL(relay);
    ASSERT_NULL(err);

    /* Connect first to create connection with channels (in test mode, no network I/O) */
    bool connected = nostr_relay_connect(relay, &err);
    ASSERT_TRUE(connected);
    ASSERT_NULL(err);

    /* Attach */
    int result = nostr_mock_relay_attach(mock, relay);
    ASSERT_EQ(result, 0);

    /* Detach */
    nostr_mock_relay_detach(mock);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

/* === Event Seeding Tests === */

static void test_mock_relay_seed_single_event(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrEvent *event = make_test_event(1, "Hello, world!", 1700000000);
    int result = nostr_mock_relay_seed_event(mock, event);
    ASSERT_EQ(result, 0);

    size_t count = nostr_mock_relay_get_seeded_count(mock);
    ASSERT_EQ(count, 1);

    nostr_event_free(event);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_seed_multiple_events(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrEvent *events[3];
    events[0] = make_test_event(1, "Event 1", 1700000000);
    events[1] = make_test_event(1, "Event 2", 1700000001);
    events[2] = make_test_event(1, "Event 3", 1700000002);

    int result = nostr_mock_relay_seed_events(mock, events, 3);
    ASSERT_EQ(result, 0);

    size_t count = nostr_mock_relay_get_seeded_count(mock);
    ASSERT_EQ(count, 3);

    for (int i = 0; i < 3; i++) {
        nostr_event_free(events[i]);
    }
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_clear_events(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrEvent *event = make_test_event(1, "Test", 1700000000);
    nostr_mock_relay_seed_event(mock, event);
    ASSERT_EQ(nostr_mock_relay_get_seeded_count(mock), 1);

    nostr_mock_relay_clear_events(mock);
    ASSERT_EQ(nostr_mock_relay_get_seeded_count(mock), 0);

    nostr_event_free(event);
    nostr_mock_relay_free(mock);
}

/* === Statistics Tests === */

static void test_mock_relay_stats_initial(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrMockRelayStats stats;
    nostr_mock_relay_get_stats(mock, &stats);

    ASSERT_EQ(stats.events_seeded, 0);
    ASSERT_EQ(stats.events_matched, 0);
    ASSERT_EQ(stats.events_published, 0);
    ASSERT_EQ(stats.subscriptions_received, 0);
    ASSERT_EQ(stats.close_received, 0);
    ASSERT_EQ(stats.faults_triggered, 0);

    nostr_mock_relay_free(mock);
}

static void test_mock_relay_stats_after_seeding(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrEvent *event = make_test_event(1, "Test", 1700000000);
    nostr_mock_relay_seed_event(mock, event);

    NostrMockRelayStats stats;
    nostr_mock_relay_get_stats(mock, &stats);
    ASSERT_EQ(stats.events_seeded, 1);

    nostr_event_free(event);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_reset_stats(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrEvent *event = make_test_event(1, "Test", 1700000000);
    nostr_mock_relay_seed_event(mock, event);

    nostr_mock_relay_reset_stats(mock);

    NostrMockRelayStats stats;
    nostr_mock_relay_get_stats(mock, &stats);
    ASSERT_EQ(stats.events_seeded, 0);

    nostr_event_free(event);
    nostr_mock_relay_free(mock);
}

/* === Fault Injection Tests === */

static void test_mock_relay_fault_none_initial(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    NostrMockFaultType fault = nostr_mock_relay_get_fault(mock);
    ASSERT_EQ(fault, MOCK_FAULT_NONE);

    nostr_mock_relay_free(mock);
}

static void test_mock_relay_set_clear_fault(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    nostr_mock_relay_set_fault(mock, MOCK_FAULT_DISCONNECT, 5);
    ASSERT_EQ(nostr_mock_relay_get_fault(mock), MOCK_FAULT_DISCONNECT);

    nostr_mock_relay_clear_fault(mock);
    ASSERT_EQ(nostr_mock_relay_get_fault(mock), MOCK_FAULT_NONE);

    nostr_mock_relay_free(mock);
}

/* === Publication Capture Tests === */

static void test_mock_relay_published_empty_initial(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    size_t count = nostr_mock_relay_get_published_count(mock);
    ASSERT_EQ(count, 0);

    size_t out_count = 0;
    const NostrEvent **events = nostr_mock_relay_get_published(mock, &out_count);
    ASSERT_NULL(events);
    ASSERT_EQ(out_count, 0);

    nostr_mock_relay_free(mock);
}

static void test_mock_relay_clear_published(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    /* Clear should work even when empty */
    nostr_mock_relay_clear_published(mock);
    ASSERT_EQ(nostr_mock_relay_get_published_count(mock), 0);

    nostr_mock_relay_free(mock);
}

/* === Response Injection Tests === */

static void test_mock_relay_inject_notice(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    ASSERT_NOT_NULL(relay);

    /* Connect first to create channels */
    nostr_relay_connect(relay, NULL);
    nostr_mock_relay_attach(mock, relay);

    /* Inject notice - should succeed when attached */
    int result = nostr_mock_relay_inject_notice(mock, "Test notice");
    ASSERT_EQ(result, 0);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_inject_ok(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    ASSERT_NOT_NULL(relay);

    /* Connect first to create channels */
    nostr_relay_connect(relay, NULL);
    nostr_mock_relay_attach(mock, relay);

    /* Inject OK */
    int result = nostr_mock_relay_inject_ok(mock, "abc123", true, NULL);
    ASSERT_EQ(result, 0);

    result = nostr_mock_relay_inject_ok(mock, "def456", false, "duplicate:");
    ASSERT_EQ(result, 0);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_inject_eose(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    ASSERT_NOT_NULL(relay);

    /* Connect first to create channels */
    nostr_relay_connect(relay, NULL);
    nostr_mock_relay_attach(mock, relay);

    int result = nostr_mock_relay_inject_eose(mock, "sub123");
    ASSERT_EQ(result, 0);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_inject_closed(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    ASSERT_NOT_NULL(relay);

    /* Connect first to create channels */
    nostr_relay_connect(relay, NULL);
    nostr_mock_relay_attach(mock, relay);

    int result = nostr_mock_relay_inject_closed(mock, "sub123", "auth-required:");
    ASSERT_EQ(result, 0);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_inject_auth(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    ASSERT_NOT_NULL(relay);

    /* Connect first to create channels */
    nostr_relay_connect(relay, NULL);
    nostr_mock_relay_attach(mock, relay);

    int result = nostr_mock_relay_inject_auth(mock, "challenge-string-123");
    ASSERT_EQ(result, 0);

    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

static void test_mock_relay_inject_event(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    ASSERT_NOT_NULL(relay);

    /* Connect first to create channels */
    nostr_relay_connect(relay, NULL);
    nostr_mock_relay_attach(mock, relay);

    NostrEvent *event = make_test_event(1, "Injected event", 1700000000);
    int result = nostr_mock_relay_inject_event(mock, "sub123", event);
    ASSERT_EQ(result, 0);

    nostr_event_free(event);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);
    nostr_mock_relay_free(mock);
}

/* === Subscription Tracking Tests === */

static void test_mock_relay_subscription_count_initial(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    size_t count = nostr_mock_relay_get_subscription_count(mock);
    ASSERT_EQ(count, 0);

    nostr_mock_relay_free(mock);
}

static void test_mock_relay_has_subscription_false(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    bool has = nostr_mock_relay_has_subscription(mock, "nonexistent");
    ASSERT_FALSE(has);

    nostr_mock_relay_free(mock);
}

/* === Configuration Tests === */

static void test_mock_relay_config_default(void) {
    NostrMockRelayConfig config = nostr_mock_relay_config_default();

    ASSERT_EQ(config.response_delay_ms, 0);
    ASSERT_EQ(config.max_events_per_req, -1);
    ASSERT_TRUE(config.auto_eose);
    ASSERT_FALSE(config.validate_signatures);
    ASSERT_FALSE(config.simulate_auth);
    ASSERT_NULL(config.auth_challenge);
}

/* === Integration Tests === */

static void test_mock_relay_full_lifecycle(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    /* Create mock with custom config */
    NostrMockRelayConfig config = nostr_mock_relay_config_default();
    config.auto_eose = true;
    NostrMockRelay *mock = nostr_mock_relay_new(&config);
    ASSERT_NOT_NULL(mock);

    /* Seed some events */
    NostrEvent *event1 = make_test_event(1, "First event", 1700000000);
    NostrEvent *event2 = make_test_event(1, "Second event", 1700000001);
    nostr_mock_relay_seed_event(mock, event1);
    nostr_mock_relay_seed_event(mock, event2);
    ASSERT_EQ(nostr_mock_relay_get_seeded_count(mock), 2);

    /* Create relay */
    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", &err);
    ASSERT_NOT_NULL(relay);
    ASSERT_NULL(err);

    /* Connect relay first (creates connection with channels in test mode) */
    bool connected = nostr_relay_connect(relay, &err);
    ASSERT_TRUE(connected);

    /* Attach and start mock */
    ASSERT_EQ(nostr_mock_relay_attach(mock, relay), 0);
    ASSERT_EQ(nostr_mock_relay_start(mock), 0);

    /* Give the relay time to start */
    usleep(50000);  /* 50ms */

    /* Stop and cleanup */
    nostr_mock_relay_stop(mock);
    nostr_relay_close(relay, NULL);
    nostr_relay_free(relay);
    go_context_free(ctx);

    nostr_event_free(event1);
    nostr_event_free(event2);
    nostr_mock_relay_free(mock);
}

/* === Await Publish Tests === */

static void test_mock_relay_await_publish_timeout(void) {
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    ASSERT_NOT_NULL(mock);

    /* Should timeout immediately with 0 timeout */
    const NostrEvent *event = nostr_mock_relay_await_publish(mock, 0);
    ASSERT_NULL(event);

    /* Should timeout after short wait */
    event = nostr_mock_relay_await_publish(mock, 10);
    ASSERT_NULL(event);

    nostr_mock_relay_free(mock);
}

/* === Main === */

int main(void) {
    printf("\n=== Mock Relay Unit Tests ===\n\n");

    /* Lifecycle tests */
    printf("Lifecycle Tests:\n");
    TEST(test_mock_relay_create_default);
    TEST(test_mock_relay_create_with_config);
    TEST(test_mock_relay_free_null);
    TEST(test_mock_relay_attach_detach);

    /* Event seeding tests */
    printf("\nEvent Seeding Tests:\n");
    TEST(test_mock_relay_seed_single_event);
    TEST(test_mock_relay_seed_multiple_events);
    TEST(test_mock_relay_clear_events);

    /* Statistics tests */
    printf("\nStatistics Tests:\n");
    TEST(test_mock_relay_stats_initial);
    TEST(test_mock_relay_stats_after_seeding);
    TEST(test_mock_relay_reset_stats);

    /* Fault injection tests */
    printf("\nFault Injection Tests:\n");
    TEST(test_mock_relay_fault_none_initial);
    TEST(test_mock_relay_set_clear_fault);

    /* Publication capture tests */
    printf("\nPublication Capture Tests:\n");
    TEST(test_mock_relay_published_empty_initial);
    TEST(test_mock_relay_clear_published);
    TEST(test_mock_relay_await_publish_timeout);

    /* Response injection tests */
    printf("\nResponse Injection Tests:\n");
    TEST(test_mock_relay_inject_notice);
    TEST(test_mock_relay_inject_ok);
    TEST(test_mock_relay_inject_eose);
    TEST(test_mock_relay_inject_closed);
    TEST(test_mock_relay_inject_auth);
    TEST(test_mock_relay_inject_event);

    /* Subscription tracking tests */
    printf("\nSubscription Tracking Tests:\n");
    TEST(test_mock_relay_subscription_count_initial);
    TEST(test_mock_relay_has_subscription_false);

    /* Configuration tests */
    printf("\nConfiguration Tests:\n");
    TEST(test_mock_relay_config_default);

    /* Integration tests */
    printf("\nIntegration Tests:\n");
    TEST(test_mock_relay_full_lifecycle);

    printf("\n=== Results: %d/%d tests passed ===\n\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
