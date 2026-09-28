#ifndef GH_INBOX_LOOKUP_H
#define GH_INBOX_LOOKUP_H

#include "gh-account-relays.h"
#include "gh-inbox-resolver.h"

G_BEGIN_DECLS

/*
 * GhInboxLookup is the send-time GhInboxResolver: it resolves another user's
 * NIP-17 kind-10050 DM inbox relay list with a URL-scoped REQ. It does not
 * authenticate (no NIP-42 AUTH as the account on others' relays).
 *
 * Sources: the Groundhog discovery-relays setting plus the active account's
 * own NIP-65 read and write relays (GhAccountRelays), deduplicated and bounded
 * to 16 URLs. A lookup first waits (signal-driven) until the account's own
 * relay-list discovery has settled, so its read/write relays can be sources.
 *
 * One URL-scoped REQ {kinds:[10050], authors:[recipient]} goes to exactly those
 * sources. The lookup completes when every source has sent EOSE or failed
 * (connect error or CLOSED); the deadline only bounds a silent source and is
 * never a success signal. Only signed kind-10050 events authored by the
 * recipient are admitted; the newest wins (NIP-01: created_at, then lowest
 * id). Events dated more than 15 minutes in the future are ignored. Only ws(s)
 * "relay" tags count, deduplicated and capped at 16. If the newest list names
 * no usable relay, the result is EMPTY: an older list is never used instead,
 * and there is deliberately no kind-10002 fallback.
 *
 * Every lookup is bound to the account generation at its start: an account
 * switch or caller cancellation closes its REQ and the lookup finishes with
 * G_IO_ERROR_CANCELLED, never with a result. FOUND results are cached per
 * (recipient, account generation) for a few minutes; forget() drops one.
 *
 * Threading: main-context only, like the relay scope and account layers.
 */

#define GH_TYPE_INBOX_LOOKUP (gh_inbox_lookup_get_type())
G_DECLARE_FINAL_TYPE(GhInboxLookup, gh_inbox_lookup, GH, INBOX_LOOKUP, GObject)

/* transport NULL uses the gnostr relay transport. */
GhInboxLookup *gh_inbox_lookup_new(GhAccountController *accounts,
                                   GhAccountRelays *account_relays,
                                   GSettings *settings,
                                   const GhRelayTransport *transport,
                                   gpointer transport_data);
/* Failure bound for a source that neither answers nor fails, in seconds
 * (default 15, clamped to 1..120), measured from the REQ. */
void gh_inbox_lookup_set_deadline(GhInboxLookup *self, guint seconds);

G_END_DECLS
#endif
