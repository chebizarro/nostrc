#ifndef GN_NIP34_ISSUE_FIELDS_H
#define GN_NIP34_ISSUE_FIELDS_H
#include <gtk/gtk.h>
G_BEGIN_DECLS

typedef struct {
  gchar *steps;
  gchar *expected;
  gchar *actual;
  gchar *labels;          /* comma-separated, caller validates/publishes */
  gchar *related_commits; /* manual Git object IDs, body text only */
  gchar *attachment_urls; /* manual text references; never uploaded */
} GnNip34IssueFieldsSnapshot;
void gn_nip34_issue_fields_snapshot_free(GnNip34IssueFieldsSnapshot *snapshot);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNip34IssueFieldsSnapshot, gn_nip34_issue_fields_snapshot_free)

#define GN_TYPE_NIP34_ISSUE_FIELDS (gn_nip34_issue_fields_get_type())
G_DECLARE_FINAL_TYPE(GnNip34IssueFields, gn_nip34_issue_fields, GN, NIP34_ISSUE_FIELDS, GtkBox)
GnNip34IssueFields *gn_nip34_issue_fields_new(void);
GnNip34IssueFieldsSnapshot *gn_nip34_issue_fields_snapshot(GnNip34IssueFields *self);
void gn_nip34_issue_fields_set_snapshot(GnNip34IssueFields *self,
                                        const GnNip34IssueFieldsSnapshot *snapshot);
G_END_DECLS
#endif
