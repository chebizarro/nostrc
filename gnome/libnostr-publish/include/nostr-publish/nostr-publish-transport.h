/* nostr-publish-transport.h - Single-relay NIP-01 transport abstraction
 *
 * SPDX-License-Identifier: MIT
 *
 * Message-oriented interface over one relay connection so that:
 *   1. production builds back it with libsoup 3's SoupWebsocketConnection
 *      (no libnostr / libwebsockets / nsync dependency);
 *   2. tests inject a deterministic in-process fixture that records
 *      outbound frames and injects EVENT/EOSE/OK/AUTH replies without a
 *      socket.
 *
 * Originally nostr-dav's NdRelayTransport (nd-relay-transport.h); nostr-dav
 * keeps those names as aliases and its relay-sync layer shares transports
 * with the publisher. Not thread-safe: use from one main context.
 */
#ifndef NOSTR_PUBLISH_TRANSPORT_H
#define NOSTR_PUBLISH_TRANSPORT_H

#include <glib.h>
#include <gio/gio.h>

#include "nostr-publish-macros.h"

G_BEGIN_DECLS

typedef struct _NostrPublishTransport NostrPublishTransport;

/**
 * NostrPublishTransportFactory:
 * @relay_url: URL the caller wants a transport for
 * @user_data: opaque pointer supplied to the factory registrant
 *
 * Returns: (transfer full) (nullable): a transport that resolves to
 *   @relay_url, or NULL when the factory refuses the URL.
 */
typedef NostrPublishTransport *(*NostrPublishTransportFactory)(const gchar *relay_url,
                                                     gpointer     user_data);

/**
 * NostrPublishTransportListener:
 *
 * Called from the transport's own thread/context whenever an incoming
 * frame is decoded. Implementations must forward through the main
 * context if they need to touch SQLite. @envelope_json is transient —
 * copy anything you need to keep.
 *
 * @kind_hint: one of `"EVENT"`, `"EOSE"`, `"OK"`, `"AUTH"`, `"CLOSED"`,
 *   `"NOTICE"`, or NULL for unknown; a fast prefix routing hint so the
 *   listener does not have to re-parse.
 */
typedef void (*NostrPublishTransportListener)(NostrPublishTransport *transport,
                                         const gchar      *kind_hint,
                                         const gchar      *envelope_json,
                                         gpointer          user_data);

/**
 * NostrPublishTransportStateCallback:
 *
 * Called on transport state changes. `connected == TRUE` means the
 * websocket handshake has completed and REQ/EVENT frames may be sent.
 * `connected == FALSE` means the transport is disconnected — either
 * cleanly closed, awaiting reconnect, or failed. When @error is non-NULL
 * the caller may treat the disconnect as terminal for this attempt and
 * schedule a backoff retry from a fresh transport.
 */
typedef void (*NostrPublishTransportStateCallback)(NostrPublishTransport *transport,
                                              gboolean          connected,
                                              const GError     *error,
                                              gpointer          user_data);

/**
 * nostr_publish_transport_new_websocket:
 * @url: `wss://…` or `ws://…`
 *
 * Returns: (transfer full): a libsoup 3 SoupWebsocketConnection-backed
 *   transport (not yet connected). Reconnect uses exponential backoff
 *   (60 s → 60 min). The transport parses inbound NIP-01 envelopes and
 *   fans them out through the primary listener plus (if registered) the
 *   OK-frame and AUTH-frame callbacks.
 */
NOSTR_PUBLISH_API NostrPublishTransport *nostr_publish_transport_new_websocket(const gchar *url);

/**
 * nostr_publish_transport_new_websocket_unix:
 * @url: `ws://…` URL used for the HTTP upgrade request line and as the
 *   transport's identity (nostr_publish_transport_get_url())
 * @socket_path: filesystem path of an AF_UNIX stream socket to dial
 *   instead of resolving @url's host
 *
 * Same as nostr_publish_transport_new_websocket() but the connection is
 * made over a Unix-domain socket — the per-user session relay listens on
 * `$XDG_RUNTIME_DIR/nostr/relay.sock` and speaks NIP-01 over WebSocket.
 *
 * Returns: (transfer full): a not-yet-connected transport.
 */
