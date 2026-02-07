#pragma once

/**
 * SECTION:nostr-glib
 * @short_description: Master include for nostr-glib
 *
 * Include this header for the core GObject wrapper API.
 * Headers that directly include core libnostr types are excluded
 * to prevent typedef conflicts — include them individually when needed.
 */

#define NOSTR_GLIB_INSIDE

/* Foundation */
#include "nostr-error.h"
#include "nostr-types.h"
#include "nostr-enums.h"

/* Core GObject wrappers (G-prefixed, no core header conflicts) */
#include "nostr_event.h"
#include "nostr_event_bus.h"
#include "nostr_keys.h"
#include "nostr_relay.h"
#include "nostr_pool.h"
#include "nostr_subscription.h"
#include "nostr_subscription_registry.h"
#include "nostr_pointer.h"
#include "nostr_async.h"
#include "crypto_utils_gobject.h"

#undef NOSTR_GLIB_INSIDE

/*
 * The following headers are NOT included here because they pull in
 * core libnostr headers that create typedef conflicts with GObject
 * wrapper types. Include them directly when needed:
 *
 *   #include "nostr_filter.h"          - NostrFilter name collision
 *   #include "nostr_tag_list.h"        - NostrTag name collision
 *   #include "nostr_relay_store.h"     - pulls core nostr-relay-store.h
 *   #include "nostr_simple_pool.h"     - pulls core nostr-simple-pool.h
 *   #include "nostr_query_batcher.h"   - pulls core nostr-filter.h
 */
