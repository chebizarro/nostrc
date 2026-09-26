/* nd-relay-transport.h - Single-relay transport abstraction
 *
 * SPDX-License-Identifier: MIT
 *
 * The relay subscribe + publish paths (plan Track 2 D4/D5) sit on top of
 * a NIP-01 WebSocket transport. This header defines an abstract
 * interface so:
 *   1. The production build can back it with libsoup 3's
 *      SoupWebsocketConnection (no libnostr, libwebsockets, or nsync
 *      dependency added to the nostr-dav service).
 *   2. Tests can inject a deterministic in-process transport that
 *      records outbound REQ/EVENT/CLOSE frames and injects EVENT/EOSE/OK
 *      replies without a real socket.
 *
 * The interface is deliberately message-oriented rather than
 * connection-oriented: transports are single-use per relay URL and
 * managed by NdRelaySync.
 */
#ifndef ND_RELAY_TRANSPORT_H
#define ND_RELAY_TRANSPORT_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _NdRelayTransport NdRelayTransport;

/**
 * NdRelayTransportFactory:
 * @relay_url: URL the caller wants a transport for
 * @user_data: opaque pointer supplied to the factory registrant
 *
 * Returns: (transfer full) (nullable): a transport that resolves to
 *   @relay_url, or NULL when the factory refuses the URL.
 */
typedef NdRelayTransport *(*NdRelayTransportFactory)(const gchar *relay_url,
                                                     gpointer     user_data);

/**
 * NdRelayTransportListener:
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
typedef void (*NdRelayTransportListener)(NdRelayTransport *transport,
                                         const gchar      *kind_hint,
                                         const gchar      *envelope_json,
                                         gpointer          user_data);

/**
 * NdRelayTransportStateCallback:
 *
 * Called on transport state changes. `connected == TRUE` means the
 * websocket handshake has completed and REQ/EVENT frames may be sent.
 * `connected == FALSE` means the transport is disconnected — either
 * cleanly closed, awaiting reconnect, or failed. When @error is non-NULL
 * the caller may treat the disconnect as terminal for this attempt and
 * schedule a backoff retry from a fresh transport.
 */
typedef void (*NdRelayTransportStateCallback)(NdRelayTransport *transport,
                                              gboolean          connected,
                                              const GError     *error,
                                              gpointer          user_data);

/**
 * nd_relay_transport_new_websocket:
 * @url: `wss://…` or `ws://…`
 *
 * Returns: (transfer full): a libsoup-backed transport (not yet
 *   connected). Currently a scaffold — production WebSocket wiring lands
 *   in a follow-up bead. The scaffold's connect method reports
 *   %G_IO_ERROR_NOT_SUPPORTED so a nostr-dav build without the follow-up
 *   still passes `-Werror` and refuses to appear healthy.
 */
NdRelayTransport *nd_relay_transport_new_websocket(const gchar *url);

/**
 * nd_relay_transport_new_fixture:
 * @url: (not nullable): identifier the transport surfaces via
 *   nd_relay_transport_get_url()
 *
 * Returns: (transfer full): an in-process transport for tests. Callers
 *   drive it via nd_relay_transport_fixture_deliver_frame() and read
 *   outbound frames back through nd_relay_transport_fixture_take_sent().
 */
NdRelayTransport *nd_relay_transport_new_fixture(const gchar *url);

/**
 * nd_relay_transport_set_listener:
 * @self: the transport
 * @listener: (nullable): callback fired for every decoded incoming frame
 * @user_data: opaque pointer passed to @listener
 *
 * Setting @listener replaces any previously installed callback.
 */
void nd_relay_transport_set_listener(NdRelayTransport         *self,
                                     NdRelayTransportListener  listener,
                                     gpointer                  user_data);

void nd_relay_transport_set_state_callback(NdRelayTransport              *self,
                                           NdRelayTransportStateCallback  cb,
                                           gpointer                       user_data);

/**
 * nd_relay_transport_connect_async:
 *
 * Kicks off the connect. Success or failure is reported via the state
 * callback; there is no direct completion tag because the caller cares
 * about the on-going connection lifecycle, not just the first attempt.
 */
void nd_relay_transport_connect_async(NdRelayTransport *self);

/**
 * nd_relay_transport_send_frame:
 * @frame_json: complete NIP-01 client frame (e.g.
 *   `["REQ","sub-1",{...}]`). Must be UTF-8. Ownership retained by
 *   caller.
 *
 * Returns: TRUE if the frame was accepted for sending; FALSE with @error
 *   set if the transport is not connected.
 */
gboolean nd_relay_transport_send_frame(NdRelayTransport *self,
                                       const gchar      *frame_json,
                                       GError          **error);

/** Closes the underlying connection and releases resources. */
void nd_relay_transport_disconnect(NdRelayTransport *self);

/** Returns TRUE while the transport is in the CONNECTED state. */
gboolean nd_relay_transport_is_connected(NdRelayTransport *self);

const gchar *nd_relay_transport_get_url(NdRelayTransport *self);

NdRelayTransport *nd_relay_transport_ref  (NdRelayTransport *self);
void              nd_relay_transport_unref(NdRelayTransport *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdRelayTransport, nd_relay_transport_unref)

/* ---- Fixture API (test-only) ----
 *
 * Available only for the fixture transport. Passing a websocket
 * transport is a programmer error; the fixture asserts on it. */

/**
 * nd_relay_transport_fixture_deliver_frame:
 * @kind_hint: routing hint fed to the listener; typically `"EVENT"`,
 *   `"EOSE"`, `"OK"`, or `"AUTH"`
 * @envelope_json: full envelope, e.g. `["EVENT","sub",{...}]`
 *
 * Injects an incoming frame as if it came from the relay. The listener
 * is invoked synchronously on the caller's thread — most tests run in a
 * single-threaded harness so this keeps the assertions ordered.
 */
void nd_relay_transport_fixture_deliver_frame(NdRelayTransport *self,
                                              const gchar      *kind_hint,
                                              const gchar      *envelope_json);

/**
 * nd_relay_transport_fixture_set_state:
 *
 * Drives the state callback. `error` may be NULL. Tests use this to
 * flip a fixture into "disconnected + terminal error" so the sync layer
 * takes its backoff path.
 */
void nd_relay_transport_fixture_set_state(NdRelayTransport *self,
                                          gboolean          connected,
                                          GError           *error);

/**
 * nd_relay_transport_fixture_take_sent:
 * @out_len: (out) (nullable): number of returned frames
 *
 * Returns: (transfer full) (array length=out_len): outbound frames in
 *   send order, then resets the internal queue. Each element is a heap
 *   string; the array is g_strv-shaped (NULL-terminated) so callers may
 *   also treat it as a %GStrv.
 */
gchar **nd_relay_transport_fixture_take_sent(NdRelayTransport *self,
                                             gsize             *out_len);

G_END_DECLS
#endif /* ND_RELAY_TRANSPORT_H */