NOSTR_PUBLISH_API
NostrPublishTransport *nostr_publish_transport_new_websocket_unix(const gchar *url,
                                                                  const gchar *socket_path);

/**
 * nostr_publish_transport_factory_websocket:
 * @relay_url: relay URL
 * @user_data: ignored
 *
 * A #NostrPublishTransportFactory that returns
 * nostr_publish_transport_new_websocket(@relay_url). Standalone publishers
 * (no relay-sync layer of their own) pass this to
 * nostr_publisher_set_transport_factory().
 */
NOSTR_PUBLISH_API
NostrPublishTransport *nostr_publish_transport_factory_websocket(const gchar *relay_url,
                                                                 gpointer     user_data);

/**
 * NostrPublishTransportOkCallback:
 * @event_id: hex64 event id from the OK envelope (never NULL)
 * @accepted: %TRUE if the relay accepted the event
 * @reason: (nullable): the relay's OK reason string; NIP-01 prefixes
 *   include `duplicate:`, `invalid:`, `blocked:`, `banned:`,
 *   `rate-limited:`, `restricted:`, `auth-required:`, `error:`
 *
 * Fired once per `["OK",...]` envelope. Publishers use this to record
 * per-relay ACKs against their in-flight rows without having to re-parse
 * every generic listener callback.
 */
typedef void (*NostrPublishTransportOkCallback)(NostrPublishTransport *transport,
                                           const gchar      *event_id,
                                           gboolean          accepted,
                                           const gchar      *reason,
                                           gpointer          user_data);

/**
 * nostr_publish_transport_set_ok_callback:
 *
 * Registers an OK-frame consumer. Passing @cb = NULL clears the binding.
 * Independent of nostr_publish_transport_set_listener() — both fire on OK.
 */
NOSTR_PUBLISH_API void nostr_publish_transport_set_ok_callback(NostrPublishTransport          *self,
                                        NostrPublishTransportOkCallback cb,
                                        gpointer                   user_data);

/**
 * NostrPublishTransportAuthCallback:
 * @challenge: server-issued NIP-42 challenge string (never NULL)
 *
 * Returns: (transfer full) (nullable): signed NIP-42 kind-22242 event
 *   JSON to send back in an `["AUTH", <event>]` envelope. Returning NULL
 *   silently drops the challenge; the transport does not retry AUTH.
 *   The callback runs on the transport's main context — implementations
 *   that need to make blocking IPC calls (DBus signer) must arrange for
 *   that themselves.
 */
typedef gchar *(*NostrPublishTransportAuthCallback)(NostrPublishTransport *transport,
                                               const gchar      *challenge,
                                               gpointer          user_data);

/**
 * nostr_publish_transport_set_auth_callback:
 *
 * Registers a NIP-42 AUTH signer. Passing @cb = NULL clears the binding
 * and reverts the transport to ignoring AUTH challenges (relays that
 * require AUTH will refuse subsequent EVENTs — the publisher classifies
 * those as permanent). Independent of the generic listener; a listener
 * still sees the raw envelope.
 */
NOSTR_PUBLISH_API void nostr_publish_transport_set_auth_callback(NostrPublishTransport            *self,
                                          NostrPublishTransportAuthCallback cb,
                                          gpointer                     user_data);

/**
 * nostr_publish_transport_new_fixture:
 * @url: (not nullable): identifier the transport surfaces via
 *   nostr_publish_transport_get_url()
 *
 * Returns: (transfer full): an in-process transport for tests. Callers
 *   drive it via nostr_publish_transport_fixture_deliver_frame() and read
 *   outbound frames back through nostr_publish_transport_fixture_take_sent().
 */
NOSTR_PUBLISH_API NostrPublishTransport *nostr_publish_transport_new_fixture(const gchar *url);

/**
 * nostr_publish_transport_set_listener:
 * @self: the transport
 * @listener: (nullable): callback fired for every decoded incoming frame
 * @user_data: opaque pointer passed to @listener
 *
 * Setting @listener replaces any previously installed callback.
 */
