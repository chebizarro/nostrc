/* nwa-error.c - see nwa-error.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-error.h"

#include <gio/gio.h>

static const GDBusErrorEntry nwa_dbus_errors[] = {
  { NWA_ERROR_FAILED,          "org.nostr.Wallet1.Error.Failed" },
  { NWA_ERROR_INVALID_ARGS,    "org.nostr.Wallet1.Error.InvalidArgs" },
  { NWA_ERROR_NOT_PAIRED,      "org.nostr.Wallet1.Error.NotPaired" },
  { NWA_ERROR_DENIED,          "org.nostr.Wallet1.Error.Denied" },
  { NWA_ERROR_BUDGET_EXCEEDED, "org.nostr.Wallet1.Error.BudgetExceeded" },
  { NWA_ERROR_TIMEOUT,         "org.nostr.Wallet1.Error.Timeout" },
  { NWA_ERROR_WALLET,          "org.nostr.Wallet1.Error.WalletError" },
  { NWA_ERROR_RELAY,           "org.nostr.Wallet1.Error.RelayError" },
  { NWA_ERROR_UNSUPPORTED,     "org.nostr.Wallet1.Error.Unsupported" },
  { NWA_ERROR_RATE_LIMITED,    "org.nostr.Wallet1.Error.RateLimited" },
  { NWA_ERROR_KEYRING,         "org.nostr.Wallet1.Error.Keyring" },
  { NWA_ERROR_INTERACTION_REQUIRED, "org.nostr.Wallet1.Error.InteractionRequired" },
};

GQuark
nwa_error_quark(void)
{
  static gsize quark = 0;
  g_dbus_error_register_error_domain("nostr-wallet-agent-error", &quark,
                                     nwa_dbus_errors,
                                     G_N_ELEMENTS(nwa_dbus_errors));
  return (GQuark)quark;
}
