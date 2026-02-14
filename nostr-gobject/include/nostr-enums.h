#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * GNostrRelayState:
 * @GNOSTR_RELAY_STATE_DISCONNECTED: Not connected
 * @GNOSTR_RELAY_STATE_CONNECTING: Connection in progress
 * @GNOSTR_RELAY_STATE_CONNECTED: Connected and ready
 * @GNOSTR_RELAY_STATE_ERROR: Connection error
 *
 * GObject wrapper connection state enum. Uses G prefix to avoid
 * conflicts with core libnostr's NostrRelayState enum.
 */
typedef enum {
  GNOSTR_RELAY_STATE_DISCONNECTED,
  GNOSTR_RELAY_STATE_CONNECTING,
  GNOSTR_RELAY_STATE_CONNECTED,
  GNOSTR_RELAY_STATE_ERROR
} GNostrRelayState;

GType gnostr_relay_state_get_type(void) G_GNUC_CONST;
#define GNOSTR_TYPE_RELAY_STATE (gnostr_relay_state_get_type())

/**
 * NostrEventKind:
 * Common event kinds as defined in NIPs
 */
typedef enum {
  NOSTR_EVENT_KIND_METADATA = 0,
  NOSTR_EVENT_KIND_TEXT_NOTE = 1,
  NOSTR_EVENT_KIND_RECOMMEND_RELAY = 2,
  NOSTR_EVENT_KIND_CONTACTS = 3,
  NOSTR_EVENT_KIND_ENCRYPTED_DM = 4,
  NOSTR_EVENT_KIND_DELETE = 5,
  NOSTR_EVENT_KIND_REPOST = 6,
  NOSTR_EVENT_KIND_REACTION = 7,
  NOSTR_EVENT_KIND_BADGE_AWARD = 8,
  NOSTR_EVENT_KIND_CHANNEL_CREATE = 40,
  NOSTR_EVENT_KIND_CHANNEL_METADATA = 41,
  NOSTR_EVENT_KIND_CHANNEL_MESSAGE = 42,
  NOSTR_EVENT_KIND_CHANNEL_HIDE = 43,
  NOSTR_EVENT_KIND_CHANNEL_MUTE = 44,
  NOSTR_EVENT_KIND_CHESS = 64,
  /* Add more as needed */
} NostrEventKind;

/**
 * GNostrSubscriptionState:
 * @GNOSTR_SUBSCRIPTION_STATE_PENDING: Created but not yet sent to relay
 * @GNOSTR_SUBSCRIPTION_STATE_ACTIVE: Active and receiving events
 * @GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED: End of stored events received
 * @GNOSTR_SUBSCRIPTION_STATE_CLOSED: Subscription has been closed
 * @GNOSTR_SUBSCRIPTION_STATE_ERROR: Subscription encountered an error
 *
 * Lifecycle state of a GObject Nostr subscription.
 */
typedef enum {
  GNOSTR_SUBSCRIPTION_STATE_PENDING,
  GNOSTR_SUBSCRIPTION_STATE_ACTIVE,
  GNOSTR_SUBSCRIPTION_STATE_EOSE_RECEIVED,
  GNOSTR_SUBSCRIPTION_STATE_CLOSED,
  GNOSTR_SUBSCRIPTION_STATE_ERROR
} GNostrSubscriptionState;

GType gnostr_subscription_state_get_type(void) G_GNUC_CONST;
#define GNOSTR_TYPE_SUBSCRIPTION_STATE (gnostr_subscription_state_get_type())

/**
 * GNostrNip46State:
 * @GNOSTR_NIP46_STATE_DISCONNECTED: Not connected to remote signer
 * @GNOSTR_NIP46_STATE_CONNECTING: Connection in progress
 * @GNOSTR_NIP46_STATE_CONNECTED: Connected and ready for RPC
 * @GNOSTR_NIP46_STATE_STOPPING: Shutting down connection
 *
 * NIP-46 remote signer session state. Maps to core NostrNip46State.
 */
typedef enum {
  GNOSTR_NIP46_STATE_DISCONNECTED,
  GNOSTR_NIP46_STATE_CONNECTING,
  GNOSTR_NIP46_STATE_CONNECTED,
  GNOSTR_NIP46_STATE_STOPPING
} GNostrNip46State;

GType gnostr_nip46_state_get_type(void) G_GNUC_CONST;
#define GNOSTR_TYPE_NIP46_STATE (gnostr_nip46_state_get_type())

G_END_DECLS
