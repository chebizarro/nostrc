#ifndef GNOSTR_ISSUE_ADAPTERS_H
#define GNOSTR_ISSUE_ADAPTERS_H

#include "gnostr-main-window.h"
#include <nostr-gtk-1.0/gn-nip34-issue-view.h>

G_BEGIN_DECLS

/* gnostr's "Report an Issue" (nostrc-8xfib.5): the portable nostr-gtk
 * GnNip34IssueView with gnostr's services. Behaviour kept from the former
 * GnostrBugReportDialog: the repository announcement is looked up (NDB, then
 * read relays) as soon as the form opens, files go to the Blossom server
 * below, the issue is signed and published by the main window to its write
 * relays plus the announced ones, an open-status event follows, editable
 * system info is included by default, and success closes the form with a
 * toast. */
#define GNOSTR_ISSUE_BLOSSOM_SERVER "https://blossom.sharegap.net"

GnNip34IssueView *gnostr_issue_view_new(GnostrMainWindow *window);

/* The services alone (also used by tests). */
GnIssuePublisher *gnostr_issue_publisher_new(GnostrMainWindow *window);
GnIssueUploader *gnostr_issue_uploader_new(const char *server_url);
GnIssueRepoResolver *gnostr_issue_resolver_new(void);

/* System info offered as the report's diagnostics. */
char *gnostr_issue_collect_system_info(void);

/**
 * Discover gnostr macOS diagnostic reports, newest first.
 *
 * If @directory is NULL, the platform default is used. Linux currently
 * returns an empty array for the default. Passing a directory explicitly is
 * supported on every platform for tests.
 *
 * Returns: (transfer full) (element-type utf8): paths owned by the array.
 */
GPtrArray *gnostr_issue_discover_crash_logs(const char *directory);

G_END_DECLS
#endif
