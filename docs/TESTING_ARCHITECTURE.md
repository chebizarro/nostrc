# Mock Relay Testing Architecture for nostrc

## Overview

This document describes the architecture for testing nostrc without requiring real network relays. The testing framework consists of three main components:

1. **In-Process Mock Relay** - For fast unit tests with complete control
2. **Standalone Mock Relay Server** - For integration and end-to-end tests
3. **Test Harness Utilities** - Shared helpers for event generation, assertion, and scenario replay

## Design Principles

- **Minimal Dependencies**: Use existing nostrc primitives (GoChannel, NostrEnvelope, NostrEvent)
- **No Network I/O**: Tests run offline, enabling CI without relay infrastructure
- **Deterministic**: Reproducible tests with seeded event sequences
- **Error Simulation**: Support timeouts, disconnects, invalid signatures, rate limiting
- **CMake/CTest Integration**: Standard test registration and parallel execution

---

## Component 1: In-Process Mock Relay

### Architecture

The in-process mock relay replaces the WebSocket connection layer with direct channel-based message passing. This leverages the existing `NOSTR_TEST_MODE` environment variable that already bypasses libwebsockets.

```
+------------------+          +------------------+
|   Test Code      |          |   MockRelay      |
|                  |          |                  |
|  NostrRelay*     |<-------->|  event_store[]   |
|  nostr_relay_    |  GoChannel|  filter_engine   |
|  connect()       |          |  response_queue  |
+------------------+          +------------------+
         |                            |
         |  recv_channel              |
         |<---------------------------+
         |  send_channel              |
         +--------------------------->|
```

### Header: `testing/mock_relay.h`

