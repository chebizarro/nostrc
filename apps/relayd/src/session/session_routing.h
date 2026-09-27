/*
 * session_routing — event-class routing table for the session-scoped relay
 * (§3.2 D4 revised routing table).
 *
 * The session relay is a cache-and-queue, not an island: it must route
 * reads/writes to the correct upstream *per event class*. NIP-29 group
 * messages MUST NOT fan out to home_relays; NIP-17 gift wraps MUST land
 * on the recipient's kind-10050 inbox relays; ordinary user publish is
 * home_relays per NIP-65.
 *
 * This header defines the routing classes and the kind/event→class lookup
 * so the mapping lives in exactly one place. The upstream federation client
 * (§3.2 D4 "store-and-forward with reconnect backoff for writes", bead
 * nostrc-7d96) resolves a class to concrete relays in session_fed_policy.h
 * and delivers through session_federation.h; apps/relayd/README.md
 * "Upstream federation" is the forwarding contract.
 */
#ifndef NOSTR_SESSION_ROUTING_H
#define NOSTR_SESSION_ROUTING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  /* Default: user's home_relays (NIP-65). Applies to kinds we do not have
   * a per-class override for. */
  NSR_ROUTE_HOME_RELAYS = 0,

  /* Group-scoped (NIP-29): kinds 9-12 (messages), 9000-9030 (moderation,
   * join/leave requests), 39000-39005 (relay-signed metadata, group id in
   * `d`), and any other kind carrying an `h` tag (see
   * nostr_session_route_class_event()). The write-back target is the
   * group's own relay, never home_relays. */
  NSR_ROUTE_GROUP_RELAY = 1,

  /* NIP-17 gift-wrap DMs: kind 1059 wraps go to the recipient's kind-10050
   * inbox relays; kind 13 seals and kind 14 rumors are classed here only
   * so they are recognised -- they are never forwarded. */
  NSR_ROUTE_NIP17_INBOX = 2,

  /* NIP-09 tombstones (kind 5) — routed to home_relays, but flagged
   * separately so the outbox can honor deletion of the original target
   * addressable. */
  NSR_ROUTE_TOMBSTONE = 3,
} NostrSessionRouteClass;

/*
 * Map an event kind to its routing class, by kind alone. Table-driven so
 * new NIP-29 kinds are added in one place (`nostr-kinds.h` defines
 * 39000-39005 as of docs/nips db5fe3d).
 */
NostrSessionRouteClass nostr_session_route_class(uint32_t kind);

/*
 * Map an event to its routing class: the kind table, plus NIP-29 "normal
 * user-created events" — a group may accept any kind as long as it carries
 * an `h` tag (docs/nips/29.md), so an h-tagged event is group-scoped
 * whatever its kind (a kind-1 note, a kind-30023 article, a kind-5
 * deletion of a group message...). Classifying by kind alone would send
 * those to the user's home relays, leaking group content — including a
 * private group's — outside the group relay.
 *
 * Exceptions, which keep their kind's class even with an `h` tag:
 *   - NIP-17 kinds (13, 14, 1059): DM transport, never group traffic;
 *   - user-level replaceable state (0, 3, 10000-19999): one event per
 *     author, so it cannot belong to one group — e.g. the kind-10009 group
 *     list and kind-10011 favorite follow sets stay on home_relays;
 *   - ephemeral kinds (20000-29999): never forwarded.
 *
 * @has_h_tag: non-zero when the event has an `h` tag with a non-empty
 *   value.
 */
NostrSessionRouteClass nostr_session_route_class_event(uint32_t kind, int has_h_tag);

/*
 * Human-readable name for logging. Never returns NULL.
 */
const char *nostr_session_route_class_name(NostrSessionRouteClass c);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_SESSION_ROUTING_H */
