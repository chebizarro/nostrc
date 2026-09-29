#ifndef GH_NET_SESSION_H
#define GH_NET_SESSION_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * GhNetSession: the app's network mode (privacy charter §4.2, G09), the one
 * place that decides how Groundhog reaches the network. It follows the
 * network-mode and tor-socks-address settings:
 *
 *  - SYSTEM ("system", the default): web requests (GhNetHttp) use the
 *    desktop proxy settings (g_proxy_resolver_get_default()). Relay
 *    connections are made directly by the GNostrRelay transport, as before
 *    G09: libwebsockets has no proxy support on the target systems (§0.3),
 *    and the Preferences copy says so.
 *  - NONE ("none"): everything connects directly.
 *  - TOR ("tor"): every relay connection and every web request goes through
 *    the SOCKS5 proxy at tor-socks-address with the destination's host name
 *    (remote DNS; nothing is resolved locally), and nothing ever falls back to
 *    a direct connection: the proxy resolver lists the proxy only. Each
 *    connection purpose gets its own SOCKS username and password (Tor's
 *    IsolateSOCKSAuth gives each its own circuits), derived from an
 *    isolation label and a per-process random salt, so no key or URL reaches
 *    the Tor daemon. .onion hosts are reachable only in this mode.
 *
 * Tor reachability: in Tor mode the session checks that a SOCKS5 server
 * answers at the address (a greeting only: nothing is asked to connect
 * anywhere) when the mode is chosen and whenever a Tor connection fails, and
 * reports it as tor-state; the main window shows "Can't reach Tor" while it
 * is UNREACHABLE. Connections keep trying through the proxy only.
 *
 * "changed" is emitted after the mode or the Tor address changed; the relay
 * dispatcher (gh-relay-net.h) closes every connection made in the old mode
 * and reconnects scopes in the new one. Any other change of the settings is
 * ignored. Main context only, except the thread-safe functions marked so.
 */

typedef enum {
  GH_NET_MODE_SYSTEM,
  GH_NET_MODE_NONE,
  GH_NET_MODE_TOR
} GhNetMode;

typedef enum {
  GH_NET_TOR_OFF,         /* not in Tor mode */
  GH_NET_TOR_CHECKING,    /* checking the SOCKS port */
  GH_NET_TOR_READY,       /* a SOCKS5 server answered, or a connection worked */
  GH_NET_TOR_UNREACHABLE  /* nothing answered at tor-socks-address */
} GhNetTorState;

GType gh_net_mode_get_type(void);
GType gh_net_tor_state_get_type(void);
#define GH_TYPE_NET_MODE (gh_net_mode_get_type())
#define GH_TYPE_NET_TOR_STATE (gh_net_tor_state_get_type())

#define GH_NET_TOR_DEFAULT_PORT 9050

/* The mode a network-mode value names. Anything but "system" and "none" is
 * TOR, the strictest mode (fail closed). */
GhNetMode gh_net_mode_from_string(const gchar *mode);

/* TRUE for a host name ending in .onion (case-insensitive). */
gboolean gh_net_host_is_onion(const gchar *host);
/* TRUE for a loopback IP literal (the test fixtures). */
gboolean gh_net_host_is_loopback(const gchar *host);

/* Whether a relay URL may be contacted in mode (PD-5, PT-5, NT-8): wss://
 * always, except a .onion host outside Tor mode; ws:// only to a loopback
 * address or, in Tor mode, to a .onion host. G_IO_ERROR_PERMISSION_DENIED
 * with a plain-language reason otherwise. */
gboolean gh_net_relay_url_allowed(GhNetMode mode, const gchar *url, GError **error);

/* The SOCKS username for an isolation label: 16 hex digits of
 * SHA-256(label || salt), with a random salt made once per process. The
 * password is the next 16 digits. Thread-safe. */
gchar *gh_net_isolation_username(const gchar *isolation);

/* The proxy resolver one connection uses in mode (a new reference):
 * SYSTEM: the desktop's default resolver; NONE: a resolver with no proxy;
 * TOR: a resolver whose only proxy, for every URI, is
 * socks5://<user>:<password>@<tor_address> for isolation (NULL: a random
 * label, so a fresh circuit). NULL with G_IO_ERROR_INVALID_ARGUMENT for a
 * tor_address that is not host:port. Thread-safe. */
GProxyResolver *gh_net_proxy_resolver_new(GhNetMode mode, const gchar *tor_address,
                                          const gchar *isolation, GError **error);

#define GH_TYPE_NET_SESSION (gh_net_session_get_type())
G_DECLARE_FINAL_TYPE(GhNetSession, gh_net_session, GH, NET_SESSION, GObject)

/* settings (nullable) supplies network-mode and tor-socks-address; without
 * it the session is in SYSTEM mode until gh_net_session_set_mode(). */
GhNetSession *gh_net_session_new(GSettings *settings);

/* For a session without settings (tests): switch mode and Tor address. */
void gh_net_session_set_mode(GhNetSession *self, GhNetMode mode, const gchar *tor_address);

/* Thread-safe. */
GhNetMode gh_net_session_get_mode(GhNetSession *self);
gchar *gh_net_session_dup_tor_address(GhNetSession *self);
/* Bumped by every mode or Tor address change. */
guint64 gh_net_session_get_serial(GhNetSession *self);
/* The resolver of one connection in the current mode (see
 * gh_net_proxy_resolver_new()). */
GProxyResolver *gh_net_session_dup_resolver(GhNetSession *self, const gchar *isolation,
                                            GError **error);
/* A Tor connection attempt's outcome, from any thread: a connection that
 * worked marks Tor READY; a failed one re-checks the SOCKS port. Ignored
 * outside Tor mode. */
void gh_net_session_report(GhNetSession *self, gboolean connected);

GhNetTorState gh_net_session_get_tor_state(GhNetSession *self);
/* Checks the SOCKS port again now (Tor mode only). */
void gh_net_session_check_tor(GhNetSession *self);

/* The session the relay dispatcher and GhNetHttp report to, set by
 * gh_relay_net_install(); NULL when none is installed. Thread-safe; returns
 * a new reference. */
GhNetSession *gh_net_session_dup_default(void);
void gh_net_session_set_default(GhNetSession *session);

G_END_DECLS
#endif