```c
#ifndef NOSTR_MOCK_RELAY_H
#define NOSTR_MOCK_RELAY_H

#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-envelope.h"
#include "channel.h"
#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque mock relay handle */
typedef struct NostrMockRelay NostrMockRelay;

/* Configuration for mock relay behavior */
typedef struct {
    int response_delay_ms;      /* Artificial delay before responses (0 = immediate) */
    int max_events_per_req;     /* Limit on events returned per REQ (-1 = unlimited) */
    bool auto_eose;             /* Automatically send EOSE after seeded events */
    bool validate_signatures;   /* Reject events with invalid signatures */
    bool simulate_auth;         /* Send AUTH challenge on connect */
    const char *auth_challenge; /* Custom AUTH challenge string */
} NostrMockRelayConfig;

/* Default configuration (immediate responses, auto EOSE, no auth) */
NostrMockRelayConfig nostr_mock_relay_config_default(void);

/* === Lifecycle === */

/**
 * nostr_mock_relay_new:
 * Create a new mock relay instance.
 *
 * @config: Configuration options (NULL for defaults)
 * Returns: New mock relay instance
 */
NostrMockRelay *nostr_mock_relay_new(const NostrMockRelayConfig *config);

/**
 * nostr_mock_relay_free:
 * Free mock relay and all seeded events.
 */
void nostr_mock_relay_free(NostrMockRelay *mock);

/* === Integration with NostrRelay === */

/**
 * nostr_mock_relay_attach:
 * Attach mock relay to a NostrRelay's channels.
 * Must be called after nostr_relay_new() but before nostr_relay_connect().
 *
 * @mock: Mock relay instance
 * @relay: Real NostrRelay instance (created with NOSTR_TEST_MODE=1)
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_attach(NostrMockRelay *mock, NostrRelay *relay);

/**
 * nostr_mock_relay_detach:
 * Detach mock relay from NostrRelay.
 */
void nostr_mock_relay_detach(NostrMockRelay *mock);

/* === Event Seeding === */

/**
 * nostr_mock_relay_seed_event:
 * Add an event to the mock relay's store.
 * Events are returned when a subscription's filter matches.
 *
 * @mock: Mock relay instance
 * @event: Event to seed (mock takes ownership)
 * Returns: 0 on success
 */
int nostr_mock_relay_seed_event(NostrMockRelay *mock, NostrEvent *event);

/**
 * nostr_mock_relay_seed_events:
 * Add multiple events to the store.
 *
 * @mock: Mock relay instance
 * @events: Array of events (mock takes ownership of each)
 * @count: Number of events
 * Returns: 0 on success
 */
int nostr_mock_relay_seed_events(NostrMockRelay *mock, NostrEvent **events, size_t count);

/**
 * nostr_mock_relay_seed_from_json:
 * Load events from a JSON array file.
 *
 * @mock: Mock relay instance
 * @json_path: Path to JSON file containing event array
 * Returns: Number of events loaded, -1 on error
 */
int nostr_mock_relay_seed_from_json(NostrMockRelay *mock, const char *json_path);

/**
 * nostr_mock_relay_clear_events:
 * Remove all seeded events.
 */
void nostr_mock_relay_clear_events(NostrMockRelay *mock);

/* === Publication Capture === */

/**
 * nostr_mock_relay_get_published:
 * Get events published by clients to this mock relay.
 *
 * @mock: Mock relay instance
 * @count: (out) Number of published events
 * Returns: Array of published events (caller must not free)
 */
const NostrEvent **nostr_mock_relay_get_published(NostrMockRelay *mock, size_t *count);

/**
 * nostr_mock_relay_await_publish:
 * Block until an event is published or timeout.
 *
 * @mock: Mock relay instance
 * @timeout_ms: Timeout in milliseconds (0 = no wait, -1 = indefinite)
 * Returns: Published event (mock retains ownership), NULL on timeout
 */
const NostrEvent *nostr_mock_relay_await_publish(NostrMockRelay *mock, int timeout_ms);

/**
 * nostr_mock_relay_clear_published:
 * Clear captured publications.
 */
void nostr_mock_relay_clear_published(NostrMockRelay *mock);

/* === Response Injection === */

/**
 * nostr_mock_relay_inject_notice:
 * Send a NOTICE message to the connected client.
 */
int nostr_mock_relay_inject_notice(NostrMockRelay *mock, const char *message);

/**
 * nostr_mock_relay_inject_ok:
 * Send an OK response for an event ID.
 */
int nostr_mock_relay_inject_ok(NostrMockRelay *mock, const char *event_id, bool ok, const char *reason);

/**
 * nostr_mock_relay_inject_closed:
 * Send a CLOSED message for a subscription.
 */
int nostr_mock_relay_inject_closed(NostrMockRelay *mock, const char *sub_id, const char *reason);

/**
 * nostr_mock_relay_inject_auth:
 * Send an AUTH challenge.
 */
int nostr_mock_relay_inject_auth(NostrMockRelay *mock, const char *challenge);

/* === Error Simulation === */

typedef enum {
    MOCK_FAULT_NONE = 0,
    MOCK_FAULT_DISCONNECT,       /* Simulate connection drop */
    MOCK_FAULT_TIMEOUT,          /* Stop responding (for timeout tests) */
    MOCK_FAULT_INVALID_JSON,     /* Send malformed JSON */
    MOCK_FAULT_RATE_LIMIT,       /* Return rate-limit CLOSED messages */
    MOCK_FAULT_AUTH_REQUIRED,    /* Require AUTH before accepting subscriptions */
} NostrMockFaultType;

/**
 * nostr_mock_relay_set_fault:
 * Configure fault injection.
 *
 * @mock: Mock relay instance
 * @fault: Fault type to inject
 * @after_n: Trigger fault after N successful operations (0 = immediate)
 */
void nostr_mock_relay_set_fault(NostrMockRelay *mock, NostrMockFaultType fault, int after_n);

/**
 * nostr_mock_relay_clear_fault:
 * Remove fault injection.
 */
void nostr_mock_relay_clear_fault(NostrMockRelay *mock);

/* === Statistics === */

typedef struct {
    size_t events_seeded;
    size_t events_matched;
    size_t events_published;
    size_t subscriptions_received;
    size_t close_received;
    size_t faults_triggered;
} NostrMockRelayStats;

/**
 * nostr_mock_relay_get_stats:
 * Get mock relay statistics.
 */
void nostr_mock_relay_get_stats(NostrMockRelay *mock, NostrMockRelayStats *stats);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_MOCK_RELAY_H */
```

### Implementation Strategy

The mock relay runs a goroutine (via `go()`) that:

1. Reads from the relay's `send_channel` (messages the client sends to relay)
2. Parses envelopes using `nostr_envelope_parse()`
3. For REQ envelopes: matches seeded events against filters, sends matching EVENTs, then EOSE
4. For EVENT envelopes: captures the event and optionally sends OK
5. For CLOSE envelopes: cleans up subscription state
6. Writes responses to the relay's `recv_channel`

