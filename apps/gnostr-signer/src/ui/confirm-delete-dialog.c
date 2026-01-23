/* confirm-delete-dialog.c - Confirmation dialog for destructive delete operations
 *
 * Implements a secure confirmation dialog that requires explicit user
 * acknowledgment before performing destructive operations.
 */

#include "confirm-delete-dialog.h"
#include "../session-manager.h"
#include <string.h>

struct _GnConfirmDeleteDialog {
  AdwDialog parent_instance;

  /* Widgets */
  GtkImage *warning_icon;
  GtkLabel *title_label;
  GtkLabel *message_label;
  GtkLabel *detail_label;
  GtkBox *items_box;
  GtkListBox *items_list;
  GtkBox *confirm_entry_box;
  GtkEntry *confirm_entry;
  GtkLabel *confirm_hint;
  GtkBox *password_box;
  GtkPasswordEntry *password_entry;
  GtkButton *btn_cancel;
  GtkButton *btn_delete;

  /* State */
  GnDeleteSeverity severity;
  gchar *confirm_text;
  GnConfirmDeleteCallback callback;
  gpointer user_data;
};

G_DEFINE_FINAL_TYPE(GnConfirmDeleteDialog, gn_confirm_delete_dialog, ADW_TYPE_DIALOG)

/* Forward declarations */
static void update_delete_button_sensitivity(GnConfirmDeleteDialog *self);

/* ============================================================
 * Signal Handlers
 * ============================================================ */

static void
on_confirm_entry_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  GnConfirmDeleteDialog *self = GN_CONFIRM_DELETE_DIALOG(user_data);
  update_delete_button_sensitivity(self);
}

static void
on_password_entry_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  GnConfirmDeleteDialog *self = GN_CONFIRM_DELETE_DIALOG(user_data);
  update_delete_button_sensitivity(self);
}

static void
on_cancel_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnConfirmDeleteDialog *self = GN_CONFIRM_DELETE_DIALOG(user_data);

  if (self->callback) {
    self->callback(FALSE, self->user_data);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_delete_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GnConfirmDeleteDialog *self = GN_CONFIRM_DELETE_DIALOG(user_data);

  /* For CRITICAL severity, verify password */
  if (self->severity == GN_DELETE_SEVERITY_CRITICAL) {
    const char *password = gtk_editable_get_text(GTK_EDITABLE(self->password_entry));

    GnSessionManager *sm = gn_session_manager_get_default();
    GError *error = NULL;

    /* Verify password by attempting re-authentication */
    if (!gn_session_manager_authenticate(sm, password, &error)) {
      /* Show error and don't proceed */
      gtk_widget_add_css_class(GTK_WIDGET(self->password_entry), "error");

      /* Clear password field for retry */
      gtk_editable_set_text(GTK_EDITABLE(self->password_entry), "");

      g_clear_error(&error);
      return;
    }
  }

  if (self->callback) {
    self->callback(TRUE, self->user_data);
  }

  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_dialog_closed(AdwDialog *dialog) {
  GnConfirmDeleteDialog *self = GN_CONFIRM_DELETE_DIALOG(dialog);

  /* Treat close as cancellation if not already handled */
  if (self->callback) {
    self->callback(FALSE, self->user_data);
    self->callback = NULL;
  }

  ADW_DIALOG_CLASS(gn_confirm_delete_dialog_parent_class)->closed(dialog);
}

/* ============================================================
 * Helper Functions
 * ============================================================ */

static void
update_delete_button_sensitivity(GnConfirmDeleteDialog *self) {
  gboolean can_delete = FALSE;
  const char *disabled_reason = NULL;

  switch (self->severity) {
    case GN_DELETE_SEVERITY_LOW:
      /* Always enabled for low severity */
      can_delete = TRUE;
      break;

    case GN_DELETE_SEVERITY_MEDIUM:
      /* Always enabled but with warning style */
      can_delete = TRUE;
      break;

    case GN_DELETE_SEVERITY_HIGH: {
      /* Must type confirmation text */
      const char *typed = gtk_editable_get_text(GTK_EDITABLE(self->confirm_entry));
      const char *expected = self->confirm_text ? self->confirm_text : "DELETE";
      can_delete = (g_strcmp0(typed, expected) == 0);
      if (!can_delete) {
        disabled_reason = "Delete button is disabled. Type the exact confirmation text to enable it.";
      }
      break;
    }

    case GN_DELETE_SEVERITY_CRITICAL: {
      /* Must type confirmation text AND enter password */
      const char *typed = gtk_editable_get_text(GTK_EDITABLE(self->confirm_entry));
      const char *expected = self->confirm_text ? self->confirm_text : "DELETE ALL DATA";
      const char *password = gtk_editable_get_text(GTK_EDITABLE(self->password_entry));

      gboolean text_matches = (g_strcmp0(typed, expected) == 0);
      gboolean has_password = (password && strlen(password) > 0);

      can_delete = text_matches && has_password;

      if (!can_delete) {
        if (!text_matches && !has_password) {
          disabled_reason = "Delete button is disabled. Type the confirmation text and enter your password to enable it.";
        } else if (!text_matches) {
          disabled_reason = "Delete button is disabled. Type the exact confirmation text to enable it.";
        } else {
          disabled_reason = "Delete button is disabled. Enter your password to enable it.";
        }
      }
      break;
    }
  }

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_delete), can_delete);

  /* Update accessibility state for the delete button (nostrc-qfdg) */
  if (can_delete) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_delete),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Click to confirm and proceed with the deletion",
                                   -1);
    gtk_accessible_update_state(GTK_ACCESSIBLE(self->btn_delete),
                                GTK_ACCESSIBLE_STATE_DISABLED, FALSE,
                                -1);
  } else if (disabled_reason) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_delete),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, disabled_reason,
                                   -1);
    gtk_accessible_update_state(GTK_ACCESSIBLE(self->btn_delete),
                                GTK_ACCESSIBLE_STATE_DISABLED, TRUE,
                                -1);
  }
}

