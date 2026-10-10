/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * NIP-34 issue (kind 1621) reporting model shared by gnostr and Groundhog
 * (nostrc-8xfib.5). GLib and libnip34 only: no GTK, no network, no app code.
 *
 * One canonical, bounded draft builder is used for both the review preview
 * and the published event, so the event a user confirmed is the event that is
 * signed. Nothing is ever trimmed to fit: oversize or malformed input is
 * refused with a translated G_IO_ERROR_INVALID_DATA message.
 */
#ifndef GN_NIP34_ISSUE_H
#define GN_NIP34_ISSUE_H

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _NostrEvent NostrEvent;

#define GN_ISSUE_TITLE_MAX_BYTES 640
#define GN_ISSUE_BODY_MAX_BYTES 16000
#define GN_ISSUE_DIAGNOSTICS_MAX_BYTES 8000
#define GN_ISSUE_MAX_USER_LABELS 16
#define GN_ISSUE_LABEL_MAX_BYTES 64
#define GN_ISSUE_MAX_RELAYS 16
#define GN_ISSUE_ATTACHMENT_MAX_BYTES (25 * 1024 * 1024)

/* Structured report fields, as edited in GnNip34IssueFields. Every member is
 * nullable free text; gn_issue_draft_new() validates and assembles them. */
typedef struct {
  gchar *steps;
  gchar *expected;
  gchar *actual;
  gchar *labels;          /* comma-separated */
  gchar *related_commits; /* full commit object IDs, body text only */
  gchar *attachment_urls; /* manual web references; never fetched */
} GnNip34IssueFieldsSnapshot;
void gn_nip34_issue_fields_snapshot_free(GnNip34IssueFieldsSnapshot *snapshot);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNip34IssueFieldsSnapshot, gn_nip34_issue_fields_snapshot_free)

/* ---- Target repository (kind 30617 announcement address) ---- */

typedef struct {
  gchar *owner_hex;   /* 64-char hex announcement author */
  gchar *repo_id;     /* announcement d tag */
  GStrv maintainers;  /* additional maintainer pubkeys (p tags); never NULL */
  GStrv relays;       /* announced relays; never NULL */
} GnIssueTarget;

#define GN_TYPE_ISSUE_TARGET (gn_issue_target_get_type())
GType gn_issue_target_get_type(void);
GnIssueTarget *gn_issue_target_new(const gchar *owner_hex, const gchar *repo_id,
                                   const gchar *const *maintainers,
                                   const gchar *const *relays);
GnIssueTarget *gn_issue_target_copy(const GnIssueTarget *target);
void gn_issue_target_free(GnIssueTarget *target);
gboolean gn_issue_target_equal(const GnIssueTarget *a, const GnIssueTarget *b);
/* "30617:<owner_hex>:<repo_id>" */
gchar *gn_issue_target_get_address(const GnIssueTarget *target);
/* Parses kind-30617 announcements (compact event JSON). Only events authored
 * by hint->owner_hex with d tag hint->repo_id count; the newest created_at
 * wins. Returns NULL when none match. n_events < 0: events is NULL-terminated. */
GnIssueTarget *gn_issue_target_from_announcements(const GnIssueTarget *hint,
                                                  const gchar *const *events,
                                                  gssize n_events);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnIssueTarget, gn_issue_target_free)

/* ---- Draft ---- */

/* Input to the canonical builder. Every member is nullable. */
typedef struct {
  const gchar *title;
  const gchar *description;
  const GnNip34IssueFieldsSnapshot *fields;
  /* Host-supplied text appended verbatim in a fenced block under
   * diagnostics_heading (default "Diagnostics"); at most
   * GN_ISSUE_DIAGNOSTICS_MAX_BYTES of UTF-8. */
  const gchar *diagnostics;
  const gchar *diagnostics_heading;
  /* Always-present labels placed before the user's labels (e.g. "bug"). */
  const gchar *const *required_labels;
  /* Public URLs returned by a GnIssueUploader, one section each. */
  const gchar *const *crash_log_urls;
  const gchar *const *attachment_urls;
  /*< private >*/
  gpointer padding[4];
} GnIssueDraftInput;

typedef struct {
  gchar *title;
  gchar *body;   /* assembled Markdown, exactly as published */
  GStrv labels;  /* required labels, then the user's, de-duplicated */
} GnIssueDraft;

#define GN_TYPE_ISSUE_DRAFT (gn_issue_draft_get_type())
GType gn_issue_draft_get_type(void);
GnIssueDraft *gn_issue_draft_new(const GnIssueDraftInput *input, GError **error);
GnIssueDraft *gn_issue_draft_copy(const GnIssueDraft *draft);
void gn_issue_draft_free(GnIssueDraft *draft);
gboolean gn_issue_draft_equal(const GnIssueDraft *a, const GnIssueDraft *b);
/* Unsigned kind-1621 event from nip34_create_issue(): a, p (owner and
 * maintainers), subject, alt, t and NIP-32 l tags. pubkey is nullable. */
NostrEvent *gn_issue_draft_build_event(const GnIssueDraft *draft,
                                       const GnIssueTarget *target,
                                       const gchar *pubkey);
gchar *gn_issue_draft_to_unsigned_json(const GnIssueDraft *draft,
                                       const GnIssueTarget *target,
                                       const gchar *pubkey);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnIssueDraft, gn_issue_draft_free)

/* Unsigned kind-1630 (open) status for a published issue. p tags: owner,
 * maintainers and pubkey (nullable), de-duplicated. */
NostrEvent *gn_issue_build_open_status(const gchar *issue_id,
                                       const GnIssueTarget *target,
                                       const gchar *pubkey);

/* Default relay rule for issue publication: a wss:// URL with a host and no
 * user name or password. Returns the trimmed URL, or NULL with
 * G_IO_ERROR_INVALID_DATA. */
gchar *gn_issue_normalize_relay_url(const gchar *url, GError **error);

G_END_DECLS
#endif /* GN_NIP34_ISSUE_H */