NOSTR_PUBLISH_API void nostr_publish_transport_set_listener(NostrPublishTransport         *self,
                                     NostrPublishTransportListener  listener,
                                     gpointer                  user_data);

NOSTR_PUBLISH_API void nostr_publish_transport_set_state_callback(NostrPublishTransport              *self,
                                           NostrPublishTransportStateCallback  cb,
                                           gpointer                       user_data);

/**
 * nostr_publish_transport_connect_async:
 *
 * Kicks off the connect. Success or failure is reported via the state
 * callback; there is no direct completion tag because the caller cares
 * about the on-going connection lifecycle, not just the first attempt.
 */
NOSTR_PUBLISH_API void nostr_publish_transport_connect_async(NostrPublishTransport *self);

/**
 * nostr_publish_transport_send_frame:
 * @frame_json: complete NIP-01 client frame (e.g.
 *   `["REQ","sub-1",{...}]`). Must be UTF-8. Ownership retained by
 *   caller.
 *
 * Returns: TRUE if the frame was accepted for sending; FALSE with @error
 *   set if the transport is not connected.
 */
NOSTR_PUBLISH_API gboolean nostr_publish_transport_send_frame(NostrPublishTransport *self,
                                       const gchar      *frame_json,
                                       GError          **error);

/** Closes the underlying connection and releases resources. */
NOSTR_PUBLISH_API void nostr_publish_transport_disconnect(NostrPublishTransport *self);

/** Returns TRUE while the transport is in the CONNECTED state. */
NOSTR_PUBLISH_API gboolean nostr_publish_transport_is_connected(NostrPublishTransport *self);

NOSTR_PUBLISH_API const gchar *nostr_publish_transport_get_url(NostrPublishTransport *self);

NOSTR_PUBLISH_API NostrPublishTransport *nostr_publish_transport_ref  (NostrPublishTransport *self);
NOSTR_PUBLISH_API void              nostr_publish_transport_unref(NostrPublishTransport *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NostrPublishTransport, nostr_publish_transport_unref)

/* ---- Fixture API (test-only) ----
 *
 * Available only for the fixture transport. Passing a websocket
 * transport is a programmer error; the fixture asserts on it. */

/**
 * nostr_publish_transport_fixture_deliver_frame:
 * @kind_hint: routing hint fed to the listener; typically `"EVENT"`,
 *   `"EOSE"`, `"OK"`, or `"AUTH"`
 * @envelope_json: full envelope, e.g. `["EVENT","sub",{...}]`
 *
 * Injects an incoming frame as if it came from the relay. The listener
 * is invoked synchronously on the caller's thread — most tests run in a
 * single-threaded harness so this keeps the assertions ordered.
 */
NOSTR_PUBLISH_API void nostr_publish_transport_fixture_deliver_frame(NostrPublishTransport *self,
                                              const gchar      *kind_hint,
                                              const gchar      *envelope_json);

/**
 * nostr_publish_transport_fixture_set_state:
 *
 * Drives the state callback. `error` may be NULL. Tests use this to
 * flip a fixture into "disconnected + terminal error" so the sync layer
 * takes its backoff path.
 */
NOSTR_PUBLISH_API void nostr_publish_transport_fixture_set_state(NostrPublishTransport *self,
                                          gboolean          connected,
                                          GError           *error);

/**
 * nostr_publish_transport_fixture_take_sent:
 * @out_len: (out) (nullable): number of returned frames
 *
 * Returns: (transfer full) (array length=out_len): outbound frames in
 *   send order, then resets the internal queue. Each element is a heap
 *   string; the array is g_strv-shaped (NULL-terminated) so callers may
 *   also treat it as a %GStrv.
 */
NOSTR_PUBLISH_API gchar **nostr_publish_transport_fixture_take_sent(NostrPublishTransport *self,
                                             gsize             *out_len);

G_END_DECLS
#endif /* NOSTR_PUBLISH_TRANSPORT_H */