static void
update_ui_for_severity(GnConfirmDeleteDialog *self) {
  /* Reset state */
  gtk_widget_set_visible(GTK_WIDGET(self->confirm_entry_box), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(self->password_box), FALSE);

  /* Remove existing CSS classes */
  gtk_widget_remove_css_class(GTK_WIDGET(self->btn_delete), "destructive-action");
  gtk_widget_remove_css_class(GTK_WIDGET(self->btn_delete), "error");

  const char *severity_desc = NULL;
  const char *icon_desc = NULL;

  switch (self->severity) {
    case GN_DELETE_SEVERITY_LOW:
      /* Simple dialog with destructive button */
      gtk_button_set_label(self->btn_delete, "Delete");
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "destructive-action");
      gtk_image_set_from_icon_name(self->warning_icon, "user-trash-symbolic");
      severity_desc = "Low severity deletion. Click Delete to proceed.";
      icon_desc = "Trash icon indicating a simple deletion";
      break;

    case GN_DELETE_SEVERITY_MEDIUM:
      /* Warning icon, destructive button */
      gtk_button_set_label(self->btn_delete, "Delete");
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "destructive-action");
      gtk_image_set_from_icon_name(self->warning_icon, "dialog-warning-symbolic");
      severity_desc = "Medium severity deletion. Please review before proceeding.";
      icon_desc = "Warning icon indicating this deletion requires attention";
      break;

    case GN_DELETE_SEVERITY_HIGH:
      /* Must type confirmation text */
      gtk_widget_set_visible(GTK_WIDGET(self->confirm_entry_box), TRUE);

      if (self->confirm_text) {
        gchar *hint = g_strdup_printf("Type \"%s\" to confirm", self->confirm_text);
        gtk_label_set_text(self->confirm_hint, hint);

        /* Update accessibility for confirm entry (nostrc-qfdg) */
        gchar *entry_desc = g_strdup_printf("Type exactly %s to enable the delete button", self->confirm_text);
        gtk_accessible_update_property(GTK_ACCESSIBLE(self->confirm_entry),
                                       GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, entry_desc,
                                       -1);
        g_free(entry_desc);
        g_free(hint);
      } else {
        gtk_label_set_text(self->confirm_hint, "Type \"DELETE\" to confirm");
        gtk_accessible_update_property(GTK_ACCESSIBLE(self->confirm_entry),
                                       GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Type exactly DELETE to enable the delete button",
                                       -1);
      }

      gtk_button_set_label(self->btn_delete, "Permanently Delete");
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "destructive-action");
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "error");
      gtk_image_set_from_icon_name(self->warning_icon, "dialog-warning-symbolic");
      severity_desc = "High severity deletion. You must type a confirmation text to proceed. This action cannot be undone.";
      icon_desc = "Warning icon indicating this deletion is permanent and requires confirmation";
      break;

    case GN_DELETE_SEVERITY_CRITICAL:
      /* Must type confirmation AND enter password */
      gtk_widget_set_visible(GTK_WIDGET(self->confirm_entry_box), TRUE);
      gtk_widget_set_visible(GTK_WIDGET(self->password_box), TRUE);

      if (self->confirm_text) {
        gchar *hint = g_strdup_printf("Type \"%s\" to confirm", self->confirm_text);
        gtk_label_set_text(self->confirm_hint, hint);

        /* Update accessibility for confirm entry (nostrc-qfdg) */
        gchar *entry_desc = g_strdup_printf("Type exactly %s to enable the delete button", self->confirm_text);
        gtk_accessible_update_property(GTK_ACCESSIBLE(self->confirm_entry),
                                       GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, entry_desc,
                                       -1);
        g_free(entry_desc);
        g_free(hint);
      } else {
        gtk_label_set_text(self->confirm_hint, "Type \"DELETE ALL DATA\" to confirm");
        gtk_accessible_update_property(GTK_ACCESSIBLE(self->confirm_entry),
                                       GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Type exactly DELETE ALL DATA to enable the delete button",
                                       -1);
      }

      gtk_button_set_label(self->btn_delete, "Permanently Delete Everything");
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "destructive-action");
      gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "error");
      gtk_image_set_from_icon_name(self->warning_icon, "dialog-error-symbolic");
      severity_desc = "Critical severity deletion. You must type a confirmation text AND enter your password to proceed. This action is irreversible and will delete all data.";
      icon_desc = "Error icon indicating this is a critical and irreversible deletion";
      break;
  }

  /* Update dialog accessibility with severity information (nostrc-qfdg) */
  if (severity_desc) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self),
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, severity_desc,
                                   -1);
  }

  /* Update warning icon accessibility (nostrc-qfdg) */
  if (icon_desc) {
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->warning_icon),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, icon_desc,
                                   -1);
  }

  update_delete_button_sensitivity(self);
}

