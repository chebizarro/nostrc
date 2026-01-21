/**
 * @file mock_relay_server.h
 * @brief Standalone mock relay WebSocket server for integration tests
 *
 * This module provides a lightweight WebSocket server that simulates a Nostr relay
 * for integration testing. It allows seeding events, capturing publications, and
 * testing NIP-11 relay information endpoints.
 *
 * Key features:
 * - Port 0 support for automatic port assignment (parallel test safety)
 * - Thread-safe event storage
 * - JSONL file seeding support
 * - NIP-11 relay information document support
 */
#ifndef NOSTR_MOCK_RELAY_SERVER_H
#define NOSTR_MOCK_RELAY_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque server handle */
typedef struct NostrMockRelayServer NostrMockRelayServer;

/**
 * NostrMockRelayServerConfig:
 * Configuration options for the mock relay server.
 */
typedef struct {
    uint16_t port;              /**< TCP port (0 = auto-assign) */
    const char *bind_addr;      /**< Bind address (NULL = "127.0.0.1") */
    bool use_tls;               /**< Enable WSS (requires cert/key) */
    const char *cert_path;      /**< TLS certificate path (PEM) */
    const char *key_path;       /**< TLS private key path (PEM) */
    const char *seed_file;      /**< JSONL file to pre-seed events (NULL = none) */
    const char *relay_name;     /**< Relay name for NIP-11 (NULL = "MockRelay") */
    const char *relay_desc;     /**< Relay description for NIP-11 */
    bool auto_eose;             /**< Auto-send EOSE after seeded events (default: true) */
    bool validate_signatures;   /**< Reject events with invalid signatures */
    int response_delay_ms;      /**< Artificial delay before responses (0 = immediate) */
    int max_events_per_req;     /**< Limit on events returned per REQ (-1 = unlimited) */
} NostrMockRelayServerConfig;

/**
 * nostr_mock_server_config_default:
 * Returns a default configuration suitable for most tests.
 *
 * Defaults:
 * - port: 0 (auto-assign)
 * - bind_addr: "127.0.0.1"
 * - use_tls: false
 * - auto_eose: true
 * - validate_signatures: false
 * - response_delay_ms: 0
 * - max_events_per_req: -1 (unlimited)
 *
 * Returns: Default configuration struct
 */
NostrMockRelayServerConfig nostr_mock_server_config_default(void);

/* === Lifecycle === */

/**
 * nostr_mock_server_new:
 * Create a new mock relay server instance. Does not start listening yet.
 *
 * @config: Server configuration (NULL for defaults)
 * Returns: Server instance, NULL on error
 */
NostrMockRelayServer *nostr_mock_server_new(const NostrMockRelayServerConfig *config);

/**
 * nostr_mock_server_start:
 * Start the server and begin listening for connections.
 * The server runs in a background thread.
 *
 * @server: Server instance
 * Returns: 0 on success, -1 on error
 */
int nostr_mock_server_start(NostrMockRelayServer *server);

/**
 * nostr_mock_server_stop:
 * Stop the server and close all connections.
 *
 * @server: Server instance
 */
void nostr_mock_server_stop(NostrMockRelayServer *server);

/**
 * nostr_mock_server_free:
 * Stop the server (if running) and free all resources.
 *
 * @server: Server instance (safe to call with NULL)
 */
void nostr_mock_server_free(NostrMockRelayServer *server);

/* === Connection Info === */

/**
 * nostr_mock_server_get_url:
 * Get the WebSocket URL for connecting to this server.
 * Only valid after nostr_mock_server_start() succeeds.
 *
 * @server: Server instance
 * Returns: URL string like "ws://127.0.0.1:7777" (owned by server, do not free)
 */
const char *nostr_mock_server_get_url(NostrMockRelayServer *server);

/**
 * nostr_mock_server_get_port:
 * Get the actual port (useful when port=0 was specified for auto-assignment).
 *
 * @server: Server instance
 * Returns: Port number, or 0 if not started
 */
