/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GnNip34IssueView: the shared "Report an Issue" form (nostrc-8xfib.5).
 *
 * Consent rules, enforced here for every host:
 *  - Nothing is resolved, uploaded, signed or published while the form is
 *    edited. The exact unsigned event is frozen at Review and shown; Publish
 *    signs that event only if the form still produces it.
 *  - Files are uploaded only after the user accepts a prompt naming the
 *    destination and every file; the final event is then reviewed again.
 *  - The repository lookup runs only through
 *    gn_nip34_issue_view_lookup_repository() (the host decides when).
 *  - Diagnostics are host-supplied text, previewed in place.
 *
 * GTK 4.6 / libadwaita 1.2 floor: the view is a GtkBox. The consent prompt is
 * an AdwAlertDialog with libadwaita >= 1.5 and an AdwMessageDialog before.
 * gn_nip34_issue_view_present() wraps the view in an AdwDialog (>= 1.5) or a
 * modal GtkWindow.
 */
#ifndef GN_NIP34_ISSUE_VIEW_H
#define GN_NIP34_ISSUE_VIEW_H

#include <gtk/gtk.h>
#include <nostr-gtk-1.0/gn-nip34-issue.h>
#include <nostr-gtk-1.0/gn-nip34-issue-iface.h>
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>

G_BEGIN_DECLS

#define GN_TYPE_NIP34_ISSUE_VIEW (gn_nip34_issue_view_get_type())
G_DECLARE_FINAL_TYPE(GnNip34IssueView, gn_nip34_issue_view, GN, NIP34_ISSUE_VIEW, GtkBox)

typedef enum {
  GN_ISSUE_DIAGNOSTICS_NONE = 0,
  GN_ISSUE_DIAGNOSTICS_DEFAULT_ON = 1 << 0, /* checkbox starts checked */
  GN_ISSUE_DIAGNOSTICS_EDITABLE = 1 << 1,   /* editable text instead of a fixed preview */
} GnIssueDiagnosticsFlags;

/* Returns the diagnostics text (transfer full), or NULL for none. Called when
 * the checkbox is turned on, so the preview is a fresh snapshot. */
typedef gchar *(*GnIssueDiagnosticsFunc)(gpointer user_data);

/* target and publisher are required; resolver and uploader may be NULL (no
 * lookup; manual attachment URLs only). */
GnNip34IssueView *gn_nip34_issue_view_new(const GnIssueTarget *target,
                                          GnIssuePublisher *publisher,
                                          GnIssueRepoResolver *resolver,
                                          GnIssueUploader *uploader);

/* The author shown in the preview. Changing it invalidates a pending review. */
void gn_nip34_issue_view_set_pubkey(GnNip34IssueView *self, const gchar *pubkey_hex);
const gchar *gn_nip34_issue_view_get_pubkey(GnNip34IssueView *self);
const GnIssueTarget *gn_nip34_issue_view_get_target(GnNip34IssueView *self);
/* The embedded structured-fields widget (e.g. to prefill labels). */
GnNip34IssueFields *gn_nip34_issue_view_get_fields(GnNip34IssueView *self);
void gn_nip34_issue_view_set_intro(GnNip34IssueView *self, const gchar *text);
/* Labels always published first (not editable), e.g. "bug". */
void gn_nip34_issue_view_set_required_labels(GnNip34IssueView *self, const gchar *const *labels);
/* Prefills the editable "Publish to Relays" row. */
void gn_nip34_issue_view_set_relays(GnNip34IssueView *self, const gchar *const *relays);
/* Without a func the diagnostics checkbox is hidden. */
void gn_nip34_issue_view_set_diagnostics(GnNip34IssueView *self, const gchar *heading,
                                         const gchar *check_label, const gchar *description,
                                         GnIssueDiagnosticsFlags flags,
                                         GnIssueDiagnosticsFunc func, gpointer user_data,
                                         GDestroyNotify destroy);
/* Candidate crash reports (GFile items), offered unchecked. Ignored without
 * an uploader. */
void gn_nip34_issue_view_set_crash_logs(GnNip34IssueView *self, GListModel *files);
/* Adds a file to the attachment list (as the "Attach Files" button does).
 * FALSE without an uploader or over GN_ISSUE_ATTACHMENT_MAX_BYTES. Nothing
 * is uploaded until the user consents. */
gboolean gn_nip34_issue_view_add_attachment(GnNip34IssueView *self, GFile *file);
/* Starts the resolver now (no-op without one). Hosts call this on an explicit
 * user action, or on open when their privacy model allows it. */
void gn_nip34_issue_view_lookup_repository(GnNip34IssueView *self);
/* Cancels in-flight work, drops any review and locks the form with message
 * (e.g. the account changed). */
void gn_nip34_issue_view_invalidate(GnNip34IssueView *self, const gchar *message);

/* Wraps the view in a dialog presented over parent; returns the container. */
GtkWidget *gn_nip34_issue_view_present(GnNip34IssueView *self, GtkWidget *parent);
/* Closes the container created by gn_nip34_issue_view_present(). */
void gn_nip34_issue_view_close(GnNip34IssueView *self);

G_END_DECLS
#endif /* GN_NIP34_ISSUE_VIEW_H */