/* ============================================================
 * GObject Implementation
 * ============================================================ */

static void
gn_confirm_delete_dialog_dispose(GObject *object) {
  GnConfirmDeleteDialog *self = GN_CONFIRM_DELETE_DIALOG(object);

  g_clear_pointer(&self->confirm_text, g_free);

  G_OBJECT_CLASS(gn_confirm_delete_dialog_parent_class)->dispose(object);
}

static void
gn_confirm_delete_dialog_class_init(GnConfirmDeleteDialogClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  AdwDialogClass *dialog_class = ADW_DIALOG_CLASS(klass);

  object_class->dispose = gn_confirm_delete_dialog_dispose;
  dialog_class->closed = on_dialog_closed;
}

static void
gn_confirm_delete_dialog_init(GnConfirmDeleteDialog *self) {
  self->severity = GN_DELETE_SEVERITY_LOW;
  self->confirm_text = NULL;
  self->callback = NULL;
  self->user_data = NULL;

  /* Build the dialog UI programmatically */
  adw_dialog_set_title(ADW_DIALOG(self), "Confirm Deletion");
  adw_dialog_set_content_width(ADW_DIALOG(self), 400);

  /* Set accessibility role and label for the dialog (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Confirmation dialog for deletion",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "A confirmation dialog requiring your acknowledgment before deleting data. This action may be irreversible.",
                                 -1);

  /* Main content box */
  GtkBox *main_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 24));
  gtk_widget_set_margin_start(GTK_WIDGET(main_box), 24);
  gtk_widget_set_margin_end(GTK_WIDGET(main_box), 24);
  gtk_widget_set_margin_top(GTK_WIDGET(main_box), 24);
  gtk_widget_set_margin_bottom(GTK_WIDGET(main_box), 24);

  /* Header with icon and title */
  GtkBox *header_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16));
  gtk_widget_set_halign(GTK_WIDGET(header_box), GTK_ALIGN_CENTER);

  self->warning_icon = GTK_IMAGE(gtk_image_new_from_icon_name("dialog-warning-symbolic"));
  gtk_image_set_pixel_size(self->warning_icon, 48);
  gtk_widget_add_css_class(GTK_WIDGET(self->warning_icon), "warning");
  gtk_box_append(header_box, GTK_WIDGET(self->warning_icon));

  GtkBox *title_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 4));
  gtk_widget_set_valign(GTK_WIDGET(title_box), GTK_ALIGN_CENTER);

  self->title_label = GTK_LABEL(gtk_label_new("Delete Item"));
  gtk_widget_add_css_class(GTK_WIDGET(self->title_label), "title-2");
  gtk_label_set_wrap(self->title_label, TRUE);
  gtk_box_append(title_box, GTK_WIDGET(self->title_label));

  self->message_label = GTK_LABEL(gtk_label_new("This action cannot be undone."));
  gtk_widget_add_css_class(GTK_WIDGET(self->message_label), "dim-label");
  gtk_label_set_wrap(self->message_label, TRUE);
  gtk_box_append(title_box, GTK_WIDGET(self->message_label));

  gtk_box_append(header_box, GTK_WIDGET(title_box));
  gtk_box_append(main_box, GTK_WIDGET(header_box));

  /* Detail label */
  self->detail_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_wrap(self->detail_label, TRUE);
  gtk_label_set_justify(self->detail_label, GTK_JUSTIFY_CENTER);
  gtk_widget_set_visible(GTK_WIDGET(self->detail_label), FALSE);
  gtk_box_append(main_box, GTK_WIDGET(self->detail_label));

  /* Items list box */
  self->items_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
  gtk_widget_set_visible(GTK_WIDGET(self->items_box), FALSE);

  GtkLabel *items_header = GTK_LABEL(gtk_label_new("Items to be deleted:"));
  gtk_widget_add_css_class(GTK_WIDGET(items_header), "heading");
  gtk_widget_set_halign(GTK_WIDGET(items_header), GTK_ALIGN_START);
  gtk_box_append(self->items_box, GTK_WIDGET(items_header));

  GtkFrame *items_frame = GTK_FRAME(gtk_frame_new(NULL));
  self->items_list = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->items_list, GTK_SELECTION_NONE);
  gtk_widget_add_css_class(GTK_WIDGET(self->items_list), "boxed-list");
  gtk_frame_set_child(items_frame, GTK_WIDGET(self->items_list));
  gtk_box_append(self->items_box, GTK_WIDGET(items_frame));

  gtk_box_append(main_box, GTK_WIDGET(self->items_box));

  /* Confirmation entry box */
  self->confirm_entry_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
  gtk_widget_set_visible(GTK_WIDGET(self->confirm_entry_box), FALSE);

  self->confirm_hint = GTK_LABEL(gtk_label_new("Type \"DELETE\" to confirm"));
  gtk_widget_add_css_class(GTK_WIDGET(self->confirm_hint), "dim-label");
  gtk_box_append(self->confirm_entry_box, GTK_WIDGET(self->confirm_hint));

  self->confirm_entry = GTK_ENTRY(gtk_entry_new());
  gtk_entry_set_placeholder_text(self->confirm_entry, "Type confirmation text here");
  gtk_widget_set_focusable(GTK_WIDGET(self->confirm_entry), TRUE);
  g_signal_connect(self->confirm_entry, "changed",
                   G_CALLBACK(on_confirm_entry_changed), self);
  gtk_box_append(self->confirm_entry_box, GTK_WIDGET(self->confirm_entry));

  /* Add accessibility for confirm entry (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->confirm_entry),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Confirmation text entry",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Type the exact confirmation text shown above to enable the delete button",
                                 -1);

  gtk_box_append(main_box, GTK_WIDGET(self->confirm_entry_box));

  /* Password entry box */
  self->password_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
  gtk_widget_set_visible(GTK_WIDGET(self->password_box), FALSE);

  GtkLabel *password_hint = GTK_LABEL(gtk_label_new("Enter your password to confirm"));
  gtk_widget_add_css_class(GTK_WIDGET(password_hint), "dim-label");
  gtk_box_append(self->password_box, GTK_WIDGET(password_hint));

  self->password_entry = GTK_PASSWORD_ENTRY(gtk_password_entry_new());
  gtk_password_entry_set_show_peek_icon(self->password_entry, TRUE);
  gtk_widget_set_focusable(GTK_WIDGET(self->password_entry), TRUE);
  g_signal_connect(self->password_entry, "changed",
                   G_CALLBACK(on_password_entry_changed), self);
  gtk_box_append(self->password_box, GTK_WIDGET(self->password_entry));

  /* Add accessibility for password entry (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->password_entry),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Password confirmation",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Enter your password to confirm this critical deletion operation",
                                 -1);

  gtk_box_append(main_box, GTK_WIDGET(self->password_box));

  /* Button box */
  GtkBox *button_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
  gtk_widget_set_halign(GTK_WIDGET(button_box), GTK_ALIGN_END);
  gtk_widget_set_margin_top(GTK_WIDGET(button_box), 12);

  self->btn_cancel = GTK_BUTTON(gtk_button_new_with_label("Cancel"));
  gtk_widget_set_focusable(GTK_WIDGET(self->btn_cancel), TRUE);
  g_signal_connect(self->btn_cancel, "clicked",
                   G_CALLBACK(on_cancel_clicked), self);
  gtk_box_append(button_box, GTK_WIDGET(self->btn_cancel));

  /* Add accessibility for cancel button (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_cancel),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Cancel deletion",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Cancel and close this dialog without deleting anything",
                                 -1);

  self->btn_delete = GTK_BUTTON(gtk_button_new_with_label("Delete"));
  gtk_widget_set_focusable(GTK_WIDGET(self->btn_delete), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->btn_delete), "destructive-action");
  g_signal_connect(self->btn_delete, "clicked",
                   G_CALLBACK(on_delete_clicked), self);
  gtk_box_append(button_box, GTK_WIDGET(self->btn_delete));

  /* Add accessibility for delete button (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->btn_delete),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Confirm deletion",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Proceed with the deletion. This action may be irreversible.",
                                 -1);

  gtk_box_append(main_box, GTK_WIDGET(button_box));

  /* Set the main box as dialog content */
  adw_dialog_set_child(ADW_DIALOG(self), GTK_WIDGET(main_box));

  /* Initial state */
  update_ui_for_severity(self);
}