```c
static void *mock_relay_loop(void *arg) {
    NostrMockRelay *mock = (NostrMockRelay *)arg;

    while (!mock->shutdown) {
        WebSocketMessage *msg = NULL;
        GoSelectCase cases[] = {
            { .op = GO_SELECT_RECEIVE, .chan = mock->relay->connection->send_channel, .recv_buf = (void**)&msg },
            { .op = GO_SELECT_RECEIVE, .chan = mock->shutdown_chan, .recv_buf = NULL },
        };

        int idx = go_select(cases, 2);
        if (idx == 1 || !msg) break;  // shutdown

        NostrEnvelope *env = nostr_envelope_parse(msg->data);
        if (!env) continue;

        switch (env->type) {
            case NOSTR_ENVELOPE_REQ:
                handle_req(mock, (NostrReqEnvelope *)env);
                break;
            case NOSTR_ENVELOPE_EVENT:
                handle_event(mock, (NostrEventEnvelope *)env);
                break;
            case NOSTR_ENVELOPE_CLOSE:
                handle_close(mock, (NostrCloseEnvelope *)env);
                break;
            // ...
        }

        nostr_envelope_free(env);
        free(msg->data);
        free(msg);
    }

    return NULL;
}
```

### Filter Matching

Use the existing `nostr_filters_match()` function from libnostr to determine which seeded events match a subscription's filters.

---

## Component 2: Standalone Mock Relay Server

### Purpose

For integration tests that need to exercise the full WebSocket stack, or for testing multiple clients against a shared relay.

### Architecture

```
+------------------+     WebSocket     +------------------+
|   Test Client    |<----------------->|  Mock Relay      |
|   (nostrc)       |    localhost:7777 |  Server          |
+------------------+                   +------------------+
                                              |
                                              v
                                       +------------------+
                                       |  Event Store     |
                                       |  (memory/file)   |
                                       +------------------+
```

### Header: `testing/mock_relay_server.h`

```c
#ifndef NOSTR_MOCK_RELAY_SERVER_H
#define NOSTR_MOCK_RELAY_SERVER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NostrMockRelayServer NostrMockRelayServer;

typedef struct {
    uint16_t port;              /* TCP port (0 = auto-assign) */
    const char *bind_addr;      /* Bind address (NULL = "127.0.0.1") */
    bool use_tls;               /* Enable WSS (requires cert/key) */
    const char *cert_path;      /* TLS certificate path */
    const char *key_path;       /* TLS private key path */
    const char *seed_file;      /* JSON file to pre-seed events */
} NostrMockRelayServerConfig;

/**
 * nostr_mock_server_new:
 * Create and start a mock relay server.
 *
 * @config: Server configuration
 * Returns: Server instance, NULL on error
 */
NostrMockRelayServer *nostr_mock_server_new(const NostrMockRelayServerConfig *config);

/**
 * nostr_mock_server_get_url:
 * Get the WebSocket URL for connecting to this server.
 *
 * Returns: URL string like "ws://127.0.0.1:7777" (owned by server)
 */
const char *nostr_mock_server_get_url(NostrMockRelayServer *server);

/**
 * nostr_mock_server_get_port:
 * Get the actual port (useful when port=0 was specified).
 */
uint16_t nostr_mock_server_get_port(NostrMockRelayServer *server);

/**
 * nostr_mock_server_seed_event:
 * Add an event to the server's store.
 */
int nostr_mock_server_seed_event(NostrMockRelayServer *server, const char *event_json);

/**
 * nostr_mock_server_get_published_json:
 * Get all published events as a JSON array string.
 *
 * Returns: JSON string (caller must free)
 */
char *nostr_mock_server_get_published_json(NostrMockRelayServer *server);

/**
 * nostr_mock_server_stop:
 * Stop the server and free resources.
 */
void nostr_mock_server_stop(NostrMockRelayServer *server);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_MOCK_RELAY_SERVER_H */
```

### Implementation Notes

- Built on libwebsockets (already a dependency)
- Runs in a separate thread, same process as the test
- Reuses the in-process mock relay's event matching logic
- Port 0 enables automatic port assignment to avoid conflicts in parallel tests

---

## Component 3: Test Harness Utilities

### Header: `testing/test_harness.h`

