#ifndef GH_NET_HTTP_H
#define GH_NET_HTTP_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * GhNetHttp: Groundhog's HTTP client (privacy charter §2.1, §4.2). libsoup is
 * used here and in src/media/ only (the check_privacy libsoup boundary).
 * Every request is one the user asked for (a GET; for G21 attachments also a
 * Blossom PUT, gh_net_http_send_async()), made in the configured
 * network mode (gh-net-session.h):
 *   - "system": the desktop proxy settings (GLib's default proxy resolver);
 *   - "none":   a direct connection;
 *   - "tor":    through the SOCKS5 proxy at tor-socks-address, with the host
 *               name resolved by the proxy (never locally), fresh SOCKS
 *               credentials (so a fresh Tor circuit) for every request, and
 *               never a direct connection: an unusable address or an
 *               unreachable proxy is an error. The outcome is reported to the
 *               app's GhNetSession (gh_net_session_dup_default()).
 * A change of network-mode ends every request made in another mode, with
 * G_IO_ERROR_CONNECTION_CLOSED (never G_IO_ERROR_CANCELLED, which stays the
 * caller's own cancellation): nothing made before the user chose Tor still
 * completes directly afterwards.
 * The session has no cookie jar, cache, HSTS or authentication store, sends
 * no User-Agent, Referer or Accept-Language, follows no redirect (a 3xx is an
 * error), resumes no TLS session and leaves none to resume (gh-net-tls.h),
 * and waits at most GH_NET_HTTP_TIMEOUT_S for the server. Only
 * https URLs are fetched; plain http only to a loopback address (the test
 * fixtures) or, in Tor mode, to a .onion host. A .onion host is refused
 * (G_IO_ERROR_PERMISSION_DENIED) outside Tor mode, so it never reaches the
 * local DNS. The body is read up to max_bytes and refused beyond it.
 * Main context only.
 */

#define GH_NET_HTTP_TIMEOUT_S 10

/* The fetch seam, so callers are tested without sockets: get_async reports
 * the body of a 200 response (at most max_bytes) or an error. */
typedef struct {
  void (*get_async)(gpointer data, const gchar *uri, gsize max_bytes,
                    GCancellable *cancellable, GAsyncReadyCallback callback,
                    gpointer user_data);
  GBytes *(*get_finish)(gpointer data, GAsyncResult *result, GError **error);
} GhHttpTransport;

#define GH_TYPE_NET_HTTP (gh_net_http_get_type())
G_DECLARE_FINAL_TYPE(GhNetHttp, gh_net_http, GH, NET_HTTP, GObject)

/* settings (nullable: "system") supplies network-mode and tor-socks-address,
 * read at each request. */
GhNetHttp *gh_net_http_new(GSettings *settings);

void gh_net_http_get_async(GhNetHttp *self, const gchar *uri, gsize max_bytes,
                           GCancellable *cancellable, GAsyncReadyCallback callback,
                           gpointer user_data);
/* As gh_net_http_get_async() with accept as the request's one Accept value
 * (NULL: "application/json"), e.g. NIP-11's "application/nostr+json". The
 * result is finished with gh_net_http_get_finish(). */
void gh_net_http_get_accept_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                                  gsize max_bytes, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);
GBytes *gh_net_http_get_finish(GhNetHttp *self, GAsyncResult *result, GError **error);

/* nostrc-qi5e: as gh_net_http_get_accept_async(), for a URL someone else
 * chose (a G21 attachment download), which may only reach a public address
 * (gh_net_address_is_public()). In System and No Proxy modes this is checked
 * where the connection is made. The request has a session of its own whose
 * one target is the URL's host, and every address the host name resolves to
 * as the connection is attempted, and any literal address, is checked before
 * it is dialled. Refused addresses are skipped, so the addresses checked are
 * the addresses connected to: a DNS answer cannot change between a check and
 * the connection (DNS rebinding), and no kept connection is reused. A host
 * with no public address fails with G_IO_ERROR_PERMISSION_DENIED after no
 * connection. In Tor mode the proxy resolves the name and connects, and Tor
 * exits refuse private addresses. In System mode through a desktop proxy the
 * proxy does both too, and is trusted with the user's own network. Finished
 * with gh_net_http_get_finish(). Up to three redirects are followed (W33:
 * Blossom servers such as Primal's answer with a 302 to their storage
 * host), each hop checked again exactly like the first: https only, .onion
 * only through Tor, a public address. The other GETs never follow one
 * (NIP-05 forbids it). */
void gh_net_http_get_public_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                                  gsize max_bytes, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data);

