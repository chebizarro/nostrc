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

/* Upload @data to the first of @servers that accepts it. @out_url is the
 * server-reported descriptor URL (or <server>/<sha256>.<ext> when the
 * server omits it); @out_server is the server that took it. */
gboolean ns_blossom_upload(const gchar *const *servers,
                           NostrPublishSigner *signer,
                           const gchar        *pubkey_hex,
                           const gchar        *mime,
                           GBytes             *data,
                           const gchar        *sha256_hex,
                           gchar             **out_url,
                           gchar             **out_server,
                           GError            **error);

G_END_DECLS

#endif /* NS_BLOSSOM_H */