```c
#ifndef NOSTR_TEST_HARNESS_H
#define NOSTR_TEST_HARNESS_H

#include "nostr-event.h"
#include "nostr-filter.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Event Generation === */

/**
 * nostr_test_make_text_note:
 * Create a kind-1 text note with random id/pubkey.
 *
 * @content: Note content
 * @created_at: Timestamp (0 = now)
 * Returns: New event (caller must free)
 */
NostrEvent *nostr_test_make_text_note(const char *content, int64_t created_at);

/**
 * nostr_test_make_signed_event:
 * Create and sign an event with the given private key.
 *
 * @kind: Event kind
 * @content: Event content
 * @privkey_hex: 64-char hex private key
 * @tags: Optional tags (ownership transferred)
 * Returns: Signed event (caller must free)
 */
NostrEvent *nostr_test_make_signed_event(int kind, const char *content,
                                          const char *privkey_hex, NostrTags *tags);

/**
 * nostr_test_generate_events:
 * Generate N random events matching a pattern.
 *
 * @count: Number of events to generate
 * @kind: Event kind (-1 = random)
 * @pubkey_hex: Fixed pubkey (NULL = random)
 * @time_start: Start timestamp
 * @time_step: Seconds between events
 * Returns: Array of events (caller must free array and events)
 */
NostrEvent **nostr_test_generate_events(size_t count, int kind,
                                         const char *pubkey_hex,
                                         int64_t time_start, int64_t time_step);

/* === Test Keys === */

typedef struct {
    char privkey_hex[65];  /* 64 hex chars + null */
    char pubkey_hex[65];   /* 64 hex chars + null */
    unsigned char privkey[32];
    unsigned char pubkey[32];
} NostrTestKeypair;

/**
 * nostr_test_generate_keypair:
 * Generate a random keypair for testing.
 */
void nostr_test_generate_keypair(NostrTestKeypair *kp);

/**
 * nostr_test_keypair_from_seed:
 * Generate deterministic keypair from seed (for reproducible tests).
 */
void nostr_test_keypair_from_seed(NostrTestKeypair *kp, uint32_t seed);

/* Well-known test keypairs (deterministic) */
extern const NostrTestKeypair NOSTR_TEST_ALICE;
extern const NostrTestKeypair NOSTR_TEST_BOB;
extern const NostrTestKeypair NOSTR_TEST_CAROL;

/* === Assertions === */

/**
 * nostr_test_assert_event_matches:
 * Assert that an event matches a filter.
 */
void nostr_test_assert_event_matches(const NostrEvent *event, const NostrFilter *filter,
                                      const char *file, int line);

/**
 * nostr_test_assert_event_equals:
 * Assert two events are equivalent (same id, content, etc).
 */
void nostr_test_assert_event_equals(const NostrEvent *a, const NostrEvent *b,
                                     const char *file, int line);

/**
 * nostr_test_assert_signature_valid:
 * Assert event has valid signature.
 */
void nostr_test_assert_signature_valid(const NostrEvent *event,
                                        const char *file, int line);

/* Convenience macros */
#define NOSTR_ASSERT_EVENT_MATCHES(ev, f) \
    nostr_test_assert_event_matches(ev, f, __FILE__, __LINE__)

#define NOSTR_ASSERT_EVENT_EQUALS(a, b) \
    nostr_test_assert_event_equals(a, b, __FILE__, __LINE__)

#define NOSTR_ASSERT_SIG_VALID(ev) \
    nostr_test_assert_signature_valid(ev, __FILE__, __LINE__)

/* === Scenario Replay === */

typedef struct NostrTestScenario NostrTestScenario;

/**
 * nostr_test_scenario_load:
 * Load a test scenario from JSON file.
 *
 * Scenario format:
 * {
 *   "seed_events": [...],
 *   "steps": [
 *     {"action": "subscribe", "filters": [...]},
 *     {"action": "expect_events", "count": 5, "timeout_ms": 1000},
 *     {"action": "publish", "event": {...}},
 *     {"action": "inject_fault", "type": "disconnect"},
 *     ...
 *   ]
 * }
 */
NostrTestScenario *nostr_test_scenario_load(const char *json_path);

/**
 * nostr_test_scenario_run:
 * Execute a scenario against a mock relay.
 *
 * Returns: 0 on success, -1 on failure (check logs)
 */
int nostr_test_scenario_run(NostrTestScenario *scenario, NostrMockRelay *mock);

void nostr_test_scenario_free(NostrTestScenario *scenario);

/* === Timing Utilities === */

/**
 * nostr_test_wait_condition:
 * Wait for a condition to become true.
 *
 * @check: Function returning true when condition is met
 * @ctx: User context for check function
 * @timeout_ms: Maximum wait time
 * Returns: true if condition met, false on timeout
 */
typedef bool (*NostrTestCondition)(void *ctx);
bool nostr_test_wait_condition(NostrTestCondition check, void *ctx, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_TEST_HARNESS_H */
```

---

## CMake Integration

### Directory Structure

