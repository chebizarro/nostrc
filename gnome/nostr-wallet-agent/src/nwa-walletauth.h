/* nwa-walletauth.h - agent-generated NWC client keys ("nostr+walletauth")
 *
 * SPDX-License-Identifier: MIT
 *
 * With nostr+walletconnect:// pairing, whoever produced or relayed the link
 * also knows the NWC client secret and can use the wallet directly, outside
 * the agent's budgets; Unpair cannot revoke that (nostrc-prqu.12). Here the
 * agent generates the client keypair itself and asks the wallet for a
 * connection to that key:
 *
 *   nostr+walletauth://<client-pk>?relay=<r>…&name=<n>
 *       &request_methods=pay_invoice%20get_balance%20…
 *       &notification_types=payment_received%20payment_sent
 *       &pubkey=<client-pk>&state=<128-bit hex>
 *
 * (Alby Hub's form, which puts the client key in the authority, plus the
 * `pubkey` and `state` parameters of the NWC-08 draft / nips PR #1818.) The
 * wallet, once the user approves there, publishes its kind-13194 info event
 * with a `p` tag = the client key (optionally carrying a relay hint), and
 * NWC-08 wallets echo `state` in a `state` tag. The author of that event is
 * the wallet service key.
 *
 * An answer is a *candidate* when its id and signature verify, the author
 * is not the client key, it p-tags the client key, created_at lies in
 * [start - 60 s, now + 300 s] and its content lists at least one NWC
 * method. Then:
 *   - `state` tag present: must equal ours (else ignored) — "confirmed";
 *     accepted at once;
 *   - no `state` (Alby today): "unconfirmed"; accepted after a settle window
 *     unless a *different* author also answers in time (-> failed,
 *     NWA_WALLET_AUTH_CONFLICT) or a confirmed answer arrives (it wins).
 *     The same author answering twice (relays redeliver) is no conflict.
 * `state` travels inside the URI, so it binds an answer to this request but
 * does not authenticate the wallet against someone who saw the URI; the
 * agent therefore always has the user confirm the wallet (nwa-service.c).
 * A relay hint in the p tag is subscribed to in addition to ours, never
 * instead. The secret key never leaves this object except in the
 * nostr+walletconnect URI built for the keyring.
 */
#ifndef NWA_WALLETAUTH_H
#define NWA_WALLETAUTH_H

#include "nwa-nwc.h"

G_BEGIN_DECLS

#define NWA_WALLET_AUTH_MAX_RELAYS 5

typedef enum {
  NWA_WALLET_AUTH_TIMEOUT,
  NWA_WALLET_AUTH_CONFLICT, /* two different wallets answered */
  NWA_WALLET_AUTH_STOPPED,
} NwaWalletAuthFailure;

#define NWA_TYPE_WALLET_AUTH (nwa_wallet_auth_get_type())
G_DECLARE_FINAL_TYPE(NwaWalletAuth, nwa_wallet_auth, NWA, WALLET_AUTH, GObject)

/* @relays: 1..NWA_WALLET_AUTH_MAX_RELAYS ws(s):// URLs (deduplicated);
 * @name: shown by the wallet (<= 64 chars). @factory NULL = websocket. */
NwaWalletAuth *nwa_wallet_auth_new(const gchar *const *relays, const gchar *name,
                                   NwaTransportFactory factory, gpointer factory_data,
                                   GError **error);

const gchar *nwa_wallet_auth_get_uri(NwaWalletAuth *self);
const gchar *nwa_wallet_auth_get_client_pubkey(NwaWalletAuth *self);
const gchar *nwa_wallet_auth_get_state(NwaWalletAuth *self);

/* Defaults: 3000 ms settle window, 600 s overall. */
void nwa_wallet_auth_set_timing(NwaWalletAuth *self, guint settle_ms, guint timeout_s);
void nwa_wallet_auth_start(NwaWalletAuth *self);
void nwa_wallet_auth_stop(NwaWalletAuth *self);

/* nostr+walletconnect://<wallet_pk>?relay=<relay>&secret=<client secret>
 * — contains the secret: wipe after use. */
gchar *nwa_wallet_auth_build_nwc_uri(NwaWalletAuth *self, const gchar *wallet_pk, const gchar *relay);

/* Signals (exactly one of them, once, from an idle after the decision;
 * then the object is inert):
 *   "authorized" (const gchar *wallet_pubkey, const gchar *relay, gboolean state_confirmed)
 *   "failed"     (guint NwaWalletAuthFailure, const gchar *message) */

G_END_DECLS

#endif /* NWA_WALLETAUTH_H */
