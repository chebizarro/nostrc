/**
 * @file mock_relay.h
 * @brief In-process mock relay for unit tests
 *
 * This provides a mock Nostr relay that runs in-process without network I/O.
 * It uses GoChannel message passing to communicate with NostrRelay instances
 * that have been created with NOSTR_TEST_MODE=1.
 */
#ifndef NOSTR_MOCK_RELAY_H
#define NOSTR_MOCK_RELAY_H

#include "nostr-relay.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-envelope.h"
#include "channel.h"
#include "context.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

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

/**
 * nostr_mock_relay_config_default:
 * Get default configuration (immediate responses, auto EOSE, no auth).
 *
 * Returns: Default configuration struct
 */
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
 *
 * @mock: Mock relay instance (safe on NULL)
 */
void nostr_mock_relay_free(NostrMockRelay *mock);

/* === Integration with NostrRelay === */

/**
 * nostr_mock_relay_attach:
 * Attach mock relay to a NostrRelay's channels.
 * Must be called after nostr_relay_connect() since that creates the connection
 * with channels in NOSTR_TEST_MODE.
 *
 * @mock: Mock relay instance
 * @relay: Real NostrRelay instance (created with NOSTR_TEST_MODE=1)
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_attach(NostrMockRelay *mock, NostrRelay *relay);

/**
 * nostr_mock_relay_detach:
 * Detach mock relay from NostrRelay.
 *
 * @mock: Mock relay instance
 */
void nostr_mock_relay_detach(NostrMockRelay *mock);

/**
 * nostr_mock_relay_start:
 * Start the mock relay's message processing loop.
 * Should be called after attach and before the relay connects.
 *
 * @mock: Mock relay instance
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_start(NostrMockRelay *mock);

/**
 * nostr_mock_relay_stop:
 * Stop the mock relay's message processing loop.
 *
 * @mock: Mock relay instance
 */
void nostr_mock_relay_stop(NostrMockRelay *mock);

/* === Event Seeding === */

/**
 * nostr_mock_relay_seed_event:
 * Add an event to the mock relay's store.
 * Events are returned when a subscription's filter matches.
 *
 * @mock: Mock relay instance
 * @event: Event to seed (mock takes ownership via copy)
 * Returns: 0 on success
 */
int nostr_mock_relay_seed_event(NostrMockRelay *mock, NostrEvent *event);

/**
 * nostr_mock_relay_seed_events:
 * Add multiple events to the store.
 *
 * @mock: Mock relay instance
 * @events: Array of events (mock takes ownership of copies)
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
 *
 * @mock: Mock relay instance
 */
void nostr_mock_relay_clear_events(NostrMockRelay *mock);

/**
 * nostr_mock_relay_get_seeded_count:
 * Get the number of seeded events.
 *
 * @mock: Mock relay instance
 * Returns: Number of seeded events
 */
size_t nostr_mock_relay_get_seeded_count(NostrMockRelay *mock);

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
 *
 * @mock: Mock relay instance
 */
void nostr_mock_relay_clear_published(NostrMockRelay *mock);

/**
 * nostr_mock_relay_get_published_count:
 * Get the number of published (captured) events.
 *
 * @mock: Mock relay instance
 * Returns: Number of published events
 */
size_t nostr_mock_relay_get_published_count(NostrMockRelay *mock);

/* === Response Injection === */

/**
 * nostr_mock_relay_inject_notice:
 * Send a NOTICE message to the connected client.
 *
 * @mock: Mock relay instance
 * @message: Notice message
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_inject_notice(NostrMockRelay *mock, const char *message);

/**
 * nostr_mock_relay_inject_ok:
 * Send an OK response for an event ID.
 *
 * @mock: Mock relay instance
 * @event_id: Event ID the OK is for
 * @ok: Whether the event was accepted
 * @reason: Optional reason string (NULL for none)
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_inject_ok(NostrMockRelay *mock, const char *event_id, bool ok, const char *reason);

/**
 * nostr_mock_relay_inject_closed:
 * Send a CLOSED message for a subscription.
 *
 * @mock: Mock relay instance
 * @sub_id: Subscription ID
 * @reason: Reason for closure
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_inject_closed(NostrMockRelay *mock, const char *sub_id, const char *reason);

/**
 * nostr_mock_relay_inject_auth:
 * Send an AUTH challenge.
 *
 * @mock: Mock relay instance
 * @challenge: Challenge string
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_inject_auth(NostrMockRelay *mock, const char *challenge);

/**
 * nostr_mock_relay_inject_eose:
 * Send an EOSE (end of stored events) message for a subscription.
 *
 * @mock: Mock relay instance
 * @sub_id: Subscription ID
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_inject_eose(NostrMockRelay *mock, const char *sub_id);

/**
 * nostr_mock_relay_inject_event:
 * Send an EVENT message for a subscription.
 *
 * @mock: Mock relay instance
 * @sub_id: Subscription ID
 * @event: Event to send
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_relay_inject_event(NostrMockRelay *mock, const char *sub_id, NostrEvent *event);

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
 *
 * @mock: Mock relay instance
 */
void nostr_mock_relay_clear_fault(NostrMockRelay *mock);

/**
 * nostr_mock_relay_get_fault:
 * Get the current fault type.
 *
 * @mock: Mock relay instance
 * Returns: Current fault type
 */
NostrMockFaultType nostr_mock_relay_get_fault(NostrMockRelay *mock);

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
 *
 * @mock: Mock relay instance
 * @stats: (out) Statistics structure to fill
 */
void nostr_mock_relay_get_stats(NostrMockRelay *mock, NostrMockRelayStats *stats);

/**
 * nostr_mock_relay_reset_stats:
 * Reset all statistics counters to zero.
 *
 * @mock: Mock relay instance
 */
void nostr_mock_relay_reset_stats(NostrMockRelay *mock);

/* === Subscription Tracking === */

/**
 * nostr_mock_relay_get_subscription_count:
 * Get the number of active subscriptions.
 *
 * @mock: Mock relay instance
 * Returns: Number of active subscriptions
 */
size_t nostr_mock_relay_get_subscription_count(NostrMockRelay *mock);

/**
 * nostr_mock_relay_has_subscription:
 * Check if a subscription with the given ID is active.
 *
 * @mock: Mock relay instance
 * @sub_id: Subscription ID to check
 * Returns: true if subscription exists
 */
bool nostr_mock_relay_has_subscription(NostrMockRelay *mock, const char *sub_id);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_MOCK_RELAY_H */