/* ============================================================
 * Public API
 * ============================================================ */

GnConfirmDeleteDialog *
gn_confirm_delete_dialog_new(void) {
  return g_object_new(GN_TYPE_CONFIRM_DELETE_DIALOG, NULL);
}

void
gn_confirm_delete_dialog_set_title(GnConfirmDeleteDialog *self,
                                    const char *title) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  gtk_label_set_text(self->title_label, title ? title : "Confirm Deletion");
  adw_dialog_set_title(ADW_DIALOG(self), title ? title : "Confirm Deletion");
}

void
gn_confirm_delete_dialog_set_message(GnConfirmDeleteDialog *self,
                                      const char *message) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  gtk_label_set_text(self->message_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->message_label), message != NULL);
}

void
gn_confirm_delete_dialog_set_detail(GnConfirmDeleteDialog *self,
                                     const char *detail) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  gtk_label_set_text(self->detail_label, detail ? detail : "");
  gtk_widget_set_visible(GTK_WIDGET(self->detail_label), detail != NULL);
}

void
gn_confirm_delete_dialog_set_severity(GnConfirmDeleteDialog *self,
                                       GnDeleteSeverity severity) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  self->severity = severity;
  update_ui_for_severity(self);
}

void
gn_confirm_delete_dialog_set_confirm_text(GnConfirmDeleteDialog *self,
                                           const char *text) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  g_free(self->confirm_text);
  self->confirm_text = text ? g_strdup(text) : NULL;
  update_ui_for_severity(self);
}