```
testing/
    CMakeLists.txt
    mock_relay.h
    mock_relay.c
    mock_relay_server.h
    mock_relay_server.c
    test_harness.h
    test_harness.c
    scenarios/
        basic_subscribe.json
        pagination.json
        error_recovery.json
```

### CMakeLists.txt for Testing Library

```cmake
# testing/CMakeLists.txt

add_library(nostr_testing STATIC
    mock_relay.c
    mock_relay_server.c
    test_harness.c
)

target_include_directories(nostr_testing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nostr_testing PUBLIC
    libnostr
    nostr_json
    libgo
    ${NSYNC_LIB}
)

# Export for tests in other directories
set(NOSTR_TESTING_LIBRARY nostr_testing PARENT_SCOPE)
set(NOSTR_TESTING_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
```

### Integration with Root CMakeLists.txt

```cmake
# In root CMakeLists.txt, after libnostr/libjson:

# Build testing library
add_subdirectory(testing)

# Tests can link against nostr_testing
```

### Test Registration Pattern

```cmake
# In tests/CMakeLists.txt or nips/*/CMakeLists.txt:

add_executable(test_mock_relay_basic test_mock_relay_basic.c)
target_link_libraries(test_mock_relay_basic PRIVATE
    nostr_testing
    libnostr
    ${NSYNC_LIB}
)
target_include_directories(test_mock_relay_basic PRIVATE
    ${NOSTR_TESTING_INCLUDE_DIR}
)
apply_sanitizers(test_mock_relay_basic)

add_test(NAME MockRelayBasic COMMAND test_mock_relay_basic)
set_tests_properties(MockRelayBasic PROPERTIES
    TIMEOUT 30
    LABELS "mock;unit"
)
```

### Test Labels for Selective Execution

```bash
# Run only mock relay tests
ctest -L mock

# Run only unit tests (no network)
ctest -L unit

# Run integration tests (may use standalone server)
ctest -L integration

# Exclude slow tests
ctest -LE slow
```

---

## Example Test Cases

### Basic Subscription Test

```c
#include "testing/mock_relay.h"
#include "testing/test_harness.h"
#include "nostr-relay.h"
#include "nostr-filter.h"
#include <assert.h>

void test_basic_subscribe_receives_seeded_events(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    // Create mock relay
    NostrMockRelay *mock = nostr_mock_relay_new(NULL);

    // Seed events
    NostrEvent *ev1 = nostr_test_make_text_note("Hello", 1700000000);
    NostrEvent *ev2 = nostr_test_make_text_note("World", 1700000001);
    nostr_mock_relay_seed_event(mock, ev1);
    nostr_mock_relay_seed_event(mock, ev2);

    // Create and connect relay
    GoContext *ctx = go_context_background();
    Error *err = NULL;
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", &err);
    assert(relay && !err);

    nostr_mock_relay_attach(mock, relay);
    assert(nostr_relay_connect(relay, &err));

    // Subscribe with filter
    NostrFilter *f = nostr_filter_new();
    int kinds[] = {1};
    nostr_filter_set_kinds(f, kinds, 1);

    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, f);

    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
    assert(nostr_subscription_fire(sub, &err));

    // Receive events
    int count = 0;
    NostrEvent *received = NULL;
    GoContext *timeout_ctx = ctx_with_timeout_ms(1000);

    while (go_channel_receive_with_context(sub->events, (void**)&received, timeout_ctx) == 0) {
        assert(received->kind == 1);
        nostr_event_free(received);
        count++;
    }

    assert(count == 2);

    // Cleanup
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    nostr_mock_relay_free(mock);
    go_context_free(ctx);
}
```

### Publish Capture Test

```c
void test_publish_captured_by_mock(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);
    GoContext *ctx = go_context_background();
    Error *err = NULL;

    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", &err);
    nostr_mock_relay_attach(mock, relay);
    nostr_relay_connect(relay, &err);

    // Publish an event
    NostrEvent *ev = nostr_test_make_signed_event(1, "Test",
        NOSTR_TEST_ALICE.privkey_hex, NULL);
    nostr_relay_publish(relay, ev);

    // Wait for mock to capture it
    const NostrEvent *captured = nostr_mock_relay_await_publish(mock, 1000);
    assert(captured != NULL);
    assert(captured->kind == 1);
    assert(strcmp(captured->content, "Test") == 0);

    nostr_event_free(ev);
    nostr_relay_free(relay);
    nostr_mock_relay_free(mock);
}
```

### Error Simulation Test

