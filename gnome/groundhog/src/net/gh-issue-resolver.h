/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GH_ISSUE_RESOLVER_H
#define GH_ISSUE_RESOLVER_H

#include <nostr-gtk-1.0/gn-nip34-issue-iface.h>
#include "gh-account-controller.h"

G_BEGIN_DECLS

/*
 * GhIssueResolver: Groundhog's GnIssueRepoResolver for the public NIP-34
 * issue form (nostrc-ewx8x). The only resolver the form may get (privacy
 * rule "issue-services" in tests/check_privacy.py), and it runs only after
 * the user accepts the form's lookup consent, which names its relays
 * (charter: no lookup by default).
 *
 * It asks the Groundhog discovery-relays (snapshotted when the form opens,
 * deduplicated, at most 16) with one URL-scoped REQ through GhRelayScope's
 * application transport, so through GhNet: the network mode, Tor isolation
 * and the dispatcher. NIP-42: GhAuthPolicy's CONTACT_DIRECTORY identity (an
 * ephemeral key if a relay demands AUTH, never the account). The filter is
 * {kinds:[30617], authors:[owner], #d:[repo id], limit:5}; only events the
 * scope verified are kept (at most 16), and the newest announcement by the
 * owner for that id wins (gn_issue_target_from_announcements()), whose
 * maintainers become p tags and whose relays join the publish set.
 *
 * The lookup completes when every relay sent EOSE or failed; the deadline
 * (15 s) only bounds a silent relay. An account switch or the form's
 * cancellation closes the REQ and finishes with G_IO_ERROR_CANCELLED.
 * Main context only.
 */

#define GH_TYPE_ISSUE_RESOLVER (gh_issue_resolver_get_type())
G_DECLARE_FINAL_TYPE(GhIssueResolver, gh_issue_resolver, GH, ISSUE_RESOLVER, GObject)

/* NULL when settings is NULL or has no usable discovery relay: the form
 * then offers no lookup. */
GnIssueRepoResolver *gh_issue_resolver_new(GhAccountController *accounts, GSettings *settings);

/* The relays this resolver asks, in order. */
const gchar *const *gh_issue_resolver_get_relays(GhIssueResolver *self);

/* Failure bound for a silent relay, in seconds (default 15, 1..120). */
void gh_issue_resolver_set_deadline(GhIssueResolver *self, guint seconds);

G_END_DECLS
#endif
