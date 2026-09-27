/* nostr-publish-session-relay.h - Upstream status of the per-user session relay
 *
 * SPDX-License-Identifier: MIT
 *
 * Client side of `org.nostr.SessionRelay1`'s upstream-federation surface
 * (gnome/dbus/org.nostr.SessionRelay1.xml; the contract is
 * apps/relayd/README.md "Upstream federation", bead nostrc-7d96), shared
 * by the publishers that may route through the session relay (nostr-dav,
 * nostr-share; bead nostrc-t24q):
 *
 *   * FederationState tells whether an `OK true` from relay.sock will ever
 *     leave the machine. Only `active` and `waiting-for-account` forward;
 *     `disabled`, `unavailable`, an unrecognised value, a daemon that
 *     predates the property and a daemon that is not running do not, and
 *     a publisher must then not treat the session relay as its upstream
 *     (fail closed).
 *   * An event's upstream state (UpstreamStatusChanged / GetEventUpstream)
 *     replaces the local OK as the publish verdict: `forwarded` / `partial`
 *     are published; `failed`, `skipped`, `superseded`, `cancelled` and
 *     `unknown` are final without delivery; `new`, `pending` and
 *     `unroutable` are still in progress.
 *
 * The bus name is owned by the running daemon only (no D-Bus activation
 * file: the relay is socket-activated). Every call here uses
 * G_DBUS_CALL_FLAGS_NO_AUTO_START; nostr_publish_session_relay_nudge()
 * starts a stopped relay the supported way, by connecting to its socket.
 */
#ifndef NOSTR_PUBLISH_SESSION_RELAY_H
#define NOSTR_PUBLISH_SESSION_RELAY_H

#include <glib.h>
#include <gio/gio.h>

#include "nostr-publish-macros.h"

G_BEGIN_DECLS

#define NOSTR_PUBLISH_SESSION_RELAY_BUS_NAME "org.nostr.SessionRelay1"
#define NOSTR_PUBLISH_SESSION_RELAY_PATH     "/org/nostr/SessionRelay1"
#define NOSTR_PUBLISH_SESSION_RELAY_IFACE    "org.nostr.SessionRelay1"

/**
 * NostrPublishFederation:
 * @NOSTR_PUBLISH_FEDERATION_UNKNOWN: not determined yet
 * @NOSTR_PUBLISH_FEDERATION_NOT_RUNNING: nobody owns the bus name (relay
 *   stopped, not socket-activated yet, or no session bus)
 * @NOSTR_PUBLISH_FEDERATION_UNSUPPORTED: the daemon has no FederationState
 *   property (it predates nostrc-7d96), reports a value this library does
 *   not know, or did not answer
 * @NOSTR_PUBLISH_FEDERATION_ACTIVE: `active`
 * @NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT: `waiting-for-account`
 * @NOSTR_PUBLISH_FEDERATION_DISABLED: `disabled` (federation = 0)
 * @NOSTR_PUBLISH_FEDERATION_UNAVAILABLE: `unavailable`
 */
typedef enum {
  NOSTR_PUBLISH_FEDERATION_UNKNOWN = 0,
  NOSTR_PUBLISH_FEDERATION_NOT_RUNNING,
  NOSTR_PUBLISH_FEDERATION_UNSUPPORTED,
  NOSTR_PUBLISH_FEDERATION_ACTIVE,
  NOSTR_PUBLISH_FEDERATION_WAITING_FOR_ACCOUNT,
  NOSTR_PUBLISH_FEDERATION_DISABLED,
  NOSTR_PUBLISH_FEDERATION_UNAVAILABLE
} NostrPublishFederation;

/** Parses a FederationState value; anything unrecognised is UNSUPPORTED. */
NOSTR_PUBLISH_API
NostrPublishFederation nostr_publish_federation_from_string(const gchar *state);

/** The D-Bus spelling ("active", …) or a descriptive word for the
 *  client-side states ("unknown", "not-running", "unsupported"). */
NOSTR_PUBLISH_API
const gchar *nostr_publish_federation_to_string(NostrPublishFederation state);

/** TRUE only for ACTIVE and WAITING_FOR_ACCOUNT. */
NOSTR_PUBLISH_API
gboolean nostr_publish_federation_forwards(NostrPublishFederation state);

/**
 * NostrPublishForwardState:
 *
 * An event's upstream state as reported by the session relay
 * (README state table). UNKNOWN covers "unknown" and any value this
 * library does not recognise.
 */
typedef enum {
  NOSTR_PUBLISH_FORWARD_UNKNOWN = 0,
  NOSTR_PUBLISH_FORWARD_NEW,
  NOSTR_PUBLISH_FORWARD_UNROUTABLE,
  NOSTR_PUBLISH_FORWARD_PENDING,
  NOSTR_PUBLISH_FORWARD_FORWARDED,
  NOSTR_PUBLISH_FORWARD_PARTIAL,
  NOSTR_PUBLISH_FORWARD_FAILED,
  NOSTR_PUBLISH_FORWARD_SKIPPED,
  NOSTR_PUBLISH_FORWARD_SUPERSEDED,
  NOSTR_PUBLISH_FORWARD_CANCELLED
} NostrPublishForwardState;

