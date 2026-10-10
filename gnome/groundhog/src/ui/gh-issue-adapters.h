#ifndef GH_ISSUE_ADAPTERS_H
#define GH_ISSUE_ADAPTERS_H

#include <adwaita.h>
#include <nostr-gtk-1.0/gn-nip34-issue-view.h>
#include "gh-account-controller.h"

G_BEGIN_DECLS

/* Groundhog's "Report an Issue" (nostrc-8xfib.5): the portable nostr-gtk
 * GnNip34IssueView with Groundhog services.
 *  - Publisher: signs exactly the reviewed event through Grotto (the account
 *    controller, bound to its generation) and publishes with GhRelayPublish,
 *    which follows the network mode, Tor isolation and the dispatcher.
 *  - No uploader and no repository resolver: Groundhog never uploads files
 *    and makes no lookup; the built-in nostrc address is used.
 *  - Local diagnostics are offered only when collection is on, off for every
 *    new report and previewed verbatim.
 * Nothing is signed or sent before the review is confirmed. */
GnNip34IssueView *gh_issue_dialog_new(GhAccountController *accounts, GSettings *settings);

/* The publisher alone, for tests. */
GnIssuePublisher *gh_issue_publisher_new(GhAccountController *accounts);

G_END_DECLS
#endif
