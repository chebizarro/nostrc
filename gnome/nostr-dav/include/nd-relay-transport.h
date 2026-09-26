/* nd-relay-transport.h - nostr-dav names for the libnostr-publish transport
 *
 * SPDX-License-Identifier: MIT
 *
 * The single-relay NIP-01 transport (libsoup 3 WebSocket backend + test
 * fixture) moved to libnostr-publish (nostr-publish-transport.h, bead
 * nostrc-tmsc). nostr-dav's relay-sync layer and its tests keep the
 * historic NdRelayTransport spelling through these aliases; semantics
 * are documented in the library header.
 */
#ifndef ND_RELAY_TRANSPORT_H
#define ND_RELAY_TRANSPORT_H

#include <nostr-publish/nostr-publish-transport.h>

G_BEGIN_DECLS

typedef NostrPublishTransport              NdRelayTransport;
typedef NostrPublishTransportFactory       NdRelayTransportFactory;
typedef NostrPublishTransportListener      NdRelayTransportListener;
typedef NostrPublishTransportStateCallback NdRelayTransportStateCallback;
typedef NostrPublishTransportOkCallback    NdRelayTransportOkCallback;
typedef NostrPublishTransportAuthCallback  NdRelayTransportAuthCallback;

#define nd_relay_transport_new_websocket        nostr_publish_transport_new_websocket
#define nd_relay_transport_new_fixture          nostr_publish_transport_new_fixture
#define nd_relay_transport_set_ok_callback      nostr_publish_transport_set_ok_callback
#define nd_relay_transport_set_auth_callback    nostr_publish_transport_set_auth_callback
#define nd_relay_transport_set_listener         nostr_publish_transport_set_listener
#define nd_relay_transport_set_state_callback   nostr_publish_transport_set_state_callback
#define nd_relay_transport_connect_async        nostr_publish_transport_connect_async
#define nd_relay_transport_send_frame           nostr_publish_transport_send_frame
#define nd_relay_transport_disconnect           nostr_publish_transport_disconnect
#define nd_relay_transport_is_connected         nostr_publish_transport_is_connected
#define nd_relay_transport_get_url              nostr_publish_transport_get_url
#define nd_relay_transport_ref                  nostr_publish_transport_ref
#define nd_relay_transport_unref                nostr_publish_transport_unref
#define nd_relay_transport_fixture_deliver_frame nostr_publish_transport_fixture_deliver_frame
#define nd_relay_transport_fixture_set_state    nostr_publish_transport_fixture_set_state
#define nd_relay_transport_fixture_take_sent    nostr_publish_transport_fixture_take_sent

G_DEFINE_AUTOPTR_CLEANUP_FUNC(NdRelayTransport, nostr_publish_transport_unref)

G_END_DECLS
#endif /* ND_RELAY_TRANSPORT_H */
