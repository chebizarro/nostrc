#ifndef GH_RELAY_SOUP_H
#define GH_RELAY_SOUP_H

#include <gio/gio.h>

#include "gh-relay-publish.h"
#include "gh-relay-scope.h"

G_BEGIN_DECLS

/*
 * The libsoup-3 relay transport (privacy charter §4.1 D3 = A, G09): NIP-01
 * client framing with libnostr's envelope parser and serializer over
 * soup_session_websocket_connect_async(), behind the existing
 * GhRelayTransport / GhRelayPublishTransport seams.
 *
 * Each handle (one scope URL, or one publish URL) owns its own SoupSession:
 * no cookie jar, cache, HSTS or authentication store, no User-Agent, no
 * Origin, and no connection shared with any other handle. TLS sessions are
 * never resumed or left to resume: glib-networking's session cache is one
 * per process, keyed by host name, whatever the SoupSession, so every
 * WebSocket message turns resumption off (gh-net-tls.h, PD-6).
 * All of its connections use the GProxyResolver it was opened with (NULL:
 * direct), so a SOCKS5 resolver sends the relay's host name to the proxy
 * (remote DNS) and nothing else is tried: libsoup/GIO never fall back to a
 * direct connection when the only proxy fails.
 *
 * Scope handles keep one live REQ (fresh subscription id per REQ) and
 * reconnect after a lost connection with a jittered backoff (1 s doubling to
 * 30 s, times U(0.5, 1.5); charter S5); a failed dial is reported as
 * GH_RELAY_NOTICE_ERROR, a lost connection as GH_RELAY_NOTICE_DISCONNECTED.
 * Publish handles make one attempt: the EVENT is sent once the WebSocket is
 * open, and a failed dial or a connection lost before the relay's OK is
 * gh_relay_publish_failed(). Everything runs on the thread-default main
 * context current when the handle was opened (the scope's or publish's
 * owning context); close stops every callback before it returns.
 *
 * status (nullable) hears whether each dial succeeded, so the network session
 * can tell "Tor is unreachable" apart from a relay being down.
 */

typedef void (*GhRelaySoupStatusFunc)(gpointer data, gboolean connected);

gpointer gh_relay_soup_scope_open(GhRelayScope *scope, const gchar *url,
                                  const NostrFilters *filters, GProxyResolver *resolver,
                                  GhRelaySoupStatusFunc status, gpointer status_data,
                                  GError **error);
void gh_relay_soup_scope_close(gpointer handle);
gboolean gh_relay_soup_scope_send_auth(gpointer handle, const gchar *signed_event_json,
                                       GError **error);
void gh_relay_soup_scope_resubscribe(gpointer handle);

gpointer gh_relay_soup_publish_open(GhRelayPublish *publish, const gchar *url,
                                    const gchar *event_json, GProxyResolver *resolver,
                                    GhRelaySoupStatusFunc status, gpointer status_data,
                                    GError **error);
void gh_relay_soup_publish_close(gpointer handle);
gboolean gh_relay_soup_publish_send_auth(gpointer handle, const gchar *signed_event_json,
                                         GError **error);
gboolean gh_relay_soup_publish_resend(gpointer handle, GError **error);

/* The same as transport vtables whose transport_data is a GProxyResolver
 * (NULL: direct), for tests and callers that choose the transport
 * themselves (NT-10 parity). */
extern const GhRelayTransport gh_relay_soup_transport;
extern const GhRelayAuthTransport gh_relay_soup_auth_transport;
extern const GhRelayPublishTransport gh_relay_soup_publish_transport;
extern const GhRelayPublishAuthTransport gh_relay_soup_publish_auth_transport;

G_END_DECLS
#endif
