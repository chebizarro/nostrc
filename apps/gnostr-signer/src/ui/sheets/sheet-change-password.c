/* sheet-change-password.c - Change password dialog
 *
 * Allows user to change their account passphrase.
 * Features:
 * - Secure password entry with GnSecureEntry widget
 * - Password strength indicator for new password
 * - Caps lock warning
 * - Validates current password and ensures new password matches confirmation
 * - Auto-clear timeout for security
 * - Secure memory handling (zeroed on destruction)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "sheet-change-password.h"
#include "../app-resources.h"
#include "../widgets/gn-secure-entry.h"
#include "../../accounts_store.h"
#include "../../secret-storage.h"
#include "../../keyboard-nav.h"
#include <string.h>

struct _SheetChangePassword {
  AdwDialog parent_instance;

  /* Template children - containers for secure entries */
  GtkBox *box_current_container;
  GtkBox *box_new_container;
  GtkBox *box_confirm_container;

  /* Secure password entries (created programmatically) */
  GnSecureEntry *secure_current;
  GnSecureEntry *secure_new;
  GnSecureEntry *secure_confirm;

  /* Feedback widgets */
  GtkLabel *lbl_password_match;
  AdwBanner *banner_status;

  /* Status widgets */
  GtkBox *box_status;
  GtkSpinner *spinner_status;
  GtkLabel *lbl_status;

  /* Buttons */
  GtkButton *btn_cancel;
  GtkButton *btn_update;

  /* State */
  char *account_id;
};

G_DEFINE_TYPE(SheetChangePassword, sheet_change_password, ADW_TYPE_DIALOG)

static void on_secure_entry_changed(GnSecureEntry *entry, gpointer user_data);
static void on_btn_cancel_clicked(GtkButton *btn, gpointer user_data);
static void on_btn_update_clicked(GtkButton *btn, gpointer user_data);

static void
set_status(SheetChangePassword *self, const gchar *message, gboolean spinning)
{
  if (!self) return;

  if (message && *message) {
    if (self->lbl_status) gtk_label_set_text(self->lbl_status, message);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, spinning);
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), TRUE);
  } else {
    if (self->box_status) gtk_widget_set_visible(GTK_WIDGET(self->box_status), FALSE);
    if (self->spinner_status) gtk_spinner_set_spinning(self->spinner_status, FALSE);
  }
}

static void
validate_and_update_button(SheetChangePassword *self)
{
  gchar *current = gn_secure_entry_get_text(self->secure_current);
  gchar *new_pass = gn_secure_entry_get_text(self->secure_new);
  gchar *confirm = gn_secure_entry_get_text(self->secure_confirm);

  gboolean valid = FALSE;
  gboolean has_current = current && *current;
  gboolean has_new = new_pass && *new_pass;
  gboolean has_confirm = confirm && *confirm;
  gboolean passwords_match = g_strcmp0(new_pass, confirm) == 0;
  gboolean password_long_enough = gn_secure_entry_meets_requirements(self->secure_new);
  gboolean different_from_current = g_strcmp0(current, new_pass) != 0;

  /* Update password match indicator */
  if (self->lbl_password_match) {
    if (has_confirm) {
      GtkWidget *match_widget = GTK_WIDGET(self->lbl_password_match);
      if (passwords_match) {
        gtk_label_set_text(self->lbl_password_match, "Passwords match");
        gtk_widget_remove_css_class(match_widget, "error");
        gtk_widget_add_css_class(match_widget, "success");
      } else {
        gtk_label_set_text(self->lbl_password_match, "Passwords do not match");
        gtk_widget_remove_css_class(match_widget, "success");
        gtk_widget_add_css_class(match_widget, "error");
      }
      gtk_widget_set_visible(match_widget, TRUE);
    } else {
      gtk_widget_set_visible(GTK_WIDGET(self->lbl_password_match), FALSE);
    }
  }

  /* All fields must be filled */
  if (has_current && has_new && has_confirm) {
    /* New password must match confirmation */
    if (passwords_match) {
      /* New password must be different from current */
      if (different_from_current) {
        /* New password must meet minimum requirements */
        if (password_long_enough) {
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
      /* Don't show banner for mismatch - the label shows it */
      adw_banner_set_revealed(self->banner_status, FALSE);
    }
  } else {
    adw_banner_set_revealed(self->banner_status, FALSE);
  }

  /* Securely clear retrieved passwords */
  gn_secure_entry_free_text(current);
  gn_secure_entry_free_text(new_pass);
  gn_secure_entry_free_text(confirm);

  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_update), valid);
}

static void
sheet_change_password_dispose(GObject *object)
{
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(object);

  /* Clear secure entries before disposal */
  if (self->secure_current) {
    gn_secure_entry_clear(self->secure_current);
  }
  if (self->secure_new) {
    gn_secure_entry_clear(self->secure_new);
  }
  if (self->secure_confirm) {
    gn_secure_entry_clear(self->secure_confirm);
  }

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

  /* Containers for secure entries */
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, box_current_container);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, box_new_container);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, box_confirm_container);

  /* Feedback widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, lbl_password_match);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, banner_status);

  /* Status widgets */
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, box_status);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, spinner_status);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, lbl_status);

  /* Buttons */
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, btn_cancel);
  gtk_widget_class_bind_template_child(widget_class, SheetChangePassword, btn_update);
}

