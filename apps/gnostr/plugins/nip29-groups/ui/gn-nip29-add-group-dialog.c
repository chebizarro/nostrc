/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-add-group-dialog.c - Dialog for tracking or creating a NIP-29 group
 */

#include "gn-nip29-add-group-dialog.h"

struct _GnNip29AddGroupDialog
{
  AdwDialog parent_instance;

  GnNip29GroupService *service;
  GCancellable        *cancellable;
  gboolean             create_mode;
  gboolean             submitting;

  GtkEntry       *relay_entry;
  GtkEntry       *group_id_entry;
  GtkEntry       *alias_entry;
  GtkEntry       *about_entry;
  GtkEntry       *picture_entry;
  GtkCheckButton *private_check;
  GtkCheckButton *restricted_check;
  GtkCheckButton *hidden_check;
  GtkCheckButton *closed_check;
  GtkWidget      *metadata_section;
  GtkButton      *add_button;
  GtkLabel       *status_label;
};

G_DEFINE_TYPE(GnNip29AddGroupDialog, gn_nip29_add_group_dialog, ADW_TYPE_DIALOG)

static void
update_add_sensitivity(GnNip29AddGroupDialog *self)
{
  const char *relay = gtk_editable_get_text(GTK_EDITABLE(self->relay_entry));
  const char *gid = gtk_editable_get_text(GTK_EDITABLE(self->group_id_entry));

  gboolean ok = !self->submitting &&
                relay != NULL && relay[0] != '\0' &&
                gid != NULL && gid[0] != '\0';
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_button), ok);
}

static void
set_status(GnNip29AddGroupDialog *self,
           const char            *message,
           gboolean               is_error)
{
  gtk_label_set_text(self->status_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->status_label),
                         message != NULL && message[0] != '\0');
  if (is_error)
    gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->status_label), "error");
}

static void
on_entry_changed(GtkEditable *editable, gpointer user_data)
{
  (void)editable;
  update_add_sensitivity(GN_NIP29_ADD_GROUP_DIALOG(user_data));
}

static const char *
entry_text_or_null(GtkEntry *entry)
{
  const char *text = gtk_editable_get_text(GTK_EDITABLE(entry));
  return (text != NULL && text[0] != '\0') ? text : NULL;
}

static void
on_create_group_done(GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  (void)source;
  GnNip29AddGroupDialog *self = GN_NIP29_ADD_GROUP_DIALOG(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_create_group_finish(self->service,
                                                           result,
                                                           &error);
  self->submitting = FALSE;
  g_clear_object(&self->cancellable);

  if (ok)
    {
      adw_dialog_close(ADW_DIALOG(self));
    }
  else
    {
      set_status(self, error ? error->message : "Failed to create group", TRUE);
      gtk_button_set_label(self->add_button, "Create Group");
      update_add_sensitivity(self);
    }

  g_object_unref(self);
}

static void
on_add_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnNip29AddGroupDialog *self = GN_NIP29_ADD_GROUP_DIALOG(user_data);

  const char *relay = gtk_editable_get_text(GTK_EDITABLE(self->relay_entry));
  const char *gid = gtk_editable_get_text(GTK_EDITABLE(self->group_id_entry));
  const char *alias = entry_text_or_null(self->alias_entry);

  if (self->create_mode)
    {
      self->submitting = TRUE;
      g_clear_object(&self->cancellable);
      self->cancellable = g_cancellable_new();
      gtk_button_set_label(self->add_button, "Creating…");
      update_add_sensitivity(self);
      set_status(self, "Signing and publishing create-group…", FALSE);

      gn_nip29_group_service_create_group_async(
        self->service,
        relay,
        gid,
        alias,
        entry_text_or_null(self->about_entry),
        entry_text_or_null(self->picture_entry),
        gtk_check_button_get_active(self->private_check),
        gtk_check_button_get_active(self->restricted_check),
        gtk_check_button_get_active(self->hidden_check),
        gtk_check_button_get_active(self->closed_check),
        self->cancellable,
        on_create_group_done,
        g_object_ref(self));
      return;
    }

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_track_group(self->service,
                                                   relay, gid, alias,
                                                   &error);
  if (ok)
    {
      adw_dialog_close(ADW_DIALOG(self));
    }
  else
    {
      set_status(self, error ? error->message : "Failed to add group", TRUE);
    }
}

static void
gn_nip29_add_group_dialog_dispose(GObject *object)
{
  GnNip29AddGroupDialog *self = GN_NIP29_ADD_GROUP_DIALOG(object);
  if (self->cancellable != NULL)
    g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  g_clear_object(&self->service);
  G_OBJECT_CLASS(gn_nip29_add_group_dialog_parent_class)->dispose(object);
}