```c
void test_disconnect_recovery(void) {
    setenv("NOSTR_TEST_MODE", "1", 1);

    NostrMockRelay *mock = nostr_mock_relay_new(NULL);

    // Disconnect after 2 events
    nostr_mock_relay_set_fault(mock, MOCK_FAULT_DISCONNECT, 2);

    // Seed 5 events
    NostrEvent **events = nostr_test_generate_events(5, 1, NULL, 1700000000, 1);
    nostr_mock_relay_seed_events(mock, events, 5);

    GoContext *ctx = go_context_background();
    NostrRelay *relay = nostr_relay_new(ctx, "wss://mock.test", NULL);
    nostr_mock_relay_attach(mock, relay);
    nostr_relay_connect(relay, NULL);

    // Subscribe
    NostrFilters *filters = nostr_filters_new();
    nostr_filters_add(filters, nostr_filter_new());
    NostrSubscription *sub = nostr_relay_prepare_subscription(relay, ctx, filters);
    nostr_subscription_fire(sub, NULL);

    // Should receive 2 events then disconnect
    int count = 0;
    NostrEvent *ev;
    GoContext *to = ctx_with_timeout_ms(500);
    while (go_channel_receive_with_context(sub->events, (void**)&ev, to) == 0) {
        nostr_event_free(ev);
        count++;
    }

    assert(count == 2);
    assert(!nostr_relay_is_connected(relay));

    // Cleanup
    nostr_subscription_free(sub);
    nostr_filters_free(filters);
    nostr_relay_free(relay);
    nostr_mock_relay_free(mock);
    free(events);
}
```

---

## Implementation Roadmap

### Phase 1: In-Process Mock Relay (Week 1-2)

1. Create `testing/` directory structure
2. Implement `mock_relay.c` core loop
3. Implement event seeding and filter matching
4. Implement publication capture
5. Add basic fault injection
6. Write unit tests for the mock itself

### Phase 2: Test Harness Utilities (Week 2-3)

1. Implement event generation helpers
2. Add test keypairs and signing
3. Implement assertion macros
4. Create scenario loader and runner
5. Document scenario JSON format

### Phase 3: CMake Integration (Week 3)

1. Create `testing/CMakeLists.txt`
2. Add test labels and categories
3. Update CI configuration
4. Migrate existing NOSTR_TEST_MODE tests to use mock

### Phase 4: Standalone Server (Week 4)

1. Implement WebSocket server using libwebsockets
2. Wire in mock relay logic
3. Add TLS support
4. Create integration test examples

### Phase 5: NIP-Specific Test Suites (Ongoing)

1. NIP-01: Basic protocol tests
2. NIP-42: AUTH flow tests
3. NIP-45: COUNT tests
4. NIP-77: Negentropy sync tests

---

## Appendix: Comparison with Existing Test Patterns

### Current Pattern (NOSTR_TEST_MODE)

The existing `NOSTR_TEST_MODE=1` environment variable:
- Bypasses libwebsockets connection
- Creates dummy channels
- Does NOT simulate relay behavior

### New Pattern (Mock Relay)

The mock relay:
- Also uses `NOSTR_TEST_MODE=1` internally
- Actively responds to subscriptions
- Returns seeded events
- Captures publications
- Simulates error conditions

### Migration Path

Existing tests using `NOSTR_TEST_MODE`:
```c
// Before: Just checks no crash
setenv("NOSTR_TEST_MODE", "1", 1);
NostrRelay *relay = nostr_relay_new(ctx, "wss://x", &err);
nostr_relay_connect(relay, &err);
// ... operations that don't receive responses

// After: Full round-trip testing
setenv("NOSTR_TEST_MODE", "1", 1);
NostrMockRelay *mock = nostr_mock_relay_new(NULL);
nostr_mock_relay_seed_event(mock, test_event);
NostrRelay *relay = nostr_relay_new(ctx, "wss://x", &err);
nostr_mock_relay_attach(mock, relay);
nostr_relay_connect(relay, &err);
// ... operations that receive mock responses
```

---

## References

- [NIP-01: Basic Protocol](https://github.com/nostr-protocol/nips/blob/master/01.md)
- [libwebsockets Documentation](https://libwebsockets.org/lws-api-doc-main/html/)
- [nostrc ARCHITECTURE.md](/Users/bizarro/gt/nostrc/refinery/rig/ARCHITECTURE.md)
- [Existing test patterns](/Users/bizarro/gt/nostrc/refinery/rig/tests/)
