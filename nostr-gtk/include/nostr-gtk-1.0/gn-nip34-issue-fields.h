#ifndef GN_NIP34_ISSUE_FIELDS_H
#define GN_NIP34_ISSUE_FIELDS_H
#include "nostr-gtk-portable-export.h"
#include <gtk/gtk.h>
/* GnNip34IssueFieldsSnapshot is defined with the GTK-free issue model. */
#include <nostr-gtk-1.0/gn-nip34-issue.h>
G_BEGIN_DECLS

#define GN_TYPE_NIP34_ISSUE_FIELDS (gn_nip34_issue_fields_get_type())
NOSTR_GTK_PORTABLE_API G_DECLARE_FINAL_TYPE(GnNip34IssueFields, gn_nip34_issue_fields, GN, NIP34_ISSUE_FIELDS, GtkBox)
NOSTR_GTK_PORTABLE_API GnNip34IssueFields *gn_nip34_issue_fields_new(void);
NOSTR_GTK_PORTABLE_API GnNip34IssueFieldsSnapshot *gn_nip34_issue_fields_snapshot(GnNip34IssueFields *self);
NOSTR_GTK_PORTABLE_API void gn_nip34_issue_fields_set_snapshot(GnNip34IssueFields *self,
                                        const GnNip34IssueFieldsSnapshot *snapshot);
/* Appends label to the comma-separated labels entry unless already present.
 * Since 1.2. */
NOSTR_GTK_PORTABLE_API void gn_nip34_issue_fields_add_label(GnNip34IssueFields *self, const char *label);
G_END_DECLS
#endif
