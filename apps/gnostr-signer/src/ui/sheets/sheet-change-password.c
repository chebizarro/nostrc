/* sheet-change-password.c - Change password dialog
 *
 * Allows user to change their account passphrase.
 * Validates current password and ensures new password matches confirmation.
 */

#include "sheet-change-password.h"
#include "../app-resources.h"
#include "../../accounts_store.h"
#include "../../secret-storage.h"
#include <string.h>

struct _SheetChangePassword {
  AdwDialog parent_instance;

  /* Template children */
  AdwPasswordEntryRow *entry_current;
  AdwPasswordEntryRow *entry_new;
  AdwPasswordEntryRow *entry_confirm;
  AdwBanner *banner_status;
  GtkButton *btn_cancel;
  GtkButton *btn_update;

  /* State */
  char *account_id;
};

G_DEFINE_TYPE(SheetChangePassword, sheet_change_password, ADW_TYPE_DIALOG)

static void on_entry_changed(GtkEditable *editable, gpointer user_data);
static void on_btn_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_update_clicked(GtkButton *btn, gpointer user_data);

static void
sheet_change_password_dispose(GObject *object)
{
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(object);
  
  g_clear_pointer(&self->account_id, g_free);
  
  G_OBJECT_CLASS(sheet_change_password_parent_class)->dispose(object);
}

static void
sheet_change_password_class_init(SheetChangePasswordClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = sheet_change_password_dispose;

  gtk_widget_class_set_template_from_resource(widget_class,
    APP_RESOURCE_PATH "/ui/sheets/sheet-change-password.ui");

  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, entry_current);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, entry_new);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, entry_confirm);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, banner_status);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, btn_cancel);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, btn_update);
}

static void
sheet_change_password_init(SheetChangePassword *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));

  /* Connect signals */
  g_signal_connect(self->entry_current, "changed", G_CALLBACK(on_entry_changed), self);
  g_signal_connect(self->entry_new, "changed", G_CALLBACK(on_entry_changed), self);
  g_signal_connect(self->entry_confirm, "changed", G_CALLBACK(on_entry_changed), self);
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_btn_cancel_clicked), self);
  g_signal_connect(self->btn_update, "clicked", G_CALLBACK(on_btn_update_clicked), self);
}

SheetChangePassword *
sheet_change_password_new(GtkWindow *parent)
{
  SheetChangePassword *self = g_object_new(SHEET_TYPE_CHANGE_PASSWORD, NULL);
  if (parent) {
    adw_dialog_present(ADW_DIALOG(self), GTK_WIDGET(parent));
  }
  return self;
}

void
sheet_change_password_set_account(SheetChangePassword *self, const char *account_id)
{
  g_return_if_fail(SHEET_IS_CHANGE_PASSWORD(self));
  
  g_free(self->account_id);
  self->account_id = g_strdup(account_id);
}

static void
validate_and_update_button(SheetChangePassword *self)
{
  const char *current = gtk_editable_get_text(GTK_EDITABLE(self->entry_current));
  const char *new_pass = gtk_editable_get_text(GTK_EDITABLE(self->entry_new));
  const char *confirm = gtk_editable_get_text(GTK_EDITABLE(self->entry_confirm));

  gboolean valid = FALSE;
  
  /* All fields must be filled */
  if (current && *current && new_pass && *new_pass && confirm && *confirm) {
    /* New password must match confirmation */
    if (g_strcmp0(new_pass, confirm) == 0) {
      /* New password must be different from current */
      if (g_strcmp0(current, new_pass) != 0) {
        /* New password must be at least 8 characters */
        if (strlen(new_pass) >= 8) {
          valid = TRUE;
          adw_banner_set_revealed(self->banner_status, FALSE);
        } else {
          adw_banner_set_title(self->banner_status, "Password must be at least 8 characters");
          adw_banner_set_revealed(self->banner_status, TRUE);
        }
      } else {
        adw_banner_set_title(self->banner_status, "New password must be different from current password");
        adw_banner_set_revealed(self->banner_status, TRUE);
      }
    } else {
      adw_banner_set_title(self->banner_status, "Passwords do not match");
      adw_banner_set_revealed(self->banner_status, TRUE);
    }
  }

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_update), valid);
}

static void
on_entry_changed(GtkEditable *editable, gpointer user_data)
{
  (void)editable;
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(user_data);
  validate_and_update_button(self);
}

static void
on_btn_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(user_data);
  adw_dialog_close(ADW_DIALOG(self));
}

static void
on_btn_update_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(user_data);

  if (!self->account_id) {
    adw_banner_set_title(self->banner_status, "No account selected");
    adw_banner_set_revealed(self->banner_status, TRUE);
    return;
  }

  /* TODO: Implement password change using secret-storage API
   * 1. Retrieve key using gn_secret_storage_retrieve_key() with current password
   * 2. Delete old key entry
   * 3. Store key again with new password
   * For now, show placeholder message */
  
  adw_banner_set_title(self->banner_status, "Password change functionality coming soon");
  adw_banner_set_revealed(self->banner_status, TRUE);
  
  /* Close dialog after brief delay */
  g_timeout_add(2000, (GSourceFunc)adw_dialog_close, self);
}
