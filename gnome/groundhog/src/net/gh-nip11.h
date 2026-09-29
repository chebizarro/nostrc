#ifndef GH_NIP11_H
#define GH_NIP11_H

#include "gh-net-http.h"

#include <gio/gio.h>

G_BEGIN_DECLS

/*
 * The NIP-11 relay key of a NIP-29 group relay (privacy charter §8.2 G20a,
 * qp24.12.2): the "self" field of the relay information document. NIP-29
 * group metadata (39000-39003) is only trusted when signed by that key.
 * NIP-11's "pubkey" is the administrator's contact key, not the relay's: it
 * is never used, so a relay without "self" has no key (GH_NIP11_ERROR_NO_KEY,
 * GH_NIP29_RELAY_KEY_UNAVAILABLE: its group state stays unverified) rather
 * than letting a person speak for its groups.
 *
 * Why NIP-11 and not trust-on-first-use of a 39000 author: a 39000 carries no
 * proof that its signer is the relay; on a relay that stores foreign 39000s,
 * whoever published first would be pinned. The document comes from the relay
 * itself, over TLS for a wss:// relay (the certificate is validated for the
 * relay's host), so it is as trustworthy as the relay connection that NIP-29
 * already trusts to enforce the group. The key is pinned per group once
 * fetched and fetched again only when a snapshot arrives signed by another
 * key (rotation builds a new group state; nothing signed by the old key
 * survives).
 *
 * Privacy and bounds: one GET through GhNetHttp (gh-net-http.h, the app's
 * one HTTP client: the network-mode setting, no redirect, cookie, cache,
 * User-Agent or Referer, a size cap) of the relay's own URL with wss mapped
 * to https (same host, port and path) and "Accept: application/nostr+json".
 * A ws:// relay's document is not fetched: there is no plaintext fallback, so
 * such a relay has no verified key (GH_NIP11_ERROR_PLAINTEXT) and its group
 * state stays unverified. Only loopback ws:// addresses (the test relays)
 * map to http, as GhNetHttp allows for its fixtures. The service fetches only
 * for a group the user joined or opened, never for others.
 */

#define GH_NIP11_ERROR gh_nip11_error_quark()
GQuark gh_nip11_error_quark(void);

typedef enum {
  GH_NIP11_ERROR_INVALID_URL,  /* not a ws(s) relay URL with a host */
  GH_NIP11_ERROR_PLAINTEXT,    /* a ws:// relay off loopback: nothing is fetched */
  GH_NIP11_ERROR_MALFORMED,    /* not JSON, or not a JSON object */
  GH_NIP11_ERROR_NO_KEY        /* "self" is missing or not a hex key */
} GhNip11Error;

#define GH_NIP11_MAX_RESPONSE (64 * 1024)

/* The document URL of relay_url (wss -> https, loopback ws -> http); NULL
 * with GH_NIP11_ERROR_INVALID_URL for anything but a ws(s) URL with a host
 * and without credentials, GH_NIP11_ERROR_PLAINTEXT for ws:// off loopback. */
gchar *gh_nip11_document_url(const gchar *relay_url, GError **error);
/* The 64 lowercase hex key of a NIP-11 document's "self". */
gchar *gh_nip11_parse_relay_key(const gchar *document, gssize length, GError **error);

/* Fetches relay_url's document through http and returns its key (see
 * above). GhNetHttp's errors (G_IO_ERROR: a redirect or other status, a
 * response over GH_NIP11_MAX_RESPONSE, a network mode without a connection
 * path) are passed on. Main context only. */
void gh_nip11_fetch_relay_key_async(GhNetHttp *http, const gchar *relay_url,
                                    GCancellable *cancellable, GAsyncReadyCallback callback,
                                    gpointer user_data);
gchar *gh_nip11_fetch_relay_key_finish(GAsyncResult *result, GError **error);

G_END_DECLS
#endif
