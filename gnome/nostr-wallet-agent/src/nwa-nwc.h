/* nwa-nwc.h - NIP-47 Nostr Wallet Connect client session
 *
 * SPDX-License-Identifier: MIT
 *
 * One paired wallet: the client keypair is the `secret` of the
 * nostr+walletconnect URI (never the user's Nostr identity; the signer
 * daemon is not involved). Relay I/O goes through libnostr-publish's
 * NostrPublishTransport (libsoup-3 WebSocket with reconnect in production,
 * the in-process fixture in tests).
 *
 * Wire protocol:
 *   REQ nwa-info  {"kinds":[13194],"authors":[W],"limit":1}
 *   REQ nwa-rsp   {"kinds":[23195,23196,23197],"authors":[W],"#p":[C],"since":…}
 *   EVENT kind 23194 content=enc({"method":…,"params":…})
 *         tags [p,W] [encryption,nip44_v2|nip04] [expiration,now+timeout]
 * Encryption follows the wallet's kind-13194 `encryption` tag: nip44_v2 when
 * advertised, otherwise NIP-04 (an info event without the tag means NIP-04).
 * Requests issued before the info event arrives wait for it (or for EOSE /
 * a short grace period, after which NIP-04 is assumed).
 *
 * Every inbound event must carry a valid id + signature from the wallet
 * pubkey; responses are matched to requests by their `e` tag.
 *
 * Errors: NWA_ERROR_WALLET (wallet answered with an error, message
 * "[CODE] text"), NWA_ERROR_RELAY (the request provably never reached a
 * relay that accepted it), NWA_ERROR_TIMEOUT (sent, no answer: the outcome
 * is UNKNOWN — a payment may still have happened).
 */
#ifndef NWA_NWC_H
#define NWA_NWC_H

#include <glib-object.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <nostr-publish/nostr-publish-transport.h>

G_BEGIN_DECLS

#define NWA_TYPE_NWC_CLIENT (nwa_nwc_client_get_type())
G_DECLARE_FINAL_TYPE(NwaNwcClient, nwa_nwc_client, NWA, NWC_CLIENT, GObject)

typedef NostrPublishTransport *(*NwaTransportFactory)(const gchar *relay_url, gpointer user_data);

/* @factory NULL = nostr_publish_transport_new_websocket(). */
NwaNwcClient *nwa_nwc_client_new(const gchar *nwc_uri,
                                 NwaTransportFactory factory, gpointer factory_data,
                                 GError **error);

void nwa_nwc_client_set_timeouts(NwaNwcClient *self, guint request_timeout_s, guint info_grace_ms);
void nwa_nwc_client_start(NwaNwcClient *self);
void nwa_nwc_client_stop(NwaNwcClient *self);

const gchar        *nwa_nwc_client_get_wallet_pubkey(NwaNwcClient *self);
const gchar        *nwa_nwc_client_get_client_pubkey(NwaNwcClient *self);
const gchar        *nwa_nwc_client_get_lud16(NwaNwcClient *self);
const gchar *const *nwa_nwc_client_get_relays(NwaNwcClient *self);
const gchar        *nwa_nwc_client_get_encryption(NwaNwcClient *self); /* "nip44_v2" | "nip04" */
const gchar *const *nwa_nwc_client_get_methods(NwaNwcClient *self);    /* NULL until info */
gboolean            nwa_nwc_client_supports(NwaNwcClient *self, const gchar *method);

/* @params: JSON object (transfer none) or NULL for {}. */
void      nwa_nwc_client_request_async(NwaNwcClient *self, const gchar *method, JsonObject *params,
                                       GCancellable *cancellable,
                                       GAsyncReadyCallback callback, gpointer user_data);
/* Returns the NIP-47 "result" node (transfer full; may be a JSON null). */
JsonNode *nwa_nwc_client_request_finish(NwaNwcClient *self, GAsyncResult *result, GError **error);

/* TRUE when @error proves the request had no effect at the wallet. */
gboolean  nwa_nwc_error_is_definite(const GError *error);

/* Signals:
 *   "notification" (const gchar *notification_type, const gchar *notification_json)
 *   "info-changed" () */

G_END_DECLS

#endif /* NWA_NWC_H */
