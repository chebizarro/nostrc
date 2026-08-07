#ifndef GNOSTR_BUG_REPORT_ASSEMBLY_H
#define GNOSTR_BUG_REPORT_ASSEMBLY_H

#include <glib.h>

typedef struct _NostrEvent NostrEvent;

G_BEGIN_DECLS

/**
 * Parse comma-separated issue labels. Whitespace is stripped and duplicate
 * labels are removed while preserving order.
 *
 * Returns: (transfer full): a NULL-terminated string array.
 */
GStrv gnostr_bug_report_parse_labels(const char *labels_csv);

/**
 * Assemble the reviewable dialog fields into the Markdown issue body.
 */
char *gnostr_bug_report_assemble_content(const char *description,
                                         const GPtrArray *crash_log_urls,
                                         const GPtrArray *attachment_urls,
                                         const char *system_info,
                                         const char *related_commits);

/**
 * Assemble content and build the unsigned NIP-34 issue event used by the
 * dialog. Related commit SHAs remain human-readable content because NIP-34
 * does not define issue-level commit-reference tags.
 */
NostrEvent *gnostr_bug_report_build_issue_event(
    const char *repo_owner_pubkey_hex,
    const char *repo_id,
    const char *subject,
    const char *description,
    const char *labels_csv,
    const char *const *maintainers,
    const GPtrArray *crash_log_urls,
    const GPtrArray *attachment_urls,
    const char *system_info,
    const char *related_commits);

G_END_DECLS

#endif /* GNOSTR_BUG_REPORT_ASSEMBLY_H */
