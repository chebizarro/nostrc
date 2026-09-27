#ifndef NIP55L_DBUS_ERRORS_H
#define NIP55L_DBUS_ERRORS_H

/* Canonical D-Bus error names for GNostr Signer (NIP-55L) */
#define ORG_NOSTR_SIGNER_ERR_PERMISSION     "org.nostr.Signer.Error.PermissionDenied"
#define ORG_NOSTR_SIGNER_ERR_RATELIMIT      "org.nostr.Signer.Error.RateLimited"
#define ORG_NOSTR_SIGNER_ERR_APPROVAL       "org.nostr.Signer.Error.ApprovalDenied"
/* Typed approval failures (nip55l 0.5.0, additive). Sent only to a bus
 * connection that called EnableTypedApprovalErrors; every other caller keeps
 * receiving ApprovalDenied for these cases, as before 0.5.0. */
#define ORG_NOSTR_SIGNER_ERR_APPROVAL_TIMEOUT "org.nostr.Signer.Error.ApprovalTimedOut"
#define ORG_NOSTR_SIGNER_ERR_NO_APPROVER      "org.nostr.Signer.Error.NoApprovalAgent"
#define ORG_NOSTR_SIGNER_ERR_INVALID_INPUT  "org.nostr.Signer.Error.InvalidInput"
#define ORG_NOSTR_SIGNER_ERR_INTERNAL       "org.nostr.Signer.Error.Internal"
#define ORG_NOSTR_SIGNER_ERR_NO_KEY         "org.nostr.Signer.Error.NoKeyConfigured"
/* GetRelays: nothing configured. Callers fall back to their own relays. */
#define ORG_NOSTR_SIGNER_ERR_NOT_FOUND      "org.nostr.Signer.Error.NotFound"
/* GetRelays: $XDG_CONFIG_HOME/nostr/relays.conf exists but is malformed. */
#define ORG_NOSTR_SIGNER_ERR_INVALID_CONFIG "org.nostr.Signer.Error.InvalidConfig"

#endif /* NIP55L_DBUS_ERRORS_H */