void
gn_confirm_delete_dialog_set_items(GnConfirmDeleteDialog *self,
                                    const char **items) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  /* Clear existing items */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(self->items_list))) != NULL) {
    gtk_list_box_remove(self->items_list, child);
  }

  if (items == NULL || *items == NULL) {
    gtk_widget_set_visible(GTK_WIDGET(self->items_box), FALSE);
    return;
  }

  /* Add new items */
  for (const char **p = items; *p != NULL; p++) {
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), *p);
    gtk_list_box_append(self->items_list, row);
  }

  gtk_widget_set_visible(GTK_WIDGET(self->items_box), TRUE);
}

void
gn_confirm_delete_dialog_set_callback(GnConfirmDeleteDialog *self,
                                       GnConfirmDeleteCallback callback,
                                       gpointer user_data) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  self->callback = callback;
  self->user_data = user_data;
}

void
gn_confirm_delete_dialog_present(GnConfirmDeleteDialog *self,
                                  GtkWidget *parent) {
  g_return_if_fail(GN_IS_CONFIRM_DELETE_DIALOG(self));

  adw_dialog_present(ADW_DIALOG(self), parent);
}

/* ============================================================
 * Convenience Functions
 * ============================================================ */

void
gn_show_delete_key_confirmation(GtkWidget *parent,
                                 const char *npub,
                                 const char *label,
                                 GnConfirmDeleteCallback callback,
                                 gpointer user_data) {
  GnConfirmDeleteDialog *dialog = gn_confirm_delete_dialog_new();

  gchar *title = g_strdup_printf("Delete Private Key");
  gn_confirm_delete_dialog_set_title(dialog, title);
  g_free(title);

  gchar *message;
  if (label && *label) {
    message = g_strdup_printf("Are you sure you want to permanently delete the private key for \"%s\"?", label);
  } else {
    message = g_strdup("Are you sure you want to permanently delete this private key?");
  }
  gn_confirm_delete_dialog_set_message(dialog, message);
  g_free(message);

  gn_confirm_delete_dialog_set_detail(dialog,
    "This action cannot be undone. The private key will be securely wiped "
    "and cannot be recovered unless you have a backup.");

  gn_confirm_delete_dialog_set_severity(dialog, GN_DELETE_SEVERITY_HIGH);

  /* Truncate npub for display as confirmation text */
  if (npub && strlen(npub) > 12) {
    gchar *short_npub = g_strndup(npub + 5, 8);  /* Skip "npub1" and take 8 chars */
    gn_confirm_delete_dialog_set_confirm_text(dialog, short_npub);
    g_free(short_npub);
  } else {
    gn_confirm_delete_dialog_set_confirm_text(dialog, "DELETE");
  }

  gn_confirm_delete_dialog_set_callback(dialog, callback, user_data);
  gn_confirm_delete_dialog_present(dialog, parent);
}

