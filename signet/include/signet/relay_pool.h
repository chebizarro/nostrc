/* SPDX-License-Identifier: MIT
 *
 * relay_pool.h - Relay connectivity abstraction for Signet.
 *
 * Signet listens on configured relays for:
 * - NIP-46 request events (kind 24133)
 * - Signet management events (custom kinds; see mgmt_protocol.h)
 *
 * This module is responsible for:
 * - Managing multiple relay connections
 * - Reconnecting with bounded backoff
 * - Thread-safe event publishing
 * - Delivering received events to the configured callback
 *
 * NOTE: The actual libnostr receive callback integration may be implemented in
 * a higher layer. To support that cleanly, this module provides
 * signet_relay_pool_handle_event_json() which parses a NIP-01 event JSON
 * into a SignetRelayEventView and dispatches it through the configured callback.
 */

#ifndef SIGNET_RELAY_POOL_H
#define SIGNET_RELAY_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct SignetRelayPool SignetRelayPool;

/* Minimal view of an incoming event.
 * This keeps Signet decoupled from any particular libnostr event struct ABI.
 */
typedef struct {
  int kind;
  const char *event_id_hex;      /* borrowed; may be NULL */
  const char *pubkey_hex;        /* borrowed; may be NULL */
  int64_t created_at;            /* unix seconds */
  const char *content;           /* borrowed; may be NULL */
} SignetRelayEventView;

typedef void (*SignetRelayEventCallback)(const SignetRelayEventView *ev, void *user_data);

typedef struct {
  const char *const *relays;
  size_t n_relays;

  /* Called on each received event after minimal parsing/validation. */
  SignetRelayEventCallback on_event;
  void *user_data;

  /* Optional: hex-encoded private key for NIP-42 AUTH responses.
   * When set, AUTH challenges from any relay in the pool are answered
   * automatically with a signed kind:22242 event.  Pass NULL to disable
   * (connections to relays that require auth will be rejected silently). */
  const char *auth_sk_hex;

  /* Optional: override URL used in the NIP-42 AUTH event's "relay" tag.
   * Use this when connecting to a relay via an internal address (e.g.
   * ws://172.20.0.1:7777) but the relay validates against a public URL
   * (e.g. wss://armada.sharegap.net set via RELAY_URL env on Khatru).
   * If NULL, the relay connection URL is used (default behaviour). */
  const char *auth_relay_tag_url;
} SignetRelayPoolConfig;

/* Create relay pool. Returns NULL on OOM. */
SignetRelayPool *signet_relay_pool_new(const SignetRelayPoolConfig *cfg);

/* Free relay pool. Safe on NULL. */
void signet_relay_pool_free(SignetRelayPool *rp);

/* Start relay connections/subscriptions. Returns 0 on success, -1 on failure. */
int signet_relay_pool_start(SignetRelayPool *rp);

/* Stop relay connections/subscriptions. Safe to call multiple times. */
void signet_relay_pool_stop(SignetRelayPool *rp);

/* Update subscribed kinds. Thread-safe.
 * Passing n_kinds=0 clears subscription intent. */
int signet_relay_pool_subscribe_kinds(SignetRelayPool *rp, const int *kinds, size_t n_kinds);

/* Publish a raw Nostr event JSON string to relays (thread-safe).
 * Returns 0 on enqueue success, -1 if not started / OOM. */
int signet_relay_pool_publish_event_json(SignetRelayPool *rp, const char *event_json);

/* Parse a NIP-01 event JSON string and dispatch it to the configured callback.
 * This is intended to be called by the libnostr receive-event glue layer.
 *
 * Returns:
 *  0 on successful parse+dispatch,
 * -1 on parse error or if no callback is configured.
 */
int signet_relay_pool_handle_event_json(SignetRelayPool *rp, const char *event_json);

/* True if at least one relay is currently connected. */
bool signet_relay_pool_is_connected(SignetRelayPool *rp);

#ifdef __cplusplus
}
#endif

#endif /* SIGNET_RELAY_POOL_H */