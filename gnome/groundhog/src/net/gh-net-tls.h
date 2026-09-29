#ifndef GH_NET_TLS_H
#define GH_NET_TLS_H

#include <libsoup/soup.h>

G_BEGIN_DECLS

/*
 * No TLS session resumption (privacy charter PD-6; W16 review B1).
 *
 * glib-networking keeps ONE process-wide TLS session cache for client
 * connections, keyed by the server's host name alone: not by proxy, SOCKS
 * credentials, SoupSession, account or network mode. A connection that may
 * resume stores the server's session ticket there, and the next connection to
 * that host, whatever its purpose, presents it (a TLS 1.3 pre_shared_key).
 * The server can then link the two: two Tor circuits kept apart by their
 * SOCKS credentials, two accounts, or a Tor connection and the IP address of
 * an earlier direct one.
 *
 * gh_net_tls_no_resumption() connects to message's network-event and, at
 * G_SOCKET_CLIENT_TLS_HANDSHAKING, sets session-resumption-enabled to FALSE
 * on every GTlsClientConnection the message makes, so Groundhog never puts
 * a ticket in the cache. Every SoupMessage Groundhog makes is passed here, in
 * every network mode (tests/check_privacy.py, rule tls-resumption).
 *
 * glib-networking looks a ticket up whatever that property says. This holds
 * only while nothing else in the process stores tickets there, that is,
 * while every GIO TLS client connection in Groundhog is a libsoup message
 * passed here. Today that is so: GNostrRelay's libwebsockets has its own
 * TLS, and check_privacy refuses g_tls_client_connection_new and
 * g_socket_client_set_tls in src/.
 */
void gh_net_tls_no_resumption(SoupMessage *message);

G_END_DECLS
#endif
