#ifndef GH_ISSUE_DIALOG_H
#define GH_ISSUE_DIALOG_H

#include <adwaita.h>
#include "gh-account-controller.h"

G_BEGIN_DECLS
#define GH_TYPE_ISSUE_DIALOG (gh_issue_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhIssueDialog, gh_issue_dialog, GH, ISSUE_DIALOG, AdwDialog)

/* No network or signer work before the publication preview is confirmed.
 * Uses the same nip34_create_issue() library builder and repository address
 * as gnostr's bug-report dialog. No automatic logs, uploads or diagnostics. */
GhIssueDialog *gh_issue_dialog_new(GhAccountController *accounts, GSettings *settings);
G_END_DECLS
#endif
