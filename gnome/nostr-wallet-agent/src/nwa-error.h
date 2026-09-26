/* nwa-error.h - error domain for nostr-wallet-agent (org.nostr.Wallet1)
 *
 * SPDX-License-Identifier: MIT
 *
 * Every code maps 1:1 onto an org.nostr.Wallet1.Error.* D-Bus error name
 * (registered in nwa_error_quark()), so returning a GError from a method
 * handler yields the documented D-Bus error on the wire.
 */
#ifndef NWA_ERROR_H
#define NWA_ERROR_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NWA_ERROR_FAILED,
  NWA_ERROR_INVALID_ARGS,
  NWA_ERROR_NOT_PAIRED,
  NWA_ERROR_DENIED,
  NWA_ERROR_BUDGET_EXCEEDED,
  NWA_ERROR_TIMEOUT,
  NWA_ERROR_WALLET,
  NWA_ERROR_RELAY,
  NWA_ERROR_UNSUPPORTED,
  NWA_ERROR_RATE_LIMITED,
  NWA_ERROR_KEYRING,
} NwaError;

#define NWA_ERROR (nwa_error_quark())
GQuark nwa_error_quark(void);

G_END_DECLS

#endif /* NWA_ERROR_H */
