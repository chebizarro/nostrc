#ifndef GH_RELAY_NET_H
#define GH_RELAY_NET_H

#include "gh-net-session.h"
#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

/*
 * The network-mode relay dispatcher (privacy charter §4.1, §4.2, G09).
 *
 * gh_relay_net_install() makes it the transport of every scope and publish
 * made with gh_relay_scope_new() / gh_relay_publish_new() (the whole app:
 * inbox, discovery, directory, outbox, NIP-29), and makes session the one
 * GhNetHttp reports Tor failures to. For each connection it:
 *
 *  - refuses what the mode does not allow (gh_net_relay_url_allowed(): a
 *    .onion host outside Tor mode, ws:// off loopback and .onion), before
 *    anything is resolved or dialled;
 *  - in Tor mode, opens the libsoup transport (gh-relay-soup.h) with a SOCKS5
 *    resolver whose credentials come from the scope's or publish's isolation
 *    label and account generation (NT-6). There is no code path from Tor mode
 *    to the GNostrRelay transport or to any direct connection;
 *  - in System and No Proxy modes, opens the GNostrRelay transport, as before
 *    G09 (libwebsockets cannot use a proxy; see gh-net-session.h).
 *
 * When the session's mode or Tor address changes, every connection opened in
 * the old one is closed at once (on its own context) before anything else
 * happens on it; then each scope URL is told DISCONNECTED and reconnects in
 * the new mode (or reports ERROR if the new mode refuses it), and each
 * publish URL still waiting for its OK fails (CONNECTION_FAILED; the outbox
 * decides about a retry). Handles on other threads' contexts are torn down on
 * those contexts.
 *
 * A scope or publish made while no session is installed uses GNostrRelay.
 * One made with the dispatcher after gh_relay_net_install(NULL) refuses to
 * connect. Install before any scope or publish is made (gh-app-services.c
 * does so first thing).
 */

void gh_relay_net_install(GhNetSession *session);

/* The dispatcher's vtables (transport_data unused); gh_relay_net_install()
 * installs them as the defaults. */
extern const GhRelayTransport gh_relay_net_transport;
extern const GhRelayAuthTransport gh_relay_net_auth_transport;
extern const GhRelayPublishTransport gh_relay_net_publish_transport;
extern const GhRelayPublishAuthTransport gh_relay_net_publish_auth_transport;

G_END_DECLS
#endif
