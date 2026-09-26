/* nss-nip11.h — NIP-11 relay information documents.
 * SPDX-License-Identifier: MIT
 *
 * The document is remote, untrusted JSON: every string is length-capped
 * and control characters are dropped before it reaches a label, and the
 * HTTP body is capped at NSS_NIP11_MAX_BYTES. Callers still escape markup.
 */
#ifndef NSS_NIP11_H
#define NSS_NIP11_H

#include <gio/gio.h>

G_BEGIN_DECLS

#define NSS_NIP11_MAX_BYTES    (64 * 1024)
#define NSS_NIP11_MAX_STRING   256
#define NSS_NIP11_MAX_NIPS     128

#define NSS_NIP11_ERROR (nss_nip11_error_quark())
GQuark nss_nip11_error_quark(void);
typedef enum {
  NSS_NIP11_ERROR_PARSE = 1,
  NSS_NIP11_ERROR_HTTP,
  NSS_NIP11_ERROR_TOO_LARGE,
  NSS_NIP11_ERROR_TIMEOUT,
  NSS_NIP11_ERROR_BAD_URL,
} NssNip11Error;

typedef struct {
  gchar  *name;
  gchar  *description;
  gchar  *software;
  gchar  *version;
  gchar  *contact;
  gchar  *pubkey;
  GArray *supported_nips;   /* gint, sorted, unique */
} NssNip11Info;

void          nss_nip11_info_free(NssNip11Info *info);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NssNip11Info, nss_nip11_info_free)

/* Parse a NIP-11 document. Unknown members are ignored; wrong-typed known
 * members are treated as absent; non-integer NIPs are skipped. Fails only
 * when @json is not a JSON object. */
NssNip11Info *nss_nip11_parse(const gchar *json, gssize len, GError **error);

/* "1, 11, 42" (NULL/empty → "") */
gchar        *nss_nip11_format_nips(const NssNip11Info *info);

/* ws(s)://host/path → http(s)://host/path. */
gchar        *nss_nip11_http_url(const gchar *relay_url, GError **error);

/* GET with Accept: application/nostr+json on @session (libsoup 3; may be
 * NULL for a private one), bounded by @timeout_sec and the body cap. */
void          nss_nip11_fetch_async(gpointer             soup_session,
                                    const gchar         *relay_url,
                                    guint                timeout_sec,
                                    GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);
NssNip11Info *nss_nip11_fetch_finish(GAsyncResult *result, GError **error);

G_END_DECLS

#endif /* NSS_NIP11_H */
