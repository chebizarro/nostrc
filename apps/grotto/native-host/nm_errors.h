/* nm_errors.h - NIP-07 bridge error vocabulary (nostrc-jjyp)
 *
 * Every failure the host reports to the extension carries one of these
 * stable string codes; the extension turns them into rejected promises
 * (see browser-extension/nip07/README.md "Error codes"). D-Bus errors from
 * org.nostr.Signer are folded into the same vocabulary here so the page
 * never sees daemon-internal error names.
 */
#ifndef APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_ERRORS_H
#define APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_ERRORS_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum {
  NM_ERR_INVALID_REQUEST = 0, /* malformed frame / request / params */
  NM_ERR_UNKNOWN_METHOD,      /* method not provided by any provider */
  NM_ERR_ORIGIN_DENIED,       /* origin fails the secure-origin policy */
  NM_ERR_REJECTED,            /* user (or remembered policy) said no */
  NM_ERR_RATE_LIMITED,        /* signer throttled the request */
  NM_ERR_NO_KEY,              /* signer has no identity configured */
  NM_ERR_NOT_FOUND,           /* requested data absent (internal; getRelays maps it to {}) */
  NM_ERR_SIGNER_UNAVAILABLE,  /* org.nostr.Signer not on the session bus */
  NM_ERR_TIMEOUT,             /* signer did not answer in time */
  NM_ERR_BUSY,                /* too many requests in flight / duplicate id */
  NM_ERR_TOO_LARGE,           /* request or response over the frame limit */
  NM_ERR_UNSUPPORTED,         /* method not offered (WebLN keysend/signMessage/...) or backend lacks it */
  NM_ERR_INTERNAL,            /* anything else */
  /* WebLN / org.nostr.Wallet1 (nm_webln.c) */
  NM_ERR_NOT_PAIRED,          /* no wallet is paired with the wallet agent */
  NM_ERR_WALLET_UNAVAILABLE,  /* org.nostr.Wallet1 not on the bus / wallet relay unreachable */
  NM_ERR_BUDGET_EXCEEDED,     /* over the site's daily budget and no one to ask */
  NM_ERR_WALLET_ERROR,        /* the wallet answered with a NIP-47 error */
  NM_ERR_N_CODES
} NmErrorCode;

/* Stable wire string for @code ("rejected", "timeout", ...). */
const gchar *nm_error_code_str(NmErrorCode code);

/* Default human-readable message for @code. */
const gchar *nm_error_default_message(NmErrorCode code);

/* Map a GError from a D-Bus call to org.nostr.Signer onto the bridge
 * vocabulary. Remote org.nostr.Signer.Error.* names are matched first,
 * then bus-level failures (service unknown, no reply, timeouts). */
NmErrorCode nm_error_from_dbus(const GError *error);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_NATIVE_HOST_NM_ERRORS_H */
