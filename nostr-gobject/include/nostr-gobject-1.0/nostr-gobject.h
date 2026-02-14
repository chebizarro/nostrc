#pragma once

/**
 * SECTION:nostr-gobject
 * @short_description: Master include for nostr-gobject
 *
 * Include this header for the core GObject wrapper API.
 * Headers that directly include core libnostr types are excluded
 * to prevent typedef conflicts — include them individually when needed.
 */

#define NOSTR_GOBJECT_INSIDE

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
#include "nostr_nip19.h"
#include "nostr_filter.h"
#include "nostr_json.h"
#include "nostr_tag_list.h"
#include "nostr_nip49.h"
#include "nostr_bip39.h"
#include "nostr_async.h"
#include "crypto_utils_gobject.h"
#include "nostr_utils.h"
#include "nostr_profile_provider.h"
#include "nostr_profile_service.h"

/* Services (moved from app, nostrc-lx23) */
#include "gnostr-identity.h"
#include "gnostr-relays.h"
#include "gnostr-mute-list.h"
#include "gnostr-sync-service.h"

#undef NOSTR_GOBJECT_INSIDE

/*
 * The following headers are NOT included here because they pull in
 * core libnostr headers that create typedef conflicts:
 *
 *   #include "nostr_relay_store.h"     - pulls core nostr-relay-store.h
 *   #include "nostr_simple_pool.h"     - pulls core nostr-simple-pool.h
 *   #include "nostr_query_batcher.h"   - pulls core nostr-filter.h
 */
