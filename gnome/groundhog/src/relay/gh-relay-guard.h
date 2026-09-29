#ifndef GH_RELAY_GUARD_H
#define GH_RELAY_GUARD_H

#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * The network-mode guard of a build without G09 (GROUNDHOG_HAVE_TOR=0: no
 * libsoup, so no Tor transport; nostrc-6v0i). Privacy charter P5: Tor
 * missing means stop, never connect directly.
 *
 * gh_relay_guard_install() makes it the transport of every scope and publish
 * made with gh_relay_scope_new() / gh_relay_publish_new(). It follows the
 * network-mode setting:
 *
 *  - "system" and "none" connect directly through the GNostrRelay transport,
 *    as such a build always did;
 *  - "tor", or any value this build does not know, connects to nothing:
 *    a scope URL reports GH_RELAY_NOTICE_ERROR with
 *    GH_RELAY_GUARD_REFUSED_MESSAGE and a publish URL fails at once. Nothing
 *    is resolved or dialled;
 *  - a .onion relay is refused in every mode (G_IO_ERROR_PERMISSION_DENIED):
 *    dialling it directly would ask the local DNS for it.
 *
 * When the setting changes to a refusing mode, every open connection is
 * closed on its own context: each scope URL is told DISCONNECTED and then
 * ERROR, and each publish URL still waiting for its OK fails. When it
 * changes back, every refused scope URL connects. Changes between "system"
 * and "none" change nothing (both connect directly).
 *
 * Install on the context the settings object was made on, before any scope
 * or publish exists (gh-app-services.c does so first thing);
 * gh_relay_guard_install(NULL) restores the GNostrRelay defaults.
 */

#define GH_RELAY_GUARD_REFUSED_MESSAGE \
  "Tor isn't available in this build; Groundhog won't connect until you choose another " \
  "network setting"

/* TRUE for the modes this build can connect in ("system", "none"). */
gboolean gh_relay_guard_mode_allowed(const gchar *mode);

/* The guard's decision for one connection: refused (with a reason) in a
 * refusing mode, and for a .onion relay in any mode. */
gboolean gh_relay_guard_url_allowed(const gchar *mode, const gchar *url, GError **error);

void gh_relay_guard_install(GSettings *settings);

/* The guard's vtables (transport_data unused). */
extern const GhRelayTransport gh_relay_guard_transport;
extern const GhRelayAuthTransport gh_relay_guard_auth_transport;
extern const GhRelayPublishTransport gh_relay_guard_publish_transport;
extern const GhRelayPublishAuthTransport gh_relay_guard_publish_auth_transport;

/* Tests only: the direct transports the guard opens in an allowed mode
 * (GNostrRelay by default). Any NULL argument restores that default for its
 * half. Set before any scope or publish exists. */
void gh_relay_guard_set_direct_transports(const GhRelayTransport *scope,
                                          const GhRelayAuthTransport *scope_auth,
                                          const GhRelayPublishTransport *publish,
                                          const GhRelayPublishAuthTransport *publish_auth);

G_END_DECLS
#endif
