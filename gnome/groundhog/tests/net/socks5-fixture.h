/* H3 (privacy charter §9.1): a local SOCKS5 server (RFC 1928, RFC 1929) on
 * 127.0.0.1 for the Tor-mode tests. It records every CONNECT: the
 * authentication method, ATYP, the destination exactly as the client sent it
 * (a host name for ATYP 0x03: the proof that the client did not resolve it),
 * the port, and the username and password. It can refuse every CONNECT
 * (reply 0x05). Otherwise it connects to the destination itself, directly:
 * an IP literal as given, a host name on 127.0.0.1 at domain_port (or the
 * requested port when 0), and splices the two connections, so a Tor-mode
 * client reaches the local wire relay (H2) or HTTP server through it.
 * Runs on the thread-default main context it was made on. */
#ifndef GH_TEST_SOCKS5_FIXTURE_H
#define GH_TEST_SOCKS5_FIXTURE_H

#include <gio/gio.h>

typedef struct {
  guint8 method;   /* 0x00 none, 0x02 username/password */
  guint8 atyp;     /* 0x01 IPv4, 0x03 domain name, 0x04 IPv6 */
  gchar *host;     /* the name (0x03) or the address as text */
  guint16 port;
  gchar *username; /* NULL without username/password authentication */
  gchar *password;
} Socks5Request;

typedef struct _Socks5Fixture Socks5Fixture;

Socks5Fixture *socks5_fixture_new(void);
void socks5_fixture_free(Socks5Fixture *fixture);
/* "127.0.0.1:<port>" */
const gchar *socks5_fixture_address(Socks5Fixture *fixture);
void socks5_fixture_set_refuse(Socks5Fixture *fixture, gboolean refuse);
void socks5_fixture_set_domain_port(Socks5Fixture *fixture, guint16 port);
/* Socks5Request, in arrival order. */
GPtrArray *socks5_fixture_requests(Socks5Fixture *fixture);
/* Connections that sent a SOCKS5 greeting (CONNECTs and probes alike). */
guint socks5_fixture_greetings(Socks5Fixture *fixture);
/* A counter a test can wait on: CONNECT requests seen so far. */
const guint *socks5_fixture_connect_count(Socks5Fixture *fixture);

#endif
