/* ns-blossom.h - Blossom (BUD-01/02) upload for nostr-share
 *
 * SPDX-License-Identifier: MIT
 *
 * Thin layer over libhanami's hanami_blossom_client_* (the same client
 * nh_porthome_blossom wraps). We do not reuse nh_porthome_blossom_*
 * itself: it is gated behind NOSTR_HOMED_ENABLE_PORTHOME_EXPERIMENTAL,
 * lives in nostr-homed's private static library, sends every blob as
 * application/octet-stream (right for encrypted chunks, wrong for a
 * photo someone will view in a client) and caps blobs at 16 MiB.
 * Kind-24242 auth events are signed through org.nostr.Signer.
 */
#ifndef NS_BLOSSOM_H
#define NS_BLOSSOM_H

#include <glib.h>
#include <nostr-publish/nostr-publish.h>

G_BEGIN_DECLS

/* https:// — or, with @allow_loopback_http (tests only), http:// to a
 * loopback IP literal. */
gboolean ns_blossom_server_ok(const gchar *server, gboolean allow_loopback_http);

/* Who authorises the upload (the BUD-02 kind-24242 event). */
typedef enum {
  NS_BLOSSOM_AUTH_ACCOUNT,    /* the user's key, through @signer */
  NS_BLOSSOM_AUTH_THROWAWAY,  /* a key made for this upload and wiped: the
                               * server cannot link the blob to the user
                               * (--private, nostrc-k95e) */
} NsBlossomAuth;

/* Upload @data to the first of @servers that accepts it. @out_url is the
 * server-reported descriptor URL (or <server>/<sha256>.<ext> when the
 * server omits it); @out_server is the server that took it.
 * @out_auth_refused (optional): TRUE when every server refused the
 * authorisation (401/403) rather than failing otherwise.
 * @allow_loopback_http: also accept http:// servers on loopback IP
 * literals (tests only); otherwise https:// only. */
gboolean ns_blossom_upload(const gchar *const *servers,
                           NostrPublishSigner *signer,
                           const gchar        *pubkey_hex,
                           NsBlossomAuth       auth,
                           gboolean            allow_loopback_http,
                           const gchar        *mime,
                           GBytes             *data,
                           const gchar        *sha256_hex,
                           gchar             **out_url,
                           gchar             **out_server,
                           gboolean           *out_auth_refused,
                           GError            **error);

G_END_DECLS

#endif /* NS_BLOSSOM_H */
