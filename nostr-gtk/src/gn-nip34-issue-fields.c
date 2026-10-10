#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>
#include "gn-portable-i18n-private.h"

struct _GnNip34IssueFields {
  GtkBox parent_instance;
  GtkWidget *steps, *expected, *actual, *labels, *commits, *attachments;
};
G_DEFINE_TYPE(GnNip34IssueFields, gn_nip34_issue_fields, GTK_TYPE_BOX)
static void gn_nip34_issue_fields_class_init(GnNip34IssueFieldsClass *klass) {
  (void)klass;
  gn_portable_gettext_domain();
}
static GtkWidget *row(GnNip34IssueFields *self, const char *label) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *title = gtk_label_new(label);
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), title);
  gtk_box_append(GTK_BOX(self), box);
  return box;
}
static GtkWidget *text_row(GnNip34IssueFields *self, const char *label) {
  GtkWidget *box = row(self, label);
  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_widget_set_size_request(view, -1, 70);
  gtk_box_append(GTK_BOX(box), view);
  /* The title labels the field for assistive technologies (labelled-by). */
  gtk_label_set_mnemonic_widget(GTK_LABEL(gtk_widget_get_first_child(box)), view);
  gtk_accessible_update_property(GTK_ACCESSIBLE(view), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return view;
}
static GtkWidget *entry_row(GnNip34IssueFields *self, const char *label) {
  GtkWidget *box = row(self, label);
  GtkWidget *entry = gtk_entry_new();
  gtk_box_append(GTK_BOX(box), entry);
  /* The title labels the field for assistive technologies (labelled-by). */
  gtk_label_set_mnemonic_widget(GTK_LABEL(gtk_widget_get_first_child(box)), entry);
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  return entry;
}
static void gn_nip34_issue_fields_init(GnNip34IssueFields *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 8);
  self->steps = text_row(self, _("Steps to Reproduce"));
  self->expected = text_row(self, _("Expected Result"));
  self->actual = text_row(self, _("Actual Result"));
  self->labels = entry_row(self, _("Labels (comma-separated)"));
  self->commits = entry_row(self, _("Related commits"));
  self->attachments = entry_row(self, _("Attachment URLs (manual references only)"));
  gtk_widget_set_name(self->labels, "labels_entry");
}
GnNip34IssueFields *gn_nip34_issue_fields_new(void) {
  return g_object_new(GN_TYPE_NIP34_ISSUE_FIELDS, NULL);
}
static gchar *text(GtkWidget *view) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}
void gn_nip34_issue_fields_add_label(GnNip34IssueFields *self, const char *label) {
  g_return_if_fail(GN_IS_NIP34_ISSUE_FIELDS(self));
  if (!label || !*label) return;
  const char *current = gtk_editable_get_text(GTK_EDITABLE(self->labels));
  g_auto(GStrv) parts = g_strsplit(current, ",", -1);
  for (guint i = 0; parts[i]; i++)
    if (g_str_equal(g_strstrip(parts[i]), label)) return;
  g_autofree char *trimmed = g_strstrip(g_strdup(current));
  g_autofree char *updated = *trimmed ? g_strdup_printf("%s, %s", trimmed, label) : g_strdup(label);
  gtk_editable_set_text(GTK_EDITABLE(self->labels), updated);
}
GnNip34IssueFieldsSnapshot *gn_nip34_issue_fields_snapshot(GnNip34IssueFields *self) {
  g_return_val_if_fail(GN_IS_NIP34_ISSUE_FIELDS(self), NULL);
  GnNip34IssueFieldsSnapshot *s = g_new0(GnNip34IssueFieldsSnapshot, 1);
  s->steps = text(self->steps);
  s->expected = text(self->expected);
  s->actual = text(self->actual);
  s->labels = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->labels)));
  s->related_commits = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->commits)));
  s->attachment_urls = g_strdup(gtk_editable_get_text(GTK_EDITABLE(self->attachments)));
  return s;
}
static void set_text(GtkWidget *view, const char *value) {
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), value ? value : "", -1);
}
void gn_nip34_issue_fields_set_snapshot(GnNip34IssueFields *self,
                                        const GnNip34IssueFieldsSnapshot *s) {
  g_return_if_fail(GN_IS_NIP34_ISSUE_FIELDS(self));
  set_text(self->steps, s ? s->steps : NULL);
  set_text(self->expected, s ? s->expected : NULL);
  set_text(self->actual, s ? s->actual : NULL);
  gtk_editable_set_text(GTK_EDITABLE(self->labels), s && s->labels ? s->labels : "");
  gtk_editable_set_text(GTK_EDITABLE(self->commits), s && s->related_commits ? s->related_commits : "");
  gtk_editable_set_text(GTK_EDITABLE(self->attachments), s && s->attachment_urls ? s->attachment_urls : "");
}