NOSTR_PUBLISH_API
NostrPublishForwardState nostr_publish_forward_state_from_string(const gchar *state);
NOSTR_PUBLISH_API
const gchar *nostr_publish_forward_state_to_string(NostrPublishForwardState state);

/** TRUE for FORWARDED and PARTIAL: at least one upstream relay has it. */
NOSTR_PUBLISH_API
gboolean nostr_publish_forward_state_delivered(NostrPublishForwardState state);

/** TRUE when the state will not change any more: delivered, FAILED,
 *  SKIPPED, SUPERSEDED, CANCELLED, and UNKNOWN (for an event the relay
 *  acknowledged, "not in the outbox" means it was never queued). */
NOSTR_PUBLISH_API
gboolean nostr_publish_forward_state_is_final(NostrPublishForwardState state);

/**
 * NostrPublishForwardUpdate:
 * @event_id: 64-hex event id
 * @state: the event's upstream state after this update
 * @detail: event-level explanation (the unroutable / skipped reason); ""
 *   when there is none
 * @relay_url: (nullable): the target relay whose state changed
 *   (UpstreamStatusChanged), NULL for event-level updates and for
 *   GetEventUpstream replies
 * @relay_state: (nullable): that relay's state (`pending`, `acked`,
 *   `failed`, `cancelled`)
 * @relay_reason: (nullable): that relay's last NIP-01 OK reason or
 *   transport error
 * @relays: (nullable): GetEventUpstream replies only: every target as
 *   "url relay_state" or "url relay_state: reason", in reply order
 */
typedef struct {
  const gchar              *event_id;
  NostrPublishForwardState  state;
  const gchar              *detail;
  const gchar              *relay_url;
  const gchar              *relay_state;
  const gchar              *relay_reason;
  const gchar *const       *relays;
} NostrPublishForwardUpdate;

typedef struct _NostrPublishSessionRelay NostrPublishSessionRelay;

typedef void (*NostrPublishFederationFunc)(NostrPublishSessionRelay *relay,
                                           NostrPublishFederation    state,
                                           gpointer                  user_data);
typedef void (*NostrPublishForwardFunc)(NostrPublishSessionRelay        *relay,
                                        const NostrPublishForwardUpdate *update,
                                        gpointer                         user_data);

/**
 * nostr_publish_session_relay_new:
 * @bus: (transfer none): the session bus
 * @federation_cb: (nullable): called whenever the federation state
 *   changes (including the first determination)
 * @forward_cb: (nullable): called for every UpstreamStatusChanged signal
 *   and every nostr_publish_session_relay_query() reply
 *
 * Watches the bus name (without auto-starting anything), reads
 * FederationState whenever an owner appears (NOT_RUNNING while none
 * does), and subscribes to UpstreamStatusChanged. Callbacks run on the
 * thread-default main context current at construction; nothing is
 * reported synchronously from here.
 *
 * Returns: (transfer full): the watcher; free with
 *   nostr_publish_session_relay_free().
 */
NOSTR_PUBLISH_API
NostrPublishSessionRelay *nostr_publish_session_relay_new(GDBusConnection           *bus,
                                                          NostrPublishFederationFunc federation_cb,
                                                          NostrPublishForwardFunc    forward_cb,
                                                          gpointer                   user_data);

/** Unsubscribes and cancels in-flight calls; no callback runs afterwards. */
NOSTR_PUBLISH_API
void nostr_publish_session_relay_free(NostrPublishSessionRelay *self);

/** Last known federation state (UNKNOWN until first determined). */
NOSTR_PUBLISH_API
NostrPublishFederation nostr_publish_session_relay_get_federation(NostrPublishSessionRelay *self);

/** Re-reads FederationState (asynchronously; the property is not
 *  change-notified). A no-op while no owner is known. */
NOSTR_PUBLISH_API
void nostr_publish_session_relay_refresh(NostrPublishSessionRelay *self);

/** Asks GetEventUpstream(@event_id); the reply arrives through
 *  @forward_cb. Errors (relay gone, old daemon) are dropped silently: the
 *  federation callback reports those conditions. */
NOSTR_PUBLISH_API
void nostr_publish_session_relay_query(NostrPublishSessionRelay *self,
                                       const gchar              *event_id);

/**
 * nostr_publish_session_relay_nudge:
 * @socket_path: the relay's AF_UNIX socket ($XDG_RUNTIME_DIR/nostr/relay.sock)
 *
 * Connects to the socket and closes the connection at once: the way to
 * start a socket-activated relay that is not running (its bus name has no
 * owner and no D-Bus activation file exists). Best effort.
 *
 * Returns: TRUE if the connection was accepted.
 */
NOSTR_PUBLISH_API
gboolean nostr_publish_session_relay_nudge(const gchar *socket_path);

G_END_DECLS
#endif /* NOSTR_PUBLISH_SESSION_RELAY_H */
