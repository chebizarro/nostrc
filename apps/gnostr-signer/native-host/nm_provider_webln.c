/* nm_provider_webln.c - window.webln (WebLN) provider: STUB (nostrc-jjyp)
 *
 * TODO(nostrc-yka8): forward to org.nostr.Wallet1 once nostr-wallet-agent
 * (gnome/nostr-wallet-agent/) lands. The intended mapping, mirroring the
 * NIP-07 provider:
 *
 *   webln.enable()              no D-Bus call; per-origin grant lives in the
 *                               extension (same prompt path as getPublicKey)
 *   webln.getInfo()             Wallet1.GetInfo            (non-interactive)
 *   webln.sendPayment(bolt11)   Wallet1.PayInvoice(bolt11, app_id)
 *                               (interactive: the wallet agent owns the
 *                               spend-approval dialog, keyed on app_id)
 *   webln.makeInvoice(args)     Wallet1.MakeInvoice(amount_msat, memo, app_id)
 *   webln.signMessage / keysend not planned
 *
 * app_id is the canonical page origin exactly as for NIP-07; the router
 * already enforces the secure-origin policy for this provider. Until the
 * wallet agent exists every method answers "unsupported" so the extension
 * can keep window.webln undefined (it does not inject it at all today; see
 * browser-extension/nip07/src/providers/webln.js).
 */
#include "nm_router.h"

static const gchar *const webln_methods[] = {
  "webln.enable", "webln.getInfo", "webln.sendPayment", "webln.makeInvoice",
  NULL
};

static void webln_dispatch(NmRequest *req) {
  nm_request_reply_error(req, NM_ERR_UNSUPPORTED,
                         "WebLN is not available yet (org.nostr.Wallet1, nostrc-yka8)");
}

const NmProvider nm_provider_webln = {
  .name = "webln",
  .methods = webln_methods,
  .requires_origin = TRUE,
  .dispatch = webln_dispatch,
};