void
gn_show_delete_backup_confirmation(GtkWidget *parent,
                                    const char *filepath,
                                    GnConfirmDeleteCallback callback,
                                    gpointer user_data) {
  GnConfirmDeleteDialog *dialog = gn_confirm_delete_dialog_new();

  gn_confirm_delete_dialog_set_title(dialog, "Delete Backup File");

  gchar *basename = g_path_get_basename(filepath);
  gchar *message = g_strdup_printf("Delete backup file \"%s\"?", basename);
  gn_confirm_delete_dialog_set_message(dialog, message);
  g_free(message);
  g_free(basename);

  gn_confirm_delete_dialog_set_detail(dialog,
    "The backup file will be securely wiped to prevent recovery.");

  gn_confirm_delete_dialog_set_severity(dialog, GN_DELETE_SEVERITY_MEDIUM);
  gn_confirm_delete_dialog_set_callback(dialog, callback, user_data);
  gn_confirm_delete_dialog_present(dialog, parent);
}

void
gn_show_delete_session_confirmation(GtkWidget *parent,
                                     const char *client_name,
                                     GnConfirmDeleteCallback callback,
                                     gpointer user_data) {
  GnConfirmDeleteDialog *dialog = gn_confirm_delete_dialog_new();

  gn_confirm_delete_dialog_set_title(dialog, "Revoke Session");

  gchar *message = g_strdup_printf("Revoke session for \"%s\"?",
                                    client_name ? client_name : "Unknown Client");
  gn_confirm_delete_dialog_set_message(dialog, message);
  g_free(message);

  gn_confirm_delete_dialog_set_detail(dialog,
    "The application will need to request permission again to sign events.");

  gn_confirm_delete_dialog_set_severity(dialog, GN_DELETE_SEVERITY_LOW);
  gn_confirm_delete_dialog_set_callback(dialog, callback, user_data);
  gn_confirm_delete_dialog_present(dialog, parent);
}

