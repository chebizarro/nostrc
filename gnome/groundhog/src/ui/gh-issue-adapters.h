#ifndef GH_ISSUE_ADAPTERS_H
#define GH_ISSUE_ADAPTERS_H

#include <adwaita.h>
#include <nostr-gtk-1.0/gn-nip34-issue-view.h>
#include "gh-account-controller.h"
#if GH_ISSUE_UPLOADS
#include "gh-blossom-client.h"
#else
typedef struct _GhBlossomClient GhBlossomClient;
#endif

G_BEGIN_DECLS

/* Groundhog's "Report an Issue" (nostrc-8xfib.5): the portable nostr-gtk
 * GnNip34IssueView with Groundhog services.
 *  - Publisher: signs exactly the reviewed event through Grotto (the account
 *    controller, bound to its generation) and publishes with GhRelayPublish,
 *    which follows the network mode, Tor isolation and the dispatcher.
 *  - Uploader: GhIssueUploader (src/net/gh-issue-uploader.h) over blossom,
 *    the account's attachment client, when it has a server with a public
 *    host; files are uploaded only after the form's consent, which names the
 *    servers and every file. Without one (or with blossom NULL) the form
 *    offers no files and says how to choose a server; no built-in server is
 *    substituted.
 *  - No repository resolver: no lookup; the built-in nostrc address is used.
 *  - Local diagnostics are offered only when collection is on, off for every
 *    new report and previewed verbatim.
 * Nothing is signed or sent before the review is confirmed. */
GnNip34IssueView *gh_issue_dialog_new(GhAccountController *accounts, GSettings *settings,
                                      GhBlossomClient *blossom);

/* The publisher alone, for tests. */
GnIssuePublisher *gh_issue_publisher_new(GhAccountController *accounts);

G_END_DECLS
#endif
