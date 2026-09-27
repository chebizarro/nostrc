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

#include "nostr-kinds.h"

NostrSessionRouteClass nostr_session_route_class(uint32_t kind) {
  /* NIP-29 group messages (kinds 9-12), moderation / join / leave requests
   * (9000-9030: 9000-9020 moderation, 9021 join, 9022 leave) and
   * relay-signed metadata (39000-39005: metadata, admins, members, roles,
   * livekit participants, pinned events). */
  if (kind >= NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE && kind <= NOSTR_KIND_SIMPLE_GROUP_REPLY)
    return NSR_ROUTE_GROUP_RELAY;
  if (kind >= NOSTR_KIND_SIMPLE_GROUP_ADD_USER && kind <= 9030) return NSR_ROUTE_GROUP_RELAY;
  if (kind >= NOSTR_KIND_SIMPLE_GROUP_METADATA && kind <= NOSTR_KIND_SIMPLE_GROUP_PINNED_EVENTS)
    return NSR_ROUTE_GROUP_RELAY;

  /* NIP-17 gift wraps + inner seals. Kind 14 is the DM rumor's kind after
   * unwrap; the outer wrap on the wire is kind 1059. Only 1059 is ever
   * forwarded upstream (session_fed_policy.c): 13/14 carry the sender's
   * identity / plaintext and never leave the machine. */
  if (kind == NOSTR_KIND_GIFT_WRAP || kind == NOSTR_KIND_DIRECT_MESSAGE ||
      kind == NOSTR_KIND_SEAL)
    return NSR_ROUTE_NIP17_INBOX;

  /* NIP-09 tombstones. */
  if (kind == NOSTR_KIND_DELETION) return NSR_ROUTE_TOMBSTONE;

  /* Everything else — user publish, Track 2 addressables (31922/31923/
   * 30085 etc), kind 0 profile, kind 3 contact list, kind 10050 inbox
   * hints, kind 10002 relay list — routes to home_relays. */
  return NSR_ROUTE_HOME_RELAYS;
}

NostrSessionRouteClass nostr_session_route_class_event(uint32_t kind, int has_h_tag) {
  NostrSessionRouteClass c = nostr_session_route_class(kind);
  if (!has_h_tag || c == NSR_ROUTE_GROUP_RELAY || c == NSR_ROUTE_NIP17_INBOX) return c;
  if (kind > (uint32_t)INT32_MAX) return c;
  if (nostr_kind_is_replaceable((int)kind) || nostr_kind_is_ephemeral((int)kind)) return c;
  /* NIP-29 "normal user-created events": any other h-tagged kind. */
  return NSR_ROUTE_GROUP_RELAY;
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
