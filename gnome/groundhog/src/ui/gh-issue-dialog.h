#ifndef GH_ISSUE_DIALOG_H
#define GH_ISSUE_DIALOG_H

#include <adwaita.h>
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>
#include "gh-account-controller.h"

G_BEGIN_DECLS
#define GH_TYPE_ISSUE_DIALOG (gh_issue_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhIssueDialog, gh_issue_dialog, GH, ISSUE_DIALOG, AdwDialog)

#define GH_ISSUE_TITLE_MAX_BYTES 640
#define GH_ISSUE_BODY_MAX_BYTES 16000
#define GH_ISSUE_DIAGNOSTICS_MAX_BYTES 8000

/* A canonical, validated issue draft. The assembled body already contains
 * every optional section (steps, expected/actual result, attachment URLs,
 * related commits, local diagnostics appendix) exactly as it is published. */
typedef struct {
  gchar *title;
  gchar *body;
  GStrv labels; /* "bug", "groundhog", then the user's labels */
} GhIssueDraft;

/* The one canonical builder used for both the review preview and Publish.
 * fields and diagnostics may be NULL. Never truncates: content over the
 * title/body/appendix limits, or malformed labels, commit IDs or attachment
 * URLs, fail with a translated G_IO_ERROR_INVALID_DATA message. */
GhIssueDraft *gh_issue_draft_new(const gchar *title, const gchar *description,
                                 const GnNip34IssueFieldsSnapshot *fields,
                                 const gchar *diagnostics, GError **error);
void gh_issue_draft_free(GhIssueDraft *draft);
gboolean gh_issue_draft_equal(const GhIssueDraft *a, const GhIssueDraft *b);
/* Unsigned kind-1621 JSON from the shared nip34_create_issue() builder. */
gchar *gh_issue_draft_to_unsigned_json(const GhIssueDraft *draft, const gchar *pubkey);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhIssueDraft, gh_issue_draft_free)

/* No network or signer work before the publication preview is confirmed.
 * Uses the same nip34_create_issue() library builder and repository address
 * as gnostr's bug-report dialog. Nothing is uploaded; local diagnostics are
 * added only when the per-issue checkbox is on, shown verbatim, and then
 * confirmed in the review. Publish signs exactly the reviewed event. */
GhIssueDialog *gh_issue_dialog_new(GhAccountController *accounts, GSettings *settings);
G_END_DECLS
#endif
