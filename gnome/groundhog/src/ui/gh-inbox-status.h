#ifndef GH_INBOX_STATUS_H
#define GH_INBOX_STATUS_H

#include "gh-dm-inbox.h"
#include "gh-status.h"

G_BEGIN_DECLS

/* The status of the private-message receive path, from the account's relay
 * lists (GhAccountRelays) and its inbox subscription (GhDmInbox):
 *  - while the inbox has no relays to subscribe on, the relay-list lookup
 *    says why: nothing configured (NO_SOURCES), still looking, the lookup
 *    relays failed (LOOKUP_FAILED), or the account has no inbox list
 *    (MISSING, charter §7.15 #7);
 *  - an inbox ERROR with a subscription is every inbox relay failing
 *    (UNREACHABLE, #8); without one it is a local failure (ERROR). */
GhStatusInbox gh_inbox_status_map(GhDmInboxState inbox, GhAccountRelaysState relays,
                                  gboolean inbox_has_relays);

/* Keeps status's inbox input (and its explanation) in step with inbox and
 * relays until status is finalized. */
void gh_inbox_status_attach(GhStatus *status, GhDmInbox *inbox, GhAccountRelays *relays);

G_END_DECLS
#endif