void
gn_show_wipe_all_data_confirmation(GtkWidget *parent,
                                    GnConfirmDeleteCallback callback,
                                    gpointer user_data) {
  GnConfirmDeleteDialog *dialog = gn_confirm_delete_dialog_new();

  gn_confirm_delete_dialog_set_title(dialog, "Wipe All Data");

  gn_confirm_delete_dialog_set_message(dialog,
    "This will permanently delete ALL gnostr-signer data.");

  gn_confirm_delete_dialog_set_detail(dialog,
    "WARNING: This includes all configuration, cached profiles, session data, "
    "and any locally stored files. Private keys stored in your system keychain "
    "will NOT be deleted.\n\n"
    "This action is IRREVERSIBLE.");

  const char *items[] = {
    "All configuration files",
    "All cached data",
    "All session information",
    "All policy settings",
    "All backup files",
    "All log files",
    NULL
  };
  gn_confirm_delete_dialog_set_items(dialog, items);

  gn_confirm_delete_dialog_set_severity(dialog, GN_DELETE_SEVERITY_CRITICAL);
  gn_confirm_delete_dialog_set_confirm_text(dialog, "DELETE ALL DATA");
  gn_confirm_delete_dialog_set_callback(dialog, callback, user_data);
  gn_confirm_delete_dialog_present(dialog, parent);
}

void
gn_show_delete_logs_confirmation(GtkWidget *parent,
                                  guint log_count,
                                  GnConfirmDeleteCallback callback,
                                  gpointer user_data) {
  GnConfirmDeleteDialog *dialog = gn_confirm_delete_dialog_new();

  gn_confirm_delete_dialog_set_title(dialog, "Delete Log Files");

  gchar *message = g_strdup_printf("Delete %u log file%s?",
                                    log_count, log_count == 1 ? "" : "s");
  gn_confirm_delete_dialog_set_message(dialog, message);
  g_free(message);

  gn_confirm_delete_dialog_set_detail(dialog,
    "Log files will be securely wiped to remove any sensitive data they may contain.");

  gn_confirm_delete_dialog_set_severity(dialog, GN_DELETE_SEVERITY_LOW);
  gn_confirm_delete_dialog_set_callback(dialog, callback, user_data);
  gn_confirm_delete_dialog_present(dialog, parent);
}