static void
sheet_change_password_init(SheetChangePassword *self)
{
  /* Ensure GnSecureEntry type is registered */
  g_type_ensure(GN_TYPE_SECURE_ENTRY);

  gtk_widget_init_template(GTK_WIDGET(self));

  /* Create secure current password entry */
  self->secure_current = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_current, "Enter current password");
  gn_secure_entry_set_show_strength_indicator(self->secure_current, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_current, TRUE);
  gn_secure_entry_set_timeout(self->secure_current, 120); /* 2 minute timeout */

  if (self->box_current_container) {
    gtk_box_append(self->box_current_container, GTK_WIDGET(self->secure_current));
  }

  /* Create secure new password entry */
  self->secure_new = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_new, "Enter new password");
  gn_secure_entry_set_min_length(self->secure_new, 8);
  gn_secure_entry_set_show_strength_indicator(self->secure_new, TRUE);
  gn_secure_entry_set_show_caps_warning(self->secure_new, TRUE);
  gn_secure_entry_set_requirements_text(self->secure_new,
    "Use at least 8 characters with mixed case, numbers, and symbols for a strong password.");
  gn_secure_entry_set_timeout(self->secure_new, 120);

  if (self->box_new_container) {
    gtk_box_append(self->box_new_container, GTK_WIDGET(self->secure_new));
  }

  /* Create secure confirm password entry */
  self->secure_confirm = GN_SECURE_ENTRY(gn_secure_entry_new());
  gn_secure_entry_set_placeholder_text(self->secure_confirm, "Confirm new password");
  gn_secure_entry_set_min_length(self->secure_confirm, 8);
  gn_secure_entry_set_show_strength_indicator(self->secure_confirm, FALSE);
  gn_secure_entry_set_show_caps_warning(self->secure_confirm, TRUE);
  gn_secure_entry_set_timeout(self->secure_confirm, 120);

  if (self->box_confirm_container) {
    gtk_box_append(self->box_confirm_container, GTK_WIDGET(self->secure_confirm));
  }

  /* Connect secure entry change handlers */
  g_signal_connect(self->secure_current, "changed", G_CALLBACK(on_secure_entry_changed), self);
  g_signal_connect(self->secure_new, "changed", G_CALLBACK(on_secure_entry_changed), self);
  g_signal_connect(self->secure_confirm, "changed", G_CALLBACK(on_secure_entry_changed), self);

  /* Connect button handlers */
  g_signal_connect(self->btn_cancel, "clicked", G_CALLBACK(on_btn_cancel_clicked), self);
  g_signal_connect(self->btn_update, "clicked", G_CALLBACK(on_btn_update_clicked), self);

  /* Initially disable update button and hide match indicator */
  gtk_widget_set_sensitive(GTK_WIDGET(self->btn_update), FALSE);
  if (self->lbl_password_match) {
    gtk_widget_set_visible(GTK_WIDGET(self->lbl_password_match), FALSE);
  }

  /* Setup keyboard navigation (nostrc-tz8w):
   * - Focus current password entry on dialog open
   * - Update button is default (Enter activates when form is valid) */
  gn_keyboard_nav_setup_dialog(ADW_DIALOG(self),
                                GTK_WIDGET(self->secure_current),
                                GTK_WIDGET(self->btn_update));
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
on_secure_entry_changed(GnSecureEntry *entry, gpointer user_data)
{
  (void)entry;
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(user_data);
  validate_and_update_button(self);
}

static void
on_btn_cancel_clicked(GtkButton *btn, gpointer user_data)
{
  (void)btn;
  SheetChangePassword *self = SHEET_CHANGE_PASSWORD(user_data);

  /* Clear secure entries before closing */
  if (self->secure_current)
    gn_secure_entry_clear(self->secure_current);
  if (self->secure_new)
    gn_secure_entry_clear(self->secure_new);
  if (self->secure_confirm)
    gn_secure_entry_clear(self->secure_confirm);

  adw_dialog_close(ADW_DIALOG(self));
}

/* Context for async password change */
typedef struct {
  SheetChangePassword *self;
  GtkWindow *parent;
  gchar *current_password;
  gchar *new_password;
} ChangePasswordCtx;

static void
change_password_ctx_free(ChangePasswordCtx *ctx)
{
  if (!ctx) return;

  /* Securely clear passwords */
  if (ctx->current_password) {
    gn_secure_entry_free_text(ctx->current_password);
    ctx->current_password = NULL;
  }
  if (ctx->new_password) {
    gn_secure_entry_free_text(ctx->new_password);
    ctx->new_password = NULL;
  }
  g_free(ctx);
}

