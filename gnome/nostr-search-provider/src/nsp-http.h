/*
 * nsp-http — bounded HTTPS GET for NIP-05 look-ups and avatar downloads.
 *
 * Both are driven by what someone else published (a typed NIP-05 domain,
 * a kind-0 `picture` URL), so every request:
 *   - must be https:// (every redirect hop too; redirects can be refused);
 *   - is refused at TCP-connect time if the peer is not a public unicast
 *     address (loopback, RFC 1918, CGNAT, link-local, ULA, multicast, …),
 *     before TLS or any request bytes, so a hostile profile cannot make
 *     the search provider probe the LAN or local services;
 *   - is capped in body size and time; the body is read incrementally
 *     and the transfer aborted as soon as the cap is exceeded.
 */
#ifndef NSP_HTTP_H
#define NSP_HTTP_H

#include <gio/gio.h>
#include <libsoup/soup.h>

G_BEGIN_DECLS

/* TRUE only for global unicast addresses. */
gboolean nsp_inet_address_is_public(GInetAddress *addr);

SoupSession *nsp_http_session_new(guint timeout_s);

void nsp_http_get_async(SoupSession *session, const char *url, const char *accept,
                        gsize max_bytes, gboolean follow_redirects, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data);
/* Returns the body; @content_type_out (optional) receives the media type. */
GBytes *nsp_http_get_finish(GAsyncResult *res, char **content_type_out, GError **error);

G_END_DECLS

#endif /* NSP_HTTP_H */
