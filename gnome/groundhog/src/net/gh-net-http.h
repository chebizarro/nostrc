#ifndef GH_NET_HTTP_H
#define GH_NET_HTTP_H

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * GhNetHttp: Groundhog's HTTP client (privacy charter §2.1, §4.2). libsoup is
 * used here and in src/media/ only (the check_privacy libsoup boundary).
 * Every request is one GET the user asked for, made in the configured
 * network mode:
 *   - "system": the desktop proxy settings (GLib's default proxy resolver);
 *   - "none":   a direct connection;
 *   - "tor":    refused (G_IO_ERROR_NOT_SUPPORTED) until Tor support exists
 *               (G09): Groundhog never falls back to a direct connection.
 * The session has no cookie jar, cache, HSTS or authentication store, sends
 * no User-Agent, Referer or Accept-Language, follows no redirect (a 3xx is an
 * error), and waits at most GH_NET_HTTP_TIMEOUT_S for the server. Only
 * https URLs are fetched; plain http only to a loopback address (the test
 * fixtures). The body is read up to max_bytes and refused beyond it.
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

/* settings (nullable: "system") supplies network-mode, read at each request. */
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

/* The GhHttpTransport over a GhNetHttp passed as its data. */
const GhHttpTransport *gh_net_http_transport(void);

G_END_DECLS
#endif
