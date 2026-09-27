/*
 * session_routing — event-class routing table implementation.
 *
 * The mapping mirrors §3.2 D4 in the plan. Keep this file dumb: just the
 * table, no upstream connection state. Any policy hook that needs to know
 * "which upstream should this event go to?" imports this header and gets
 * the class, then consults its own runtime state (group→relay map, DM
 * inbox map, home_relays list) to resolve the concrete upstream(s).
 */
#include "session_routing.h"

NostrSessionRouteClass nostr_session_route_class(uint32_t kind) {
  /* NIP-29 group messages (kinds 9-12), moderation / join / leave requests
   * (9000-9030: 9000-9020 moderation, 9021 join, 9022 leave) and
   * relay-signed metadata (39000-39004). */
  if (kind >= 9 && kind <= 12) return NSR_ROUTE_GROUP_RELAY;
  if (kind >= 9000 && kind <= 9030) return NSR_ROUTE_GROUP_RELAY;
  if (kind >= 39000 && kind <= 39004) return NSR_ROUTE_GROUP_RELAY;

  /* NIP-17 gift wraps + inner seals. Kind 14 is the DM rumor's kind after
   * unwrap; the outer wrap on the wire is kind 1059. Only 1059 is ever
   * forwarded upstream (session_fed_policy.c): 13/14 carry the sender's
   * identity / plaintext and never leave the machine. */
  if (kind == 1059 || kind == 14 || kind == 13) return NSR_ROUTE_NIP17_INBOX;

  /* NIP-09 tombstones. */
  if (kind == 5) return NSR_ROUTE_TOMBSTONE;

  /* Everything else — user publish, Track 2 addressables (31922/31923/
   * 30085 etc), kind 0 profile, kind 3 contact list, kind 10050 inbox
   * hints, kind 10002 relay list — routes to home_relays. */
  return NSR_ROUTE_HOME_RELAYS;
}

const char *nostr_session_route_class_name(NostrSessionRouteClass c) {
  switch (c) {
    case NSR_ROUTE_HOME_RELAYS: return "home_relays";
    case NSR_ROUTE_GROUP_RELAY: return "group_relay";
    case NSR_ROUTE_NIP17_INBOX: return "nip17_inbox";
    case NSR_ROUTE_TOMBSTONE:   return "tombstone";
  }
  return "?";
}