uint16_t nostr_mock_server_get_port(NostrMockRelayServer *server);

/**
 * nostr_mock_server_get_connection_count:
 * Get the number of currently connected clients.
 *
 * @server: Server instance
 * Returns: Number of active connections
 */
size_t nostr_mock_server_get_connection_count(NostrMockRelayServer *server);

/* === Event Seeding === */

/**
 * nostr_mock_server_seed_event:
 * Add an event to the server's store. Events are returned when a
 * subscription's filter matches.
 *
 * @server: Server instance
 * @event_json: JSON string of the event object
 * Returns: 0 on success, -1 on parse error
 */
int nostr_mock_server_seed_event(NostrMockRelayServer *server, const char *event_json);

/**
 * nostr_mock_server_seed_from_jsonl:
 * Load events from a JSONL file (one JSON event per line).
 *
 * @server: Server instance
 * @jsonl_path: Path to JSONL file
 * Returns: Number of events loaded, -1 on error
 */
int nostr_mock_server_seed_from_jsonl(NostrMockRelayServer *server, const char *jsonl_path);

/**
 * nostr_mock_server_clear_events:
 * Remove all seeded events from the store.
 *
 * @server: Server instance
 */
void nostr_mock_server_clear_events(NostrMockRelayServer *server);

/**
 * nostr_mock_server_get_seeded_count:
 * Get the number of seeded events.
 *
 * @server: Server instance
 * Returns: Number of seeded events
 */
size_t nostr_mock_server_get_seeded_count(NostrMockRelayServer *server);

/* === Publication Capture === */

/**
 * nostr_mock_server_get_published_json:
 * Get all published events as a JSON array string.
 * This returns events that clients have published to the mock relay.
 *
 * @server: Server instance
 * Returns: JSON array string (caller must free), or NULL if no events
 */
char *nostr_mock_server_get_published_json(NostrMockRelayServer *server);

/**
 * nostr_mock_server_get_published_count:
 * Get the number of events published by clients.
 *
 * @server: Server instance
 * Returns: Number of published events
 */
size_t nostr_mock_server_get_published_count(NostrMockRelayServer *server);

/**
 * nostr_mock_server_clear_published:
 * Clear captured publications.
 *
 * @server: Server instance
 */
void nostr_mock_server_clear_published(NostrMockRelayServer *server);

/**
 * nostr_mock_server_await_publish:
 * Block until an event is published or timeout.
 *
 * @server: Server instance
 * @timeout_ms: Timeout in milliseconds (0 = no wait, -1 = indefinite)
 * Returns: JSON string of published event (caller must free), NULL on timeout
 */
char *nostr_mock_server_await_publish(NostrMockRelayServer *server, int timeout_ms);

/* === Statistics === */

/**
 * NostrMockRelayStats:
 * Statistics about the mock relay's operation.
 */
typedef struct {
    size_t events_seeded;           /**< Total events in seed store */
    size_t events_matched;          /**< Events returned to subscriptions */
    size_t events_published;        /**< Events received from clients */
    size_t subscriptions_received;  /**< REQ messages received */
    size_t close_received;          /**< CLOSE messages received */
    size_t connections_total;       /**< Total connections (historical) */
    size_t connections_current;     /**< Currently active connections */
} NostrMockRelayStats;

/**
 * nostr_mock_server_get_stats:
 * Get mock relay statistics.
 *
 * @server: Server instance
 * @stats: (out) Statistics structure to fill
 */
void nostr_mock_server_get_stats(NostrMockRelayServer *server, NostrMockRelayStats *stats);

/* === NIP-11 Relay Information === */

/**
 * nostr_mock_server_set_nip11_json:
 * Set custom NIP-11 relay information document.
 * If not set, a default document is generated from config options.
 *
 * @server: Server instance
 * @nip11_json: JSON string of the NIP-11 document (server makes a copy)
 */
void nostr_mock_server_set_nip11_json(NostrMockRelayServer *server, const char *nip11_json);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_MOCK_RELAY_SERVER_H */