static void
gn_nip29_add_group_dialog_class_init(GnNip29AddGroupDialogClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_add_group_dialog_dispose;
}

static GtkWidget *
label_new_heading(const char *text)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_add_css_class(label, "heading");
  return label;
}

static void
gn_nip29_add_group_dialog_init(GnNip29AddGroupDialog *self)
{
  adw_dialog_set_content_width(ADW_DIALOG(self), 420);
  adw_dialog_set_content_height(ADW_DIALOG(self), 520);

  GtkWidget *toolbar_view = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header);

  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 24);
  gtk_widget_set_margin_end(content, 24);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);

  self->relay_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->relay_entry, "wss://relay.example.com");
  g_signal_connect(self->relay_entry, "changed",
                   G_CALLBACK(on_entry_changed), self);
  gtk_box_append(GTK_BOX(content), label_new_heading("Relay URL"));
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->relay_entry));

  self->group_id_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->group_id_entry, "general");
  g_signal_connect(self->group_id_entry, "changed",
                   G_CALLBACK(on_entry_changed), self);
  gtk_box_append(GTK_BOX(content), label_new_heading("Group ID"));
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->group_id_entry));

  self->alias_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->alias_entry, "Optional display name");
  gtk_box_append(GTK_BOX(content), label_new_heading("Name or Alias"));
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->alias_entry));

  self->metadata_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_visible(self->metadata_section, FALSE);

  self->about_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->about_entry, "Optional description");
  gtk_box_append(GTK_BOX(self->metadata_section), label_new_heading("About"));
  gtk_box_append(GTK_BOX(self->metadata_section), GTK_WIDGET(self->about_entry));

  self->picture_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->picture_entry, "Optional image URL");
  gtk_box_append(GTK_BOX(self->metadata_section), label_new_heading("Picture"));
  gtk_box_append(GTK_BOX(self->metadata_section), GTK_WIDGET(self->picture_entry));

  self->private_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Private (members only can read)"));
  self->restricted_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Restricted (members only can write)"));
  self->hidden_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Hidden metadata"));
  self->closed_check = GTK_CHECK_BUTTON(gtk_check_button_new_with_label("Closed to join requests"));
  gtk_box_append(GTK_BOX(self->metadata_section), GTK_WIDGET(self->private_check));
  gtk_box_append(GTK_BOX(self->metadata_section), GTK_WIDGET(self->restricted_check));
  gtk_box_append(GTK_BOX(self->metadata_section), GTK_WIDGET(self->hidden_check));
  gtk_box_append(GTK_BOX(self->metadata_section), GTK_WIDGET(self->closed_check));
  gtk_box_append(GTK_BOX(content), self->metadata_section);

  self->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "error");
  gtk_label_set_wrap(self->status_label, TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), FALSE);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->status_label));

  self->add_button = GTK_BUTTON(gtk_button_new_with_label("Track Group"));
  gtk_widget_add_css_class(GTK_WIDGET(self->add_button), "suggested-action");
  gtk_widget_add_css_class(GTK_WIDGET(self->add_button), "pill");
  gtk_widget_set_halign(GTK_WIDGET(self->add_button), GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(GTK_WIDGET(self->add_button), 8);
  gtk_widget_set_sensitive(GTK_WIDGET(self->add_button), FALSE);
  g_signal_connect(self->add_button, "clicked",
                   G_CALLBACK(on_add_clicked), self);
  gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->add_button));

  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), content);
  adw_dialog_set_child(ADW_DIALOG(self), toolbar_view);
}

static void
configure_mode(GnNip29AddGroupDialog *self,
               gboolean               create_mode)
{
  self->create_mode = create_mode;
  adw_dialog_set_title(ADW_DIALOG(self), create_mode ? "Create Group" : "Track Group");
  gtk_button_set_label(self->add_button, create_mode ? "Create Group" : "Track Group");
  gtk_widget_set_visible(self->metadata_section, create_mode);
}

GnNip29AddGroupDialog *
gn_nip29_add_group_dialog_new(GnNip29GroupService *service)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);

  GnNip29AddGroupDialog *self = g_object_new(GN_TYPE_NIP29_ADD_GROUP_DIALOG, NULL);
  self->service = g_object_ref(service);
  configure_mode(self, FALSE);
  return self;
}

GnNip29AddGroupDialog *
gn_nip29_add_group_dialog_new_create(GnNip29GroupService *service)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);

  GnNip29AddGroupDialog *self = g_object_new(GN_TYPE_NIP29_ADD_GROUP_DIALOG, NULL);
  self->service = g_object_ref(service);
  configure_mode(self, TRUE);
  return self;
}