static void
change_password_dbus_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void)src;
  ChangePasswordCtx *ctx = (ChangePasswordCtx *)user_data;
  if (!ctx || !ctx->self) {
    change_password_ctx_free(ctx);
    return;
  }

  SheetChangePassword *self = ctx->self;
  GError *err = NULL;
  GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  /* Hide status */
  set_status(self, NULL, FALSE);

  /* Re-enable buttons */
  if (self->btn_update) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_update), TRUE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

  gboolean ok = FALSE;

  if (err) {
    const char *domain = g_quark_to_string(err->domain);
    g_warning("ChangePassword DBus error: [%s] code=%d msg=%s",
              domain ? domain : "?", err->code, err->message);

    adw_banner_set_title(self->banner_status, err->message);
    adw_banner_set_revealed(self->banner_status, TRUE);
    g_clear_error(&err);
    change_password_ctx_free(ctx);
    return;
  }

  if (ret) {
    g_variant_get(ret, "(b)", &ok);
    g_variant_unref(ret);

    if (ok) {
      /* Clear secure entries on success */
      if (self->secure_current)
        gn_secure_entry_clear(self->secure_current);
      if (self->secure_new)
        gn_secure_entry_clear(self->secure_new);
      if (self->secure_confirm)
        gn_secure_entry_clear(self->secure_confirm);

      /* Show success message briefly then close */
      adw_banner_set_title(self->banner_status, "Password changed successfully!");
      adw_banner_set_revealed(self->banner_status, TRUE);

      /* Close dialog after brief delay */
      g_timeout_add(1500, (GSourceFunc)adw_dialog_close, self);
    } else {
      adw_banner_set_title(self->banner_status, "Password change failed. Please check your current password.");
      adw_banner_set_revealed(self->banner_status, TRUE);
    }
  }

  change_password_ctx_free(ctx);
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

  gchar *current = gn_secure_entry_get_text(self->secure_current);
  gchar *new_pass = gn_secure_entry_get_text(self->secure_new);
  gchar *confirm = gn_secure_entry_get_text(self->secure_confirm);

  /* Final validation */
  if (!current || !*current) {
    adw_banner_set_title(self->banner_status, "Please enter your current password");
    adw_banner_set_revealed(self->banner_status, TRUE);
    gn_secure_entry_free_text(current);
    gn_secure_entry_free_text(new_pass);
    gn_secure_entry_free_text(confirm);
    return;
  }

  if (!new_pass || strlen(new_pass) < 8) {
    adw_banner_set_title(self->banner_status, "New password must be at least 8 characters");
    adw_banner_set_revealed(self->banner_status, TRUE);
    gn_secure_entry_free_text(current);
    gn_secure_entry_free_text(new_pass);
    gn_secure_entry_free_text(confirm);
    return;
  }

  if (g_strcmp0(new_pass, confirm) != 0) {
    adw_banner_set_title(self->banner_status, "Passwords do not match");
    adw_banner_set_revealed(self->banner_status, TRUE);
    gn_secure_entry_free_text(current);
    gn_secure_entry_free_text(new_pass);
    gn_secure_entry_free_text(confirm);
    return;
  }

  /* Securely clear confirm (we don't need it anymore) */
  gn_secure_entry_free_text(confirm);

  /* Disable buttons while processing */
  if (self->btn_update) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_update), FALSE);
  if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), FALSE);
  set_status(self, "Updating password...", TRUE);
  adw_banner_set_revealed(self->banner_status, FALSE);

  /* Get D-Bus connection */
  GError *e = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &e);
  if (!bus) {
    set_status(self, NULL, FALSE);
    if (self->btn_update) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_update), TRUE);
    if (self->btn_cancel) gtk_widget_set_sensitive(GTK_WIDGET(self->btn_cancel), TRUE);

    adw_banner_set_title(self->banner_status,
      e ? e->message : "Failed to connect to session bus");
    adw_banner_set_revealed(self->banner_status, TRUE);
    if (e) g_clear_error(&e);
    gn_secure_entry_free_text(current);
    gn_secure_entry_free_text(new_pass);
    return;
  }

  /* Create context for async call - password ownership transfers to ctx */
  ChangePasswordCtx *ctx = g_new0(ChangePasswordCtx, 1);
  ctx->self = self;
  ctx->parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(self)));
  ctx->current_password = current;  /* Transfer ownership */
  ctx->new_password = new_pass;     /* Transfer ownership */

  /* Call ChangePassword D-Bus method
   * Expected signature: (sss) -> account_id, current_password, new_password
   * Returns: (b) -> success
   */
  g_dbus_connection_call(bus,
                         "org.nostr.Signer",
                         "/org/nostr/signer",
                         "org.nostr.Signer",
                         "ChangePassword",
                         g_variant_new("(sss)", self->account_id, ctx->current_password, ctx->new_password),
                         G_VARIANT_TYPE("(b)"),
                         G_DBUS_CALL_FLAGS_NONE,
                         10000,
                         NULL,
                         change_password_dbus_done,
                         ctx);

  g_object_unref(bus);
}
