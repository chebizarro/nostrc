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
 * This header defines the routing classes and the kind→class lookup so
 * the mapping lives in exactly one place. The wire-level "actually
 * connect to upstream" client is intentionally out of scope for the
 * initial session-relay landing (§3.2 D4 says "Store-and-forward with
 * reconnect backoff for writes. No negentropy sync in v1"): a real
 * upstream federation client is tracked separately.
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

  /* Group-scoped (NIP-29): kinds 9-12 (messages), 39000-39004 (metadata).
   * The write-back target is the group's own recorded relay URL; the
   * cache preserves relay-of-origin per stored event. */
  NSR_ROUTE_GROUP_RELAY = 1,

  /* NIP-17 gift-wrap DMs: kind 1059 inbound (fetched from the recipient's
   * kind-10050 inbox relays), kind 14 outbound (published to the
   * recipient's inbox relays). */
  NSR_ROUTE_NIP17_INBOX = 2,

  /* NIP-09 tombstones (kind 5) — routed to home_relays, but flagged
   * separately so the outbox can honor deletion of the original target
   * addressable. */
  NSR_ROUTE_TOMBSTONE = 3,
} NostrSessionRouteClass;

/*
 * Map an event kind to its routing class.
 *
 * The mapping is intentionally table-driven so future NIP-29 subrange
 * assignments (39005-39009 are not yet defined per `nostr-kinds.h`) can
 * be added in one place.
 */
NostrSessionRouteClass nostr_session_route_class(uint32_t kind);

/*
 * Human-readable name for logging. Never returns NULL.
 */
const char *nostr_session_route_class_name(NostrSessionRouteClass c);

#ifdef __cplusplus
}
#endif

#endif /* NOSTR_SESSION_ROUTING_H */
