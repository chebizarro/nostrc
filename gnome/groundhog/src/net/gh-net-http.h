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
  GBytes *body;               /* the request body, or NULL */
  gsize max_bytes;            /* the answer's body cap (> 0) */
} GhNetHttpRequest;

void gh_net_http_send_async(GhNetHttp *self, const GhNetHttpRequest *request,
                            GCancellable *cancellable, GAsyncReadyCallback callback,
                            gpointer user_data);
GBytes *gh_net_http_send_finish(GhNetHttp *self, GAsyncResult *result, GError **error);

/* The GhHttpTransport over a GhNetHttp passed as its data. */
const GhHttpTransport *gh_net_http_transport(void);

G_END_DECLS
#endif