/* As gh_net_http_get_public_async(), but a longer answer is not an error:
 * its first max_bytes are returned and the rest is never read. For web pages
 * whose <head> is all that is wanted (link previews, nostrc-p15n5.7: GitHub's
 * pages are ~400 KiB with og: tags in the first 32 KiB). */
void gh_net_http_get_public_prefix_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                                         gsize max_bytes, GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data);
/* The prefix form of gh_net_http_get_accept_async() (no redirects, any
 * address the mode allows); tests use it to stand in for the public one. */
void gh_net_http_get_accept_prefix_async(GhNetHttp *self, const gchar *uri, const gchar *accept,
                                         gsize max_bytes, GCancellable *cancellable,
                                         GAsyncReadyCallback callback, gpointer user_data);

/* Whether a download may reach address: FALSE for loopback, private,
 * link-local, CGNAT (100.64/10), 192.0.0/24, benchmarking (198.18/15),
 * "this network" (0/8), reserved (240/4), multicast and unspecified IPv4;
 * for IPv6 anything outside global unicast 2000::/3 (so unique local,
 * link-local, multicast) and 2001:db8::/32. IPv6 forms carrying an IPv4
 * address are judged by that address: IPv4-compatible ::a.b.c.d, mapped
 * ::ffff:a.b.c.d, SIIT ::ffff:0:a.b.c.d, NAT64 64:ff9b::/96 and
 * 64:ff9b:1::/48 (every RFC 6052 layout the address fits must be public),
 * 6to4 2002::/16, and Teredo 2001::/32 (server and client). */
gboolean gh_net_address_is_public(GInetAddress *address);

/* The connectable a public-only request connects through (a GNetworkAddress
 * for hostname, port and scheme): its enumerate() yields only the resolved
 * or literal addresses gh_net_address_is_public() accepts, and ends with
 * G_IO_ERROR_PERMISSION_DENIED when it refused every one. Exposed for its
 * tests. */
GSocketConnectable *gh_net_public_address_new(const gchar *hostname, guint16 port,
                                              const gchar *scheme);

/* G21 (Blossom, charter §6): one request with a method, headers and a body,
 * made exactly like a GET above (network mode, Tor isolation, URL policy, no
 * redirect, cookie, cache or TLS resumption, max_bytes on the answer's body).
 * Any 2xx answer succeeds (its body, possibly empty); any other status fails
 * with GH_NET_HTTP_ERROR whose code is the HTTP status (so it is never
 * mistaken for Groundhog's own refusals, which stay G_IO_ERROR_*), the
 * server's X-Reason (printable ASCII, cut) in the message. Nothing of the
 * request is logged. */
#define GH_NET_HTTP_ERROR gh_net_http_error_quark()
GQuark gh_net_http_error_quark(void);
typedef struct {
  const gchar *method;        /* "GET", "PUT" or "HEAD"; NULL: GET */
  const gchar *uri;
  const gchar *accept;        /* NULL: "application/json" */
  const gchar *authorization; /* the Authorization header value, or NULL */
  const gchar *content_type;  /* of body; NULL: application/octet-stream */
  const gchar *x_sha256;      /* optional BUD-02 X-SHA-256 of body */
  GBytes *body;               /* the request body, or NULL */
  gsize max_bytes;            /* the answer's body cap (> 0) */
  /* W25: the URL is someone else's choice (e.g. a group's media server):
   * connect only to public addresses, checked where the connection is made,
   * exactly as gh_net_http_get_public_async(). */
  gboolean public_only;
} GhNetHttpRequest;

void gh_net_http_send_async(GhNetHttp *self, const GhNetHttpRequest *request,
                            GCancellable *cancellable, GAsyncReadyCallback callback,
                            gpointer user_data);
GBytes *gh_net_http_send_finish(GhNetHttp *self, GAsyncResult *result, GError **error);

/* The GhHttpTransport over a GhNetHttp passed as its data. */
const GhHttpTransport *gh_net_http_transport(void);

G_END_DECLS
#endif
